#include "FrameQueue.h"

void FrameQueue::push(VideoFrame frame)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (frames_.size() >= kCapacity) {
        frames_.pop_front(); // drop the oldest to make room
    }
    frames_.push_back(std::move(frame));
}

std::optional<VideoFrame> FrameQueue::tryPopLatest()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (frames_.empty()) {
        return std::nullopt;
    }

    VideoFrame latest = std::move(frames_.back());
    frames_.clear(); // drop the moved-from newest slot and any older frame(s)
    return latest;
}
