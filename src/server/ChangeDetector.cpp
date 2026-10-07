#include "ChangeDetector.h"

#include <algorithm>
#include <cstdlib>

bool ChangeDetector::ensureContext(int width, int height, AVPixelFormat srcFormat)
{
    // sws_getCachedContext reuses ctx_ unchanged if (width, height,
    // srcFormat, GRAY8-at-sample-size) match what it was last built for;
    // otherwise it frees what it's handed and allocates+inits a new one.
    SwsContext* raw = sws_getCachedContext(
        ctx_.release(),
        width, height, srcFormat,
        kSampleWidth, kSampleHeight, AV_PIX_FMT_GRAY8,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!raw) {
        return false;
    }
    ctx_.reset(raw);
    return true;
}

std::optional<double> ChangeDetector::processFrame(const AVFrame* frame)
{
    const auto now = std::chrono::steady_clock::now();
    if (hasPreviousSample_ && now - lastCompareTime_ < kCompareInterval) {
        return std::nullopt; // not time for another comparison yet
    }
    lastCompareTime_ = now;

    if (!ensureContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format))) {
        return std::nullopt; // can't compare this frame; try again next time
    }

    std::vector<uint8_t> sample(static_cast<size_t>(kSampleWidth) * kSampleHeight);
    uint8_t* dstData[4] = {sample.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {kSampleWidth, 0, 0, 0};
    if (sws_scale(ctx_.get(), frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize) <= 0) {
        return std::nullopt;
    }

    if (!hasPreviousSample_) {
        // First sample of this session: establish the baseline only -- this
        // is what keeps a reconnect's first frame from ever being reported
        // as a change (there is nothing yet to compare it against).
        previousSample_ = std::move(sample);
        hasPreviousSample_ = true;
        return std::nullopt;
    }

    int changedPixels = 0;
    for (size_t i = 0; i < sample.size(); ++i) {
        if (std::abs(static_cast<int>(sample[i]) - static_cast<int>(previousSample_[i])) > kPixelDiffThreshold) {
            ++changedPixels;
        }
    }
    const double changeRatio = static_cast<double>(changedPixels) / static_cast<double>(sample.size());
    previousSample_ = std::move(sample);

    recentDetections_.push_back(changeRatio >= kChangeRatioThreshold);
    if (recentDetections_.size() > kDetectionWindowSize) {
        recentDetections_.pop_front();
    }

    if (now < cooldownUntil_) {
        return std::nullopt; // still cooling down from a previous event
    }

    const int detectionsInWindow = static_cast<int>(std::count(recentDetections_.begin(), recentDetections_.end(), true));
    if (detectionsInWindow < kDetectionsRequiredInWindow) {
        return std::nullopt; // not enough recent comparisons showed change yet
    }

    // Event: clear the window so the next event needs its own fresh run of
    // detections, and start a new cooldown.
    recentDetections_.clear();
    cooldownUntil_ = now + kEventCooldown;
    return changeRatio;
}
