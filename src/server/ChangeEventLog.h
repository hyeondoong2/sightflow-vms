#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

// One "화면 변화 감지" (screen change detection) sample a channel's
// ChangeDetector produced. `changeRatio` is the fraction (0.0-1.0) of the
// small comparison thumbnail that differed from the previous one at the
// moment this event fired -- see docs/DECISIONS.md D22 for exactly what
// that does and does not mean (not "a person was detected", just "the
// picture changed by at least this much, for at least this long").
struct ChangeEvent {
    std::chrono::system_clock::time_point timestamp;
    double changeRatio = 0.0;
};

// Thread-safe, fixed-capacity holder of one channel's most recent change
// events -- mirrors FrameQueue's bounded-deque-with-mutex shape
// (docs/ARCHITECTURE.md §5): oldest dropped first once at capacity, mutex
// held only for the brief copy in/out, never across FFmpeg or socket I/O.
//
// Written by that channel's DecodeWorker thread (record()), read by the Qt
// event-loop thread (recentEvents()) when an HTTP request asks for it
// (ChangeEventService). This is the *only* thing ChangeDetector's work
// crosses the worker/HTTP-thread boundary as -- no AVFrame, no pixel
// buffer, just a timestamp and a ratio.
class ChangeEventLog {
public:
    explicit ChangeEventLog(std::size_t capacity);

    // Appends one event with the current wall-clock time, dropping the
    // oldest if already at capacity. Never blocks.
    void record(double changeRatio);

    // Newest-first copy of everything currently held. Never blocks.
    std::vector<ChangeEvent> recentEvents() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<ChangeEvent> events_; // oldest at front, newest at back; size() <= capacity_
};
