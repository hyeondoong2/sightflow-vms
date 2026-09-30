#include <iostream>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include "AvError.h"
#include "Decoder.h"
#include "FrameConverter.h"
#include "RtspSource.h"
#include "VideoFrame.h"

namespace {

struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const noexcept { av_packet_free(&pkt); }
};
using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

} // namespace

int main()
{
    const std::string url = "rtsp://127.0.0.1:8554/test";

    std::string sourceError;
    std::unique_ptr<RtspSource> source = RtspSource::open(url, sourceError);
    if (!source) {
        std::cerr << sourceError << std::endl;
        return 1;
    }

    const int videoStreamIndex = source->videoStreamIndex();
    std::cout << "Connected to " << url << ", video stream index " << videoStreamIndex << std::endl;

    std::string decoderError;
    std::unique_ptr<Decoder> decoder = Decoder::create(source->videoCodecParameters(), decoderError);
    if (!decoder) {
        std::cerr << "Failed to create decoder: " << decoderError << std::endl;
        return 1;
    }

    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        std::cerr << "Failed to allocate AVPacket" << std::endl;
        return 1;
    }

    FramePtr frame(av_frame_alloc());
    if (!frame) {
        std::cerr << "Failed to allocate AVFrame" << std::endl;
        return 1;
    }

    FrameConverter converter;

    constexpr int targetFrames = 30;
    int convertedFrameCount = 0;
    bool fatalError = false;

    while (convertedFrameCount < targetFrames && !fatalError) {
        int ret = source->readPacket(packet.get());
        if (ret < 0) {
            std::cerr << "Stream ended or read error: " << avErrorToString(ret) << std::endl;
            break;
        }

        if (packet->stream_index != videoStreamIndex) {
            av_packet_unref(packet.get());
            continue;
        }

        ret = decoder->sendPacket(packet.get());
        av_packet_unref(packet.get()); // packet's data is copied into the decoder; released every code path

        if (ret < 0) {
            std::cerr << "avcodec_send_packet failed: " << avErrorToString(ret) << std::endl;
            break;
        }

        // Drain every frame this packet made available (0, 1, or more).
        while (convertedFrameCount < targetFrames) {
            ret = decoder->receiveFrame(frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break; // needs another packet, or stream ended
            }
            if (ret < 0) {
                std::cerr << "avcodec_receive_frame failed: " << avErrorToString(ret) << std::endl;
                fatalError = true;
                break;
            }

            std::string convertError;
            std::optional<VideoFrame> videoFrame = converter.convert(frame.get(), convertError);

            av_frame_unref(frame.get()); // must be clean before the next receiveFrame call;
                                          // AVFrame is no longer needed once converted

            if (!videoFrame) {
                std::cerr << "Failed to convert frame: " << convertError << std::endl;
                fatalError = true;
                break;
            }

            ++convertedFrameCount;
            std::cout << "frame " << convertedFrameCount << "/" << targetFrames
                      << " " << videoFrame->width() << "x" << videoFrame->height()
                      << " fmt=BGRA32"
                      << " stride=" << videoFrame->strideBytes() << "B"
                      << " bufferSize=" << videoFrame->bufferSize() << "B"
                      << std::endl;
        }

        if (fatalError) {
            break;
        }
    }

    std::cout << "Finished: converted " << convertedFrameCount << " video frames." << std::endl;

    return (convertedFrameCount == targetFrames) ? 0 : 1;
}
