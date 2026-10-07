#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "CaptureState.h"
#include "FrameQueue.h"

// Owns the capture/decode worker thread: runs RTSP connect -> read -> decode
// -> convert in a loop on a single dedicated thread, moving completed
// VideoFrames into a caller-owned FrameQueue. RtspSource, Decoder,
// FrameConverter, AVPacket, and AVFrame are all local to the worker thread's
// run loop (never members here, never touched by any other thread) -- only
// VideoFrame (via FrameQueue) and the connection state (via CaptureState)
// cross the thread boundary.
//
// Automatic retry (D19, supersedes D8): on a connect failure or a
// mid-stream drop, this worker waits a fixed interval and tries again, on
// the SAME thread -- no new thread, no thread pool, no work queue. This
// mirrors the shape of the server's DecodeWorker retry loop (D17), adapted
// to this class's own FrameQueue/CaptureState boundary objects rather than
// copied verbatim (DecodeWorker has no FrameQueue/display to manage;
// CaptureWorker has no DecodeMetrics-style frame-count/error reporting).
class CaptureWorker {
public:
    // `queue` and `captureState` must both outlive this CaptureWorker.
    CaptureWorker(std::string url, FrameQueue& queue, CaptureState& captureState);

    // Requests stop (if running) and joins the worker thread.
    ~CaptureWorker();

    CaptureWorker(const CaptureWorker&) = delete;
    CaptureWorker& operator=(const CaptureWorker&) = delete;
    CaptureWorker(CaptureWorker&&) = delete;
    CaptureWorker& operator=(CaptureWorker&&) = delete;

    // Starts the worker thread. Call once per CaptureWorker.
    void start();

    // Requests the worker thread stop at its next opportunity -- this
    // interrupts any blocking FFmpeg call in progress (AVIOInterruptCB,
    // §7) AND wakes an in-progress retry-backoff wait immediately (via
    // waitCv_) -- then joins. Safe to call even if start() was never
    // called, or after a previous stop().
    void stop();

private:
    void run();

    // Records `message` as the reason the attempt/session that just ended
    // failed, transitions CaptureState to Retrying, clears FrameQueue (so
    // no frame from the ended session can be displayed as if it belonged
    // to the next one), logs it (rate-limited), and waits out the retry
    // interval -- interruptibly. Returns false if stop() was requested
    // (caller must not retry, just exit), true if the interval elapsed
    // normally (caller should attempt to (re)connect again).
    bool retryAfterFailure(const std::string& message, int& consecutiveFailures);

    // Waits up to kRetryIntervalMs, or returns as soon as stop() is called.
    // Returns false if stop was requested, true if the interval elapsed.
    bool waitBeforeRetry();

    static constexpr int kRetryIntervalMs = 5000;
    static constexpr int kLogEveryNFailures = 10; // avoid one log line per retry forever

    std::string url_;
    FrameQueue& queue_;
    CaptureState& captureState_;
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;

    // Guards only the interruptible retry-backoff wait -- never held across
    // FFmpeg or socket I/O.
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
};
