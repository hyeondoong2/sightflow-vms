#include "DecodeWorker.h"

#include <iostream>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include "AvError.h"
#include "AvRaii.h"
#include "ChangeDetector.h"
#include "Decoder.h"
#include "EventStore.h"
#include "RtspSource.h"
#include "SnapshotEncoder.h"

DecodeWorker::DecodeWorker(std::string url, std::string channelName, DecodeMetrics& metrics,
    ChangeEventLog& changeEventLog, QString dbFilePath, std::size_t eventCapacity)
    : url_(std::move(url))
    , channelName_(std::move(channelName))
    , metrics_(metrics)
    , changeEventLog_(changeEventLog)
    , dbFilePath_(std::move(dbFilePath))
    , eventCapacity_(eventCapacity)
{
}

DecodeWorker::~DecodeWorker()
{
    stop();
}

void DecodeWorker::start()
{
    stopRequested_ = false;
    thread_ = std::thread(&DecodeWorker::run, this);
}

void DecodeWorker::stop()
{
    {
        // Setting the flag while holding the same mutex waitBeforeRetry()
        // holds throughout its check-then-wait sequence closes the classic
        // lost-wakeup race: without this lock, this store could land in the
        // narrow window between the waiter checking the predicate (still
        // false) and actually registering as waiting, in which case
        // notify_all() below would reach no one yet asleep and the retry
        // wait would only end once its full timeout elapsed (bounded, but
        // not the "ends immediately" this is supposed to guarantee).
        std::lock_guard<std::mutex> lock(waitMutex_);
        stopRequested_ = true;
    }
    waitCv_.notify_all(); // wake an in-progress retry-backoff wait immediately
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool DecodeWorker::waitBeforeRetry()
{
    std::unique_lock<std::mutex> lock(waitMutex_);
    waitCv_.wait_for(lock, std::chrono::milliseconds(kRetryIntervalMs), [this] { return stopRequested_.load(); });
    return !stopRequested_.load();
}

bool DecodeWorker::retryAfterFailure(const std::string& message, int& consecutiveFailures)
{
    ++consecutiveFailures;
    metrics_.setRetrying(message);

    // Log the first failure immediately, then only every Nth after that --
    // a dead source can otherwise fill the console with an identical line
    // every kRetryIntervalMs forever.
    if (consecutiveFailures == 1 || consecutiveFailures % kLogEveryNFailures == 0) {
        // Built into one string and written with a single `<<` call: with
        // two DecodeWorkers (one per fixed channel, D21) logging from two
        // different threads, a chained `std::cerr << a << b << c` here was
        // observed to interleave character-by-character with the other
        // worker's chained output onto the same stream -- a single write
        // keeps one worker's line intact regardless of what the other
        // worker logs at the same moment. `url_` is included so a reader
        // can tell which channel a line belongs to even for error messages
        // that don't otherwise mention it (unlike the "failed to open"
        // message, which already embeds the URL itself).
        std::ostringstream line;
        line << "DecodeWorker[" << url_ << "]: attempt " << consecutiveFailures << " failed (" << message
             << "), retrying in " << (kRetryIntervalMs / 1000) << "s\n";
        std::cerr << line.str();
    }

    return waitBeforeRetry();
}

void DecodeWorker::run()
{
    // Opened once for this thread's whole lifetime (independent of how many
    // times the RTSP session below reconnects) -- never shared with any
    // other thread (D24: a QtSql connection must be used only from the
    // thread that created it). A connection name per channel keeps this
    // distinct from the other channel's DecodeWorker thread's own
    // connection, and from the one-time startup-load connection main()
    // already closed before this thread was started.
    QString eventStoreError;
    std::unique_ptr<EventStore> eventStore =
        EventStore::open(QStringLiteral("events-") + QString::fromStdString(channelName_), dbFilePath_, eventStoreError);
    if (!eventStore) {
        std::cerr << "DecodeWorker[" << url_ << "]: persistence unavailable (" << eventStoreError.toStdString()
                   << ") -- this channel's events will not survive a server restart\n";
    }

    int consecutiveFailures = 0;

    while (!stopRequested_.load()) {
        metrics_.setState(DecodeMetrics::State::Connecting);

        std::string sourceError;
        std::unique_ptr<RtspSource> source = RtspSource::open(url_, stopRequested_, sourceError);
        if (!source) {
            if (stopRequested_.load()) {
                break;
            }
            if (!retryAfterFailure(sourceError, consecutiveFailures)) {
                break;
            }
            continue;
        }

        const int videoStreamIndex = source->videoStreamIndex();

        std::string decoderError;
        std::unique_ptr<Decoder> decoder = Decoder::create(source->videoCodecParameters(), decoderError);
        if (!decoder) {
            if (!retryAfterFailure(decoderError, consecutiveFailures)) {
                break;
            }
            continue;
        }

        PacketPtr packet(av_packet_alloc());
        FramePtr frame(av_frame_alloc());
        if (!packet || !frame) {
            if (!retryAfterFailure("Failed to allocate AVPacket/AVFrame", consecutiveFailures)) {
                break;
            }
            continue;
        }

        // Connected: reset per-session counters (D17) and the failure
        // streak now that an attempt has actually succeeded. A fresh
        // ChangeDetector per session (D22) means its comparison baseline is
        // always empty right after a (re)connect -- this session's first
        // frame is only ever used to establish that baseline, never
        // compared against a previous session's, so it can never itself be
        // reported as a change.
        consecutiveFailures = 0;
        metrics_.beginRunning();
        ChangeDetector changeDetector;

        bool sessionFailed = false;
        std::string sessionError;
        while (!stopRequested_.load()) {
            int ret = source->readPacket(packet.get());
            if (ret < 0) {
                if (!stopRequested_.load()) {
                    sessionFailed = true;
                    sessionError = std::string("stream ended or read error: ") + avErrorToString(ret);
                }
                break;
            }

            if (packet->stream_index != videoStreamIndex) {
                av_packet_unref(packet.get());
                continue;
            }

            ret = decoder->sendPacket(packet.get());
            av_packet_unref(packet.get()); // packet's data is copied into the decoder; released every code path

            if (ret < 0) {
                sessionFailed = true;
                sessionError = std::string("avcodec_send_packet failed: ") + avErrorToString(ret);
                break;
            }

            // Drain every frame this packet made available (0, 1, or more).
            bool fatalInnerError = false;
            while (!stopRequested_.load()) {
                ret = decoder->receiveFrame(frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break; // needs another packet, or stream ended
                }
                if (ret < 0) {
                    sessionFailed = true;
                    sessionError = std::string("avcodec_receive_frame failed: ") + avErrorToString(ret);
                    fatalInnerError = true;
                    break;
                }

                metrics_.recordFrame(frame->width, frame->height);

                // Reads frame->data/linesize while the frame is still valid
                // (before unref below) -- never stores the AVFrame or a
                // pointer into it; only a ratio, and now a small encoded
                // JPEG snapshot, if any, crosses out.
                if (const std::optional<double> changeRatio = changeDetector.processFrame(frame.get())) {
                    // The snapshot is encoded only now, at the moment an
                    // event is confirmed -- never once per decoded frame
                    // (docs/DECISIONS.md D23). A failed encode still records
                    // the event (the ratio is always known); it is simply
                    // recorded with no retrievable image, never a
                    // fabricated/placeholder one.
                    std::vector<uint8_t> snapshotJpeg;
                    std::string snapshotError;
                    if (std::optional<std::vector<uint8_t>> encoded = encodeJpegSnapshot(frame.get(), snapshotError)) {
                        snapshotJpeg = std::move(*encoded);
                    }

                    // record() takes its JPEG argument by value, so passing
                    // the lvalue `snapshotJpeg` here copies into it (moving
                    // only that copy into ChangeEventLog's own storage) --
                    // the original stays valid below for EventStore, which
                    // needs the same bytes persisted (D24).
                    const ChangeEvent recorded = changeEventLog_.record(*changeRatio, snapshotJpeg);

                    if (eventStore) {
                        QString persistError;
                        if (!eventStore->appendAndPrune(
                                QString::fromStdString(channelName_), recorded, snapshotJpeg, eventCapacity_, persistError)) {
                            // Not rate-limited like retryAfterFailure's
                            // logging: events are already cooldown-limited
                            // to roughly one per few seconds (D22), so even
                            // a persistently broken disk cannot flood the
                            // console here. The event itself is still fully
                            // usable this run -- it is already in
                            // changeEventLog_ above; only its survival past
                            // a restart is lost.
                            std::cerr << "DecodeWorker[" << url_ << "]: failed to persist event " << recorded.id
                                       << " (" << persistError.toStdString() << ")\n";
                        }
                    }
                }

                av_frame_unref(frame.get()); // must be clean before the next receiveFrame call
            }
            if (fatalInnerError) {
                break;
            }
        }

        // `source`/`decoder`/`packet`/`frame` go out of scope here, at the
        // end of this iteration's block -- RAII releases every FFmpeg
        // resource from this connection attempt before the next iteration
        // (if any) allocates fresh ones. Nothing from this session is ever
        // reused across a retry.

        if (stopRequested_.load()) {
            break;
        }
        if (sessionFailed && !retryAfterFailure(sessionError, consecutiveFailures)) {
            break;
        }
    }

    metrics_.setState(DecodeMetrics::State::Stopped);
}
