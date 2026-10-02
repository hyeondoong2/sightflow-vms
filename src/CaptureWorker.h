#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "FrameQueue.h"

// Owns the capture/decode worker thread: runs RTSP connect -> read -> decode
// -> convert in a loop on a single dedicated thread, moving completed
// VideoFrames into a caller-owned FrameQueue. RtspSource, Decoder,
// FrameConverter, AVPacket, and AVFrame are all local to the worker thread's
// run loop (never members here, never touched by any other thread) -- only
// VideoFrame crosses the thread boundary, via FrameQueue.
class CaptureWorker {
public:
    // `queue` must outlive this CaptureWorker.
    CaptureWorker(std::string url, FrameQueue& queue);

    // Requests stop (if running) and joins the worker thread.
    ~CaptureWorker();

    CaptureWorker(const CaptureWorker&) = delete;
    CaptureWorker& operator=(const CaptureWorker&) = delete;
    CaptureWorker(CaptureWorker&&) = delete;
    CaptureWorker& operator=(CaptureWorker&&) = delete;

    // Starts the worker thread. Phase 1 has no restart/reconnect: call this
    // once per CaptureWorker (see docs/DECISIONS.md D8).
    void start();

    // Requests the worker thread stop at its next opportunity -- this also
    // interrupts any blocking FFmpeg call (connect or read) in progress --
    // and joins it. Safe to call even if start() was never called, or after
    // a previous stop().
    void stop();

private:
    void run();

    std::string url_;
    FrameQueue& queue_;
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;
};
