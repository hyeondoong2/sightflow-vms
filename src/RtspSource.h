#pragma once

#include <atomic>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// Owns the AVFormatContext (and this process's FFmpeg network init) for one
// RTSP connection. Single owner, RAII-closed. Opens the URL, selects the
// video stream, and reads AVPackets from it.
class RtspSource {
public:
    // `stopRequested` must outlive this RtspSource (owned by the worker that
    // opens it, e.g. CaptureWorker). Installed as the AVFormatContext's
    // interrupt callback before avformat_open_input, so a stop request can
    // abort both the initial connection handshake and any later
    // readPacket() call that's blocked in network I/O.
    //
    // Returns nullptr and fills errorOut with a ready-to-print message on
    // failure. Every FFmpeg resource allocated during a failed attempt is
    // released before returning.
    static std::unique_ptr<RtspSource> open(const std::string& url,
                                             const std::atomic<bool>& stopRequested,
                                             std::string& errorOut);

    RtspSource(const RtspSource&) = delete;
    RtspSource& operator=(const RtspSource&) = delete;

    int videoStreamIndex() const noexcept { return videoStreamIndex_; }
    const AVCodecParameters* videoCodecParameters() const;

    // Mirrors av_read_frame's return value.
    int readPacket(AVPacket* packet);

private:
    struct NetworkGuard {
        NetworkGuard() { avformat_network_init(); }
        ~NetworkGuard() { avformat_network_deinit(); }
    };

    struct FormatContextCloser {
        void operator()(AVFormatContext* ctx) const noexcept { avformat_close_input(&ctx); }
    };
    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextCloser>;

    RtspSource() = default;

    // Does the actual opening work on an already-constructed (empty) object.
    // Returns false and fills errorOut on failure; whatever was already
    // acquired is cleaned up when the caller destroys this object.
    bool openInternal(const std::string& url, const std::atomic<bool>& stopRequested, std::string& errorOut);

    // FFmpeg's AVIOInterruptCB callback: returns non-zero (abort) once
    // `opaque` (a const std::atomic<bool>*) reads true.
    static int interruptCallback(void* opaque);

    NetworkGuard network_;
    FormatContextPtr fmtCtx_;
    int videoStreamIndex_ = -1;
};
