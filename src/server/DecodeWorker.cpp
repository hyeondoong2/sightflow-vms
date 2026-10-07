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
#include "RtspSource.h"

DecodeWorker::DecodeWorker(std::string url, DecodeMetrics& metrics, ChangeEventLog& changeEventLog)
    : url_(std::move(url))
    , metrics_(metrics)
    , changeEventLog_(changeEventLog)
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
                // pointer into it; only a ratio, if any, crosses out.
                if (const std::optional<double> changeRatio = changeDetector.processFrame(frame.get())) {
                    changeEventLog_.record(*changeRatio);
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
