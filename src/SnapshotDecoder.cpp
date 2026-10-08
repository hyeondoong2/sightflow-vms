#include "SnapshotDecoder.h"

#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "AvError.h"
#include "AvRaii.h"
#include "FrameConverter.h"

namespace {
struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const noexcept { avcodec_free_context(&ctx); }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
} // namespace

std::optional<VideoFrame> decodeJpegSnapshot(const QByteArray& jpegBytes, std::string& errorOut)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        errorOut = "No mjpeg decoder available"; // should not happen: built into libavcodec itself
        return std::nullopt;
    }

    CodecContextPtr codecCtx(avcodec_alloc_context3(codec));
    if (!codecCtx) {
        errorOut = "Failed to allocate snapshot decoder context";
        return std::nullopt;
    }

    int ret = avcodec_open2(codecCtx.get(), codec, nullptr);
    if (ret < 0) {
        errorOut = "Failed to open snapshot decoder: " + avErrorToString(ret);
        return std::nullopt;
    }

    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        errorOut = "Failed to allocate snapshot AVPacket";
        return std::nullopt;
    }
    // A plain av_new_packet + memcpy is simpler than av_packet_from_data's
    // buffer-ownership transfer; this path isn't hot (one user click, one
    // small JPEG), so the one extra copy is not worth avoiding.
    if (av_new_packet(packet.get(), jpegBytes.size()) < 0) {
        errorOut = "Failed to allocate snapshot packet buffer";
        return std::nullopt;
    }
    std::memcpy(packet->data, jpegBytes.constData(), static_cast<size_t>(jpegBytes.size()));

    ret = avcodec_send_packet(codecCtx.get(), packet.get());
    if (ret < 0) {
        errorOut = "avcodec_send_packet failed for snapshot: " + avErrorToString(ret);
        return std::nullopt;
    }

    FramePtr frame(av_frame_alloc());
    if (!frame) {
        errorOut = "Failed to allocate snapshot AVFrame";
        return std::nullopt;
    }

    ret = avcodec_receive_frame(codecCtx.get(), frame.get());
    if (ret < 0) {
        errorOut = "avcodec_receive_frame failed for snapshot: " + avErrorToString(ret);
        return std::nullopt;
    }

    FrameConverter converter;
    return converter.convert(frame.get(), errorOut);
}
