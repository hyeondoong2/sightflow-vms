#pragma once

#include <mutex>
#include <string>

// Small, application-owned snapshot of what one DecodeWorker has done so
// far -- frame count, last decoded frame's size, and run state. This is the
// *only* thing that crosses the DecodeWorker-thread / Qt-event-loop-thread
// boundary (see docs/DECISIONS.md D16): no AVPacket, AVFrame, or Qt socket
// object ever does.
//
// Written by the decode worker thread (setState/setError/recordFrame) after
// every state transition and every successfully decoded frame; read by the
// Qt event-loop thread (snapshot()) when an HTTP request asks for it
// (DecodeMetricsService). The mutex is held only for the brief copy in/out,
// never across FFmpeg or socket I/O -- mirroring FrameQueue's locking
// discipline (docs/ARCHITECTURE.md §5/§8).
class DecodeMetrics {
public:
    enum class State { Connecting, Running, Error, Stopped };

    struct Snapshot {
        State state = State::Connecting;
        long long framesDecoded = 0;
        int lastFrameWidth = 0;
        int lastFrameHeight = 0;
        std::string lastError; // meaningful only when state == Error
    };

    void setState(State state);

    // Transitions to State::Error and records `message`.
    void setError(const std::string& message);

    // Increments framesDecoded and records this frame's size.
    void recordFrame(int width, int height);

    Snapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    Snapshot data_;
};
