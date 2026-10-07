#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// Detects a meaningful change in a channel's decoded video by periodically
// downscaling a decoded frame to a small grayscale thumbnail and comparing
// it against the previous thumbnail. This is a "화면 변화 감지" (screen
// change detection) signal, NOT a motion or object detector: it only
// measures how much of a small, blurred version of the picture changed
// between two samples, so it responds to any sufficiently large brightness
// change -- lighting, camera motion, something entering frame, a scene cut
// -- and cannot distinguish a person, an object, or genuine motion from any
// of those. docs/DECISIONS.md D22 explains the reasoning behind each
// constant below.
//
// One instance is scoped to a single connected session: a fresh instance is
// constructed every time DecodeWorker's run loop reconnects (the same place
// `FrameConverter` is already fresh per session on the client side), so a
// reconnect's first frame is compared against nothing and can never itself
// be reported as a change.
//
// Single owner of its SwsContext, RAII-closed -- mirrors FrameConverter's
// exact SwsContext lifecycle pattern (src/FrameConverter.h), just scaling to
// a tiny grayscale thumbnail instead of full-size BGRA.
class ChangeDetector {
public:
    ChangeDetector() = default;

    ChangeDetector(const ChangeDetector&) = delete;
    ChangeDetector& operator=(const ChangeDetector&) = delete;

    // Call once per decoded frame, from the decode worker thread only.
    // Most calls do nothing (return std::nullopt): actual comparisons are
    // throttled to roughly once every kCompareInterval, and even a real
    // comparison only returns a value on the specific comparison that
    // completes an event -- at least kDetectionsRequiredInWindow of the
    // last kDetectionWindowSize comparisons showed change, and the result
    // is not still within the post-event cooldown. The returned value is
    // the measured change ratio (0.0-1.0) of the triggering comparison.
    std::optional<double> processFrame(const AVFrame* frame);

private:
    struct SwsContextDeleter {
        void operator()(SwsContext* ctx) const noexcept { sws_freeContext(ctx); }
    };
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

    // --- Tunable constants -- see docs/DECISIONS.md D22 for the reasoning
    // behind each one. ---

    // Comparison thumbnail size: small enough that downscaling and
    // comparing it is negligible CPU, large enough to still hold some
    // spatial structure (not just one averaged brightness value).
    static constexpr int kSampleWidth = 32;
    static constexpr int kSampleHeight = 24;

    // Minimum wall-clock time between two comparisons -- most decoded
    // frames are skipped entirely. Comparing every single decoded frame
    // would multiply CPU cost by the source frame rate for no benefit: a
    // real scene change stays visible far longer than one frame interval.
    static constexpr std::chrono::milliseconds kCompareInterval{200};

    // A thumbnail pixel counts as "changed" only if its brightness moved by
    // more than this (out of 255) -- filters out per-pixel codec/encoding
    // noise in an otherwise static scene.
    static constexpr int kPixelDiffThreshold = 25;

    // One comparison counts as "showing change" only if at least this
    // fraction of thumbnail pixels changed -- a handful of noisy pixels
    // must not look like a real change. Set from direct measurement during
    // this feature's own verification (docs/DECISIONS.md D22), not guessed:
    // an initial 0.15 never fired at all against FFmpeg's own `testsrc`
    // test pattern, whose genuine (non-duplicate-frame) comparisons
    // measured only ~0.03-0.05 at this thumbnail size/pixel-diff threshold
    // -- a moving clock digit and a scrolling color gradient occupy a
    // modest fraction of the frame and the gradient's hue changes without
    // much brightness change, so even real, continuous content change
    // produces a modest grayscale ratio. 0.03 sits just above that
    // "genuinely changing" test signal while still being clearly above the
    // exactly-0.0 a truly static source (bit-identical frames) always
    // produces.
    static constexpr double kChangeRatioThreshold = 0.03;

    // A change must show up on at least kDetectionsRequiredInWindow of the
    // last kDetectionWindowSize comparisons (each already kCompareInterval
    // apart) before it is reported as an event -- filters out a
    // single-comparison fluke (one bad/glitched frame) while still
    // tolerating one ordinary miss in between two genuine detections. A
    // strict "every comparison in a row, zero misses allowed" version was
    // tried first and found (during this feature's own verification,
    // docs/DECISIONS.md D22) to never fire at all against content that
    // legitimately repeats an unchanged frame every other sample -- a
    // common pattern from frame-rate padding/low-bitrate frame skipping in
    // both test sources and real encoders, not a defect in the source
    // itself -- because a single such repeat (ratio 0, correctly) reset a
    // strict consecutive counter back to zero every time.
    static constexpr int kDetectionWindowSize = 3;
    static constexpr int kDetectionsRequiredInWindow = 2;

    // After an event fires, no further event is reported for this long,
    // even if the change continues -- keeps one sustained change (e.g. a
    // few seconds of motion) from producing a flood of near-duplicate
    // events while it lasts.
    static constexpr std::chrono::milliseconds kEventCooldown{3000};

    // Rebuilds ctx_ only if (width, height, srcFormat) differ from what it
    // was last built for (sws_getCachedContext's own comparison) -- mirrors
    // FrameConverter::ensureContext exactly, just with a fixed small
    // grayscale destination instead of a source-sized BGRA one.
    bool ensureContext(int width, int height, AVPixelFormat srcFormat);

    SwsContextPtr ctx_;

    // kSampleWidth*kSampleHeight bytes, GRAY8. Empty/unused until
    // hasPreviousSample_ is true.
    std::vector<uint8_t> previousSample_;
    bool hasPreviousSample_ = false;

    std::chrono::steady_clock::time_point lastCompareTime_{};

    // Bounded to kDetectionWindowSize entries (oldest dropped first): each
    // entry is whether that past comparison's ratio cleared
    // kChangeRatioThreshold.
    std::deque<bool> recentDetections_;

    std::chrono::steady_clock::time_point cooldownUntil_{};
};
