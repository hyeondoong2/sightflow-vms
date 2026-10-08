#include "SnapshotEncoder.h"

#include <algorithm>
#include <memory>

extern "C" {
#include <libswscale/swscale.h>
}

#include "AvError.h"
#include "AvRaii.h"

namespace {
struct SwsContextDeleter {
    void operator()(SwsContext* ctx) const noexcept { sws_freeContext(ctx); }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const noexcept { avcodec_free_context(&ctx); }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

// Small still image, not a full-resolution frame -- see docs/DECISIONS.md D23.
constexpr int kMaxDimension = 320;

// JPEG quality knob for FFmpeg's qscale-driven mjpeg encoder (lower is
// higher quality/larger output; roughly the same 1-31 scale as mpeg qscale).
// Chosen to keep a kMaxDimension-sized snapshot comfortably small while
// still legible -- not measured against real camera footage, see D23.
constexpr int kJpegQscale = 8;

// Defensive upper bound on one encoded snapshot's size -- should never
// actually be reached at kMaxDimension/kJpegQscale; exists so one channel's
// ChangeEventLog has a hard memory ceiling regardless of scene content.
constexpr std::size_t kMaxEncodedBytes = 256 * 1024;

void computeTargetSize(int srcWidth, int srcHeight, int& dstWidth, int& dstHeight)
{
    const int longerSide = std::max(srcWidth, srcHeight);
    const double scale = longerSide > kMaxDimension ? static_cast<double>(kMaxDimension) / longerSide : 1.0;
    dstWidth = std::max(2, static_cast<int>(srcWidth * scale) & ~1);   // even: YUV 4:2:0 requires it
    dstHeight = std::max(2, static_cast<int>(srcHeight * scale) & ~1);
}
} // namespace

std::optional<std::vector<std::uint8_t>> encodeJpegSnapshot(const AVFrame* frame, std::string& errorOut)
{
    int dstWidth = 0;
    int dstHeight = 0;
    computeTargetSize(frame->width, frame->height, dstWidth, dstHeight);

    SwsContextPtr swsCtx(sws_getContext(
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        dstWidth, dstHeight, AV_PIX_FMT_YUVJ420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!swsCtx) {
        errorOut = "Failed to create snapshot SwsContext";
        return std::nullopt;
    }

    FramePtr scaled(av_frame_alloc());
    if (!scaled) {
        errorOut = "Failed to allocate snapshot AVFrame";
        return std::nullopt;
    }
    scaled->format = AV_PIX_FMT_YUVJ420P;
    scaled->width = dstWidth;
    scaled->height = dstHeight;
    if (av_frame_get_buffer(scaled.get(), 32) < 0) {
        errorOut = "Failed to allocate snapshot frame buffer";
        return std::nullopt;
    }

    if (sws_scale(swsCtx.get(), frame->data, frame->linesize, 0, frame->height, scaled->data, scaled->linesize) <= 0) {
        errorOut = "sws_scale failed for snapshot";
        return std::nullopt;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        errorOut = "No mjpeg encoder available"; // should not happen: built into libavcodec itself
        return std::nullopt;
    }

    CodecContextPtr codecCtx(avcodec_alloc_context3(codec));
    if (!codecCtx) {
        errorOut = "Failed to allocate snapshot AVCodecContext";
        return std::nullopt;
    }
    codecCtx->width = dstWidth;
    codecCtx->height = dstHeight;
    codecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    codecCtx->time_base = AVRational{1, 25};
    codecCtx->flags |= AV_CODEC_FLAG_QSCALE;
    codecCtx->global_quality = FF_QP2LAMBDA * kJpegQscale;

    int ret = avcodec_open2(codecCtx.get(), codec, nullptr);
    if (ret < 0) {
        errorOut = "Failed to open snapshot encoder: " + avErrorToString(ret);
        return std::nullopt;
    }

    scaled->quality = codecCtx->global_quality;
    scaled->pts = 0;

    ret = avcodec_send_frame(codecCtx.get(), scaled.get());
    if (ret < 0) {
        errorOut = "avcodec_send_frame failed for snapshot: " + avErrorToString(ret);
        return std::nullopt;
    }
    // Flush: one still image is sent, so exactly one packet is expected, but
    // draining via the documented send-EOF/receive-until-EOF sequence is
    // used anyway rather than assuming one send always yields one receive.
    avcodec_send_frame(codecCtx.get(), nullptr);

    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        errorOut = "Failed to allocate snapshot AVPacket";
        return std::nullopt;
    }

    std::vector<std::uint8_t> jpeg;
    while (true) {
        ret = avcodec_receive_packet(codecCtx.get(), packet.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            errorOut = "avcodec_receive_packet failed for snapshot: " + avErrorToString(ret);
            return std::nullopt;
        }
        if (jpeg.empty()) { // keep only the first packet -- one still image, one JPEG
            jpeg.assign(packet->data, packet->data + packet->size);
        }
        av_packet_unref(packet.get());
    }

    if (jpeg.empty()) {
        errorOut = "Snapshot encoder produced no data";
        return std::nullopt;
    }
    if (jpeg.size() > kMaxEncodedBytes) {
        errorOut = "Encoded snapshot exceeded the size cap";
        return std::nullopt;
    }

    return jpeg;
}
