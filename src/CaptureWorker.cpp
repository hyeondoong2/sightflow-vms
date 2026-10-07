#include "CaptureWorker.h"

#include <iostream>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include "AvError.h"
#include "AvRaii.h"
#include "Decoder.h"
#include "FrameConverter.h"
#include "RtspSource.h"
#include "VideoFrame.h"

CaptureWorker::CaptureWorker(std::string url, FrameQueue& queue, CaptureState& captureState)
    : url_(std::move(url))
    , queue_(queue)
    , captureState_(captureState)
{
}

CaptureWorker::~CaptureWorker()
{
    stop();
}

void CaptureWorker::start()
{
    stopRequested_ = false;
    thread_ = std::thread(&CaptureWorker::run, this);
}

void CaptureWorker::stop()
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

bool CaptureWorker::waitBeforeRetry()
{
    std::unique_lock<std::mutex> lock(waitMutex_);
    waitCv_.wait_for(lock, std::chrono::milliseconds(kRetryIntervalMs), [this] { return stopRequested_.load(); });
    return !stopRequested_.load();
}

bool CaptureWorker::retryAfterFailure(const std::string& message, int& consecutiveFailures)
{
    ++consecutiveFailures;
    captureState_.setState(CaptureState::State::Retrying);
    queue_.clear(); // nothing from the ended session should ever be displayed as current

    // Log the first failure immediately, then only every Nth after that --
    // a dead source can otherwise fill the console with an identical line
    // every kRetryIntervalMs forever.
    if (consecutiveFailures == 1 || consecutiveFailures % kLogEveryNFailures == 0) {
        std::cerr << "CaptureWorker: attempt " << consecutiveFailures << " failed (" << message
                   << "), retrying in " << (kRetryIntervalMs / 1000) << "s" << std::endl;
    }

    return waitBeforeRetry();
}

void CaptureWorker::run()
{
    int consecutiveFailures = 0;

    while (!stopRequested_.load()) {
        captureState_.setState(CaptureState::State::Connecting);

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

        FrameConverter converter;

        // Connected: the failure streak resets now that an attempt has
        // actually succeeded.
        if (consecutiveFailures > 0) {
            std::cout << "CaptureWorker: connected after " << consecutiveFailures << " failed attempt(s)."
                       << std::endl;
        }
        consecutiveFailures = 0;
        captureState_.setState(CaptureState::State::Running);

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

                std::string convertError;
                std::optional<VideoFrame> videoFrame = converter.convert(frame.get(), convertError);

                av_frame_unref(frame.get()); // must be clean before the next receiveFrame call;
                                              // AVFrame is no longer needed once converted

                if (!videoFrame) {
                    sessionFailed = true;
                    sessionError = "failed to convert frame: " + convertError;
                    fatalInnerError = true;
                    break;
                }

                queue_.push(std::move(*videoFrame));
            }
            if (fatalInnerError) {
                break;
            }
        }

        // `source`/`decoder`/`packet`/`frame`/`converter` go out of scope
        // here, at the end of this iteration's block -- RAII releases
        // every FFmpeg resource from this connection attempt before the
        // next iteration (if any) allocates fresh ones. Nothing from this
        // session is ever reused across a retry.

        if (stopRequested_.load()) {
            break;
        }
        if (sessionFailed && !retryAfterFailure(sessionError, consecutiveFailures)) {
            break;
        }
    }

    queue_.clear();
    captureState_.setState(CaptureState::State::Stopped);
}
