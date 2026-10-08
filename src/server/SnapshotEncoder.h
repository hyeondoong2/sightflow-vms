#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

// Encodes one decoded video frame into a small JPEG "화면 변화 감지" (screen
// change detection) snapshot, using FFmpeg's own "mjpeg" encoder -- already
// available through the avcodec library this project already links for
// decoding (no new dependency: no libjpeg, no Qt image plugin, no new vcpkg
// feature). See docs/DECISIONS.md D23 for why this format was chosen over
// Qt's QImage::save (would need QtGui linked into this Core+Network-only
// server, D14) and over FFmpeg's own "png" encoder (whose availability
// depends on an optional zlib build feature this project's vcpkg manifest
// does not enable, unlike mjpeg which has no such optional dependency).
//
// Called only at the moment ChangeDetector confirms an event -- never once
// per decoded frame -- from the owning DecodeWorker's own thread, right
// before that frame is unref'd. Builds a fresh scaler/encoder per call: this
// runs at most once per ChangeDetector::kEventCooldown (a few seconds), so
// the extra state needed to reuse a context across calls isn't justified.
//
// Downscales to at most kMaxDimension on the longer side (preserving aspect
// ratio, rounded to even pixel counts as YUV 4:2:0 requires) before encoding,
// so the result is a small still image, not a full-resolution frame -- this
// is what keeps each channel's bounded ChangeEventLog's memory footprint
// small and predictable. Returns std::nullopt and fills errorOut on any
// failure (unsupported source format, scaler/encoder setup failure, or an
// encoded result larger than kMaxEncodedBytes -- a defensive cap that should
// never actually trigger at this output size, kept as a hard upper bound on
// what one event can add to memory). A failed encode never fabricates a
// placeholder image -- the caller records the event without one.
std::optional<std::vector<std::uint8_t>> encodeJpegSnapshot(const AVFrame* frame, std::string& errorOut);
