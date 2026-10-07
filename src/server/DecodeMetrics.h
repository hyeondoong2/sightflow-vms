#pragma once

#include <mutex>
#include <string>

// Small, application-owned snapshot of what one DecodeWorker has done so
// far -- frame count, last decoded frame's size, and run state. This is the
// *only* thing that crosses the DecodeWorker-thread / Qt-event-loop-thread
// boundary (see docs/DECISIONS.md D16/D17): no AVPacket, AVFrame, or Qt
// socket object ever does.
//
// Written by the decode worker thread (setState/setRetrying/beginRunning/
// recordFrame) after every state transition and every successfully decoded
// frame; read by the Qt event-loop thread (snapshot()) when an HTTP request
// asks for it (DecodeMetricsService). The mutex is held only for the brief
// copy in/out, never across FFmpeg or socket I/O -- mirroring FrameQueue's
// locking discipline (docs/ARCHITECTURE.md §5/§8).
class DecodeMetrics {
public:
    // Connecting: a connect/decoder-setup attempt is in progress right now
    //   (the first attempt, or any later retry attempt -- the state alone
    //   doesn't distinguish which).
    // Retrying: the most recent attempt (or the most recent connected
    //   session) just ended in failure; the worker is sleeping in its
    //   interruptible backoff wait before the next Connecting attempt.
    // Running: successfully connected, actively decoding.
    // Stopped: terminal -- stop() was requested and the worker thread
    //   exited. No further state changes after this (D17: server-side
    //   retry never resumes past an explicit stop).
    enum class State { Connecting, Retrying, Running, Stopped };

    struct Snapshot {
        State state = State::Connecting;

        // Frames decoded on the CURRENT or most-recently-ended connection
        // only -- reset to 0 every time a new connection succeeds
        // (beginRunning()). Not a lifetime total (D17): a process that has
        // reconnected many times does not accumulate this across sessions,
        // so a large number can never be mistaken for "decoding since the
        // process started" when the current session is actually young.
        long long framesDecoded = 0;

        // Also reset to 0 by beginRunning() -- same reasoning as
        // framesDecoded: a stale size from an ended session must not be
        // readable as the current session's size.
        int lastFrameWidth = 0;
        int lastFrameHeight = 0;

        // Most recent failure reason, if any. Set by setRetrying(), cleared
        // by beginRunning(). May still be non-empty while state ==
        // Connecting (it describes the attempt just before this one) but is
        // always empty while state == Running.
        std::string lastError;
    };

    // Connecting or Stopped -- transitions that don't touch any other field.
    void setState(State state);

    // Records a failed attempt or a dropped session: state -> Retrying,
    // lastError -> message. Deliberately does NOT touch framesDecoded/
    // lastFrameWidth/lastFrameHeight -- see Snapshot's docs above for why
    // they stay visible (frozen, clearly labeled stale by `state`) until
    // the next successful connection.
    void setRetrying(const std::string& message);

    // Marks the start of a newly-succeeded connection: state -> Running,
    // framesDecoded/lastFrameWidth/lastFrameHeight -> 0, lastError cleared.
    void beginRunning();

    // Increments framesDecoded and records this frame's size. Caller must
    // have already called beginRunning() for this connection.
    void recordFrame(int width, int height);

    Snapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    Snapshot data_;
};
