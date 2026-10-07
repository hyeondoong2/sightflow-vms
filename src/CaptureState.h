#pragma once

#include <atomic>

// Small, application-owned snapshot of CaptureWorker's own connection
// lifecycle -- written by the capture/decode worker thread, read by the UI
// thread (VideoDisplayItem) on its existing poll timer. This is the *only*
// additional thing that crosses the worker/UI boundary beyond FrameQueue
// itself (docs/ARCHITECTURE.md §3, D19): no AVPacket, AVFrame, or Qt object
// ever does.
//
// A single std::atomic<State> is sufficient (not a mutex-guarded struct like
// FrameQueue/DecodeMetrics): there is exactly one field, so there is no
// multi-field consistency to protect across a read.
class CaptureState {
public:
    // Connecting: a connect/decoder-setup attempt is in progress right now
    //   (the first attempt, or any later retry attempt).
    // Retrying: the most recent attempt (or the most recently connected
    //   session) just ended; the worker is sleeping in its interruptible
    //   backoff wait before the next Connecting attempt. VideoDisplayItem
    //   must not keep showing the last displayed frame while in this state.
    // Running: successfully connected, actively decoding and pushing
    //   frames into FrameQueue.
    // Stopped: terminal -- stop() was requested and the worker thread
    //   exited.
    enum class State { Connecting, Retrying, Running, Stopped };

    void setState(State state) noexcept { state_.store(state); }
    State state() const noexcept { return state_.load(); }

private:
    std::atomic<State> state_{State::Connecting};
};
