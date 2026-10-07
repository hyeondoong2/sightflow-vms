#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

#include "VideoFrame.h"

// Thread-safe holder of VideoFrames shared between the capture/decode worker
// and the UI thread (see "Bounded FrameQueue" in docs/ARCHITECTURE.md).
// Fixed capacity of 2: push drops the oldest queued frame when full,
// tryPopLatest returns only the newest frame and discards any older one(s).
// The mutex guards only this queue's own internal state; callers never hold
// it across a network call or a frame conversion.
class FrameQueue {
public:
    FrameQueue() = default;

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // Moves `frame` into the queue. If already at capacity, the oldest
    // queued frame is dropped first to make room. Never blocks.
    void push(VideoFrame frame);

    // Moves the newest queued frame out and returns it, discarding any
    // older frame(s) also queued. Returns std::nullopt if the queue is
    // empty. Never blocks.
    std::optional<VideoFrame> tryPopLatest();

    // Drops every currently queued frame. Used when a capture session ends
    // (D19): without this, a frame pushed just before a disconnect could
    // still be sitting here and get displayed as if it belonged to the next
    // (reconnected) session. Never blocks.
    void clear();

private:
    static constexpr std::size_t kCapacity = 2;

    std::mutex mutex_;
    std::deque<VideoFrame> frames_; // oldest at front, newest at back; size() <= kCapacity
};
