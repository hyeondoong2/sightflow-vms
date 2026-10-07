#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "DecodeMetrics.h"

// Owns a dedicated decode worker thread: RTSP connect -> read -> decode,
// recording frame count/last-frame size/run state into a caller-owned
// DecodeMetrics (docs/DECISIONS.md D16). Mirrors CaptureWorker's
// thread/ownership shape (src/CaptureWorker.h) and reuses RtspSource/Decoder
// directly -- no decode logic is duplicated. No pixel conversion
// (FrameConverter), no FrameQueue, no display: this step only counts frames
// and records their dimensions.
class DecodeWorker {
public:
    // `metrics` must outlive this DecodeWorker.
    DecodeWorker(std::string url, DecodeMetrics& metrics);

    // Requests stop (if running) and joins the worker thread.
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;
    DecodeWorker(DecodeWorker&&) = delete;
    DecodeWorker& operator=(DecodeWorker&&) = delete;

    // Starts the worker thread. Call once per DecodeWorker (no
    // restart/reconnect in this step -- mirrors D8).
    void start();

    // Requests the worker thread stop at its next opportunity -- this also
    // interrupts any blocking FFmpeg call (connect or read) in progress --
    // and joins it. Safe to call even if start() was never called, or after
    // a previous stop().
    void stop();

private:
    void run();

    std::string url_;
    DecodeMetrics& metrics_;
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;
};
