#pragma once

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

// Owns the AVCodecContext for one video stream. Single owner, RAII-closed.
class Decoder {
public:
    // Returns nullptr and fills errorOut with a ready-to-print message on
    // failure.
    static std::unique_ptr<Decoder> create(const AVCodecParameters* codecpar, std::string& errorOut);

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Mirrors avcodec_send_packet's return value.
    int sendPacket(const AVPacket* pkt);

    // Mirrors avcodec_receive_frame's return value. `frame` must be clean
    // (freshly allocated or just av_frame_unref'd) before this call.
    int receiveFrame(AVFrame* frame);

private:
    struct CodecContextDeleter {
        void operator()(AVCodecContext* ctx) const noexcept { avcodec_free_context(&ctx); }
    };
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

    explicit Decoder(CodecContextPtr ctx) : ctx_(std::move(ctx)) {}

    CodecContextPtr ctx_;
};
