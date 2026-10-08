#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

#include <QString>

#include "ChangeEventLog.h"
#include "DecodeMetrics.h"

// Owns a dedicated decode worker thread: RTSP connect -> read -> decode,
// recording frame count/last-frame size/run state into a caller-owned
// DecodeMetrics (docs/DECISIONS.md D16), and screen-change events into a
// caller-owned ChangeEventLog (D22). Mirrors CaptureWorker's
// thread/ownership shape (src/CaptureWorker.h) and reuses RtspSource/Decoder
// directly -- no decode logic is duplicated. No pixel conversion for
// display (FrameConverter), no FrameQueue, no display: this worker counts
// frames, records their dimensions, and runs a lightweight per-frame screen
// change check (ChangeDetector) entirely on this same thread -- no second
// worker thread, no work queue, no AVFrame ever crosses a thread boundary.
//
// Server-side-only automatic retry (D17): on a connect failure or a
// mid-stream drop, this worker waits a fixed interval and tries again, on
// the SAME thread -- no new thread, no thread pool, no work queue. This is
// deliberately different from CaptureWorker/the client, which still never
// auto-reconnects (D8, Phase 1 client scope) -- see D17 for why the two are
// allowed to differ and are not in conflict.
//
// Also owns this channel's EventStore connection (D24): opened once at the
// top of run(), for this thread's whole lifetime (independent of how many
// times the RTSP session itself reconnects inside that same run() call) --
// never shared with any other thread. Right after each successful
// changeEventLog_.record() call, this worker also calls that connection's
// appendAndPrune() so the event survives a server restart. If persistence is
// unavailable (EventStore::open() failed, or a later appendAndPrune() call
// fails), this worker logs it and continues decoding/detecting/serving
// exactly as before D24 existed -- persistence failing never stops RTSP
// decoding or change detection (D24's resilience policy).
class DecodeWorker {
public:
    // `metrics` and `changeEventLog` must both outlive this DecodeWorker.
    // `channelName` identifies this channel's own rows in the shared SQLite
    // database at `dbFilePath` (D24) -- distinct from `url`, which is the
    // RTSP source, not a storage key. `eventCapacity` must match the
    // capacity `changeEventLog` was constructed with, so the on-disk and
    // in-memory bounds for this channel always agree.
    DecodeWorker(std::string url, std::string channelName, DecodeMetrics& metrics, ChangeEventLog& changeEventLog,
        QString dbFilePath, std::size_t eventCapacity);

    // Requests stop (if running) and joins the worker thread.
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;
    DecodeWorker(DecodeWorker&&) = delete;
    DecodeWorker& operator=(DecodeWorker&&) = delete;

    // Starts the worker thread. Call once per DecodeWorker.
    void start();

    // Requests the worker thread stop at its next opportunity -- this
    // interrupts any blocking FFmpeg call in progress (same AVIOInterruptCB
    // mechanism as CaptureWorker) AND wakes an in-progress retry backoff
    // wait immediately (via waitCv_) -- then joins. Safe to call even if
    // start() was never called, or after a previous stop().
    void stop();

private:
    void run();

    // Records `message` as the reason the attempt/session that just ended
    // failed, logs it (rate-limited via `consecutiveFailures`), and waits
    // out the retry interval -- interruptibly. Returns false if stop() was
    // requested (caller must not retry, just exit), true if the interval
    // elapsed normally (caller should attempt to (re)connect again).
    bool retryAfterFailure(const std::string& message, int& consecutiveFailures);

    // Waits up to kRetryIntervalMs, or returns as soon as stop() is called.
    // Returns false if stop was requested, true if the interval elapsed.
    bool waitBeforeRetry();

    static constexpr int kRetryIntervalMs = 5000;
    static constexpr int kLogEveryNFailures = 10; // avoid one log line per retry forever

    std::string url_;
    std::string channelName_;
    DecodeMetrics& metrics_;
    ChangeEventLog& changeEventLog_;
    QString dbFilePath_;
    std::size_t eventCapacity_;
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;

    // Guards only the interruptible retry-backoff wait -- never held across
    // FFmpeg, socket, or disk I/O.
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
};
