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

CaptureWorker::CaptureWorker(std::string url, FrameQueue& queue)
    : url_(std::move(url))
    , queue_(queue)
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
    stopRequested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void CaptureWorker::run()
{
    std::string sourceError;
    std::unique_ptr<RtspSource> source = RtspSource::open(url_, stopRequested_, sourceError);
    if (!source) {
        if (stopRequested_.load()) {
            std::cout << "CaptureWorker: stop requested while connecting." << std::endl;
        } else {
            std::cerr << "CaptureWorker: " << sourceError << std::endl;
        }
        return;
    }

    const int videoStreamIndex = source->videoStreamIndex();

    std::string decoderError;
    std::unique_ptr<Decoder> decoder = Decoder::create(source->videoCodecParameters(), decoderError);
    if (!decoder) {
        std::cerr << "CaptureWorker: failed to create decoder: " << decoderError << std::endl;
        return;
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        std::cerr << "CaptureWorker: failed to allocate AVPacket/AVFrame" << std::endl;
        return;
    }

    FrameConverter converter;
    bool fatalError = false;

    while (!stopRequested_.load() && !fatalError) {
        int ret = source->readPacket(packet.get());
        if (ret < 0) {
            if (stopRequested_.load()) {
                std::cout << "CaptureWorker: stop requested, exiting read loop." << std::endl;
            } else {
                std::cerr << "CaptureWorker: stream ended or read error: " << avErrorToString(ret) << std::endl;
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
            std::cerr << "CaptureWorker: avcodec_send_packet failed: " << avErrorToString(ret) << std::endl;
            break;
        }

        // Drain every frame this packet made available (0, 1, or more).
        while (!stopRequested_.load()) {
            ret = decoder->receiveFrame(frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break; // needs another packet, or stream ended
            }
            if (ret < 0) {
                std::cerr << "CaptureWorker: avcodec_receive_frame failed: " << avErrorToString(ret) << std::endl;
                fatalError = true;
                break;
            }

            std::string convertError;
            std::optional<VideoFrame> videoFrame = converter.convert(frame.get(), convertError);

            av_frame_unref(frame.get()); // must be clean before the next receiveFrame call;
                                          // AVFrame is no longer needed once converted

            if (!videoFrame) {
                std::cerr << "CaptureWorker: failed to convert frame: " << convertError << std::endl;
                fatalError = true;
                break;
            }

            queue_.push(std::move(*videoFrame));
        }
    }

    std::cout << "CaptureWorker: run loop exited." << std::endl;
}
