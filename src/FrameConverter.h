#pragma once

#include <memory>
#include <optional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "VideoFrame.h"

// Converts decoded AVFrames into VideoFrame via sws_scale. Always converts to
// VideoFrame::PixelFormat::BGRA32 at the source frame's own width/height
// (color-space conversion only, no resizing). Single owner of its
// SwsContext, RAII-closed.
class FrameConverter {
public:
    FrameConverter() = default;

    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    // Returns std::nullopt and fills errorOut on failure. The returned
    // VideoFrame owns an independent pixel buffer, so it is safe to
    // av_frame_unref `frame` immediately after this call returns.
    std::optional<VideoFrame> convert(const AVFrame* frame, std::string& errorOut);

private:
    struct SwsContextDeleter {
        void operator()(SwsContext* ctx) const noexcept { sws_freeContext(ctx); }
    };
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

    static constexpr AVPixelFormat kOutputPixelFormat = AV_PIX_FMT_BGRA;

    // Creates or reconfigures ctx_ for the given source parameters if they
    // differ from what it was last built for (or if it doesn't exist yet).
    bool ensureContext(int width, int height, AVPixelFormat srcFormat, std::string& errorOut);

    SwsContextPtr ctx_;
};
