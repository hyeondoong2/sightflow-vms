#include "DecodeMetrics.h"

void DecodeMetrics::setState(State state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.state = state;
}

void DecodeMetrics::setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.state = State::Error;
    data_.lastError = message;
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
