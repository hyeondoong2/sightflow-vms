#include "DecodeMetrics.h"

void DecodeMetrics::setState(State state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.state = state;
}

void DecodeMetrics::setRetrying(const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.state = State::Retrying;
    data_.lastError = message;
}

void DecodeMetrics::beginRunning()
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.state = State::Running;
    data_.framesDecoded = 0;
    data_.lastFrameWidth = 0;
    data_.lastFrameHeight = 0;
    data_.lastError.clear();
}

void DecodeMetrics::recordFrame(int width, int height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++data_.framesDecoded;
    data_.lastFrameWidth = width;
    data_.lastFrameHeight = height;
}

DecodeMetrics::Snapshot DecodeMetrics::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return data_;
}
