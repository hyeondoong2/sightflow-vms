#include "ChangeEventLog.h"

ChangeEventLog::ChangeEventLog(std::size_t capacity)
    : capacity_(capacity)
{
}

void ChangeEventLog::record(double changeRatio)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() >= capacity_) {
        events_.pop_front(); // drop the oldest to make room
    }
    events_.push_back(ChangeEvent{std::chrono::system_clock::now(), changeRatio});
}

std::vector<ChangeEvent> ChangeEventLog::recentEvents() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<ChangeEvent>(events_.rbegin(), events_.rend()); // newest first
}
