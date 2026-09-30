#include "FrameConverter.h"

bool FrameConverter::ensureContext(int width, int height, AVPixelFormat srcFormat, std::string& errorOut)
{
    // sws_getCachedContext reuses ctx_ unchanged if (width, height, srcFormat,
    // kOutputPixelFormat) match what it was last built for; otherwise it
    // frees the context it's handed and allocates+inits a new one. Either
    // way it returns the context to use, so ownership round-trips through it
    // cleanly via release()/reset().
    SwsContext* raw = sws_getCachedContext(
        ctx_.release(),
        width, height, srcFormat,
        width, height, kOutputPixelFormat,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!raw) {
        errorOut = "Failed to create SwsContext";
        return false;
    }

    ctx_.reset(raw);
    return true;
}

std::optional<VideoFrame> FrameConverter::convert(const AVFrame* frame, std::string& errorOut)
{
    const auto srcFormat = static_cast<AVPixelFormat>(frame->format);

    if (!ensureContext(frame->width, frame->height, srcFormat, errorOut)) {
        return std::nullopt;
    }

    // Packed BGRA, tightly packed rows: exactly width * 4 bytes per row.
    const int strideBytes = frame->width * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(strideBytes) * frame->height);

    uint8_t* dstData[4] = { pixels.data(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { strideBytes, 0, 0, 0 };

    const int scaledRows =
        sws_scale(ctx_.get(), frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
    if (scaledRows <= 0) {
        errorOut = "sws_scale failed";
        return std::nullopt;
    }

    return VideoFrame(frame->width, frame->height, strideBytes, VideoFrame::PixelFormat::BGRA32, std::move(pixels));
}
