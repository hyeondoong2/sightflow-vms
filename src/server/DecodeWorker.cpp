#include "DecodeWorker.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include "AvError.h"
#include "AvRaii.h"
#include "Decoder.h"
#include "RtspSource.h"

DecodeWorker::DecodeWorker(std::string url, DecodeMetrics& metrics)
    : url_(std::move(url))
    , metrics_(metrics)
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
    stopRequested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DecodeWorker::run()
{
    metrics_.setState(DecodeMetrics::State::Connecting);

    std::string sourceError;
    std::unique_ptr<RtspSource> source = RtspSource::open(url_, stopRequested_, sourceError);
    if (!source) {
        if (stopRequested_.load()) {
            metrics_.setState(DecodeMetrics::State::Stopped);
        } else {
            metrics_.setError(sourceError);
        }
        return;
    }

    const int videoStreamIndex = source->videoStreamIndex();

    std::string decoderError;
    std::unique_ptr<Decoder> decoder = Decoder::create(source->videoCodecParameters(), decoderError);
    if (!decoder) {
        metrics_.setError(decoderError);
        return;
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        metrics_.setError("Failed to allocate AVPacket/AVFrame");
        return;
    }

    metrics_.setState(DecodeMetrics::State::Running);

    bool errorOccurred = false;
    bool fatalError = false;
    while (!stopRequested_.load() && !fatalError) {
        int ret = source->readPacket(packet.get());
        if (ret < 0) {
            if (!stopRequested_.load()) {
                metrics_.setError(std::string("stream ended or read error: ") + avErrorToString(ret));
                errorOccurred = true;
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
            metrics_.setError(std::string("avcodec_send_packet failed: ") + avErrorToString(ret));
            errorOccurred = true;
            break;
        }

        // Drain every frame this packet made available (0, 1, or more).
        while (!stopRequested_.load()) {
            ret = decoder->receiveFrame(frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break; // needs another packet, or stream ended
            }
            if (ret < 0) {
                metrics_.setError(std::string("avcodec_receive_frame failed: ") + avErrorToString(ret));
                errorOccurred = true;
                fatalError = true;
                break;
            }

            metrics_.recordFrame(frame->width, frame->height);
            av_frame_unref(frame.get()); // must be clean before the next receiveFrame call
        }
    }

    if (!errorOccurred) {
        metrics_.setState(DecodeMetrics::State::Stopped);
    }
}
