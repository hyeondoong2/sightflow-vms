#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// A plain, move-only value type holding one decoded video frame's pixel data
// in application-owned memory. Contains no FFmpeg types and no pointers into
// FFmpeg-owned memory: its pixel buffer is a completely independent copy,
// still valid after the AVFrame it was converted from has been unref'd.
class VideoFrame {
public:
    // The single output pixel format FrameConverter produces for Phase 1
    // display. Packed, 4 bytes per pixel, in-memory byte order per pixel is
    // B, G, R, A (little-endian), no per-row padding
    // (strideBytes() == width() * 4). This is bit-compatible with Qt's
    // QImage::Format_RGB32 / Format_ARGB32 on a little-endian machine (Qt's
    // 0xAARRGGBB value is stored in memory as bytes B, G, R, A).
    enum class PixelFormat { BGRA32 };

    VideoFrame(int width, int height, int strideBytes, PixelFormat format, std::vector<uint8_t> pixels)
        : width_(width)
        , height_(height)
        , strideBytes_(strideBytes)
        , format_(format)
        , pixels_(std::move(pixels))
    {
    }

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&) noexcept = default;
    VideoFrame& operator=(VideoFrame&&) noexcept = default;

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    int strideBytes() const noexcept { return strideBytes_; }
    PixelFormat format() const noexcept { return format_; }
    const uint8_t* pixels() const noexcept { return pixels_.data(); }
    size_t bufferSize() const noexcept { return pixels_.size(); }

private:
    int width_;
    int height_;
    int strideBytes_;
    PixelFormat format_;
    std::vector<uint8_t> pixels_;
};
