#pragma once

#include <optional>
#include <string>

#include <QByteArray>

#include "VideoFrame.h"

// Decodes one self-contained JPEG byte buffer -- exactly what
// sightflow-server's GET /channels/<name>/events/<id>/snapshot returns
// (docs/DECISIONS.md D23) -- into a VideoFrame, via FFmpeg's "mjpeg" decoder
// (already linked into this executable for the live RTSP path, Decoder.h)
// followed by the existing FrameConverter (already BGRA32/QImage-compatible,
// D12). No Qt image plugin (QImage's own JPEG support) is used or required --
// this stays entirely on the same FFmpeg-decode-then-raw-QImage-copy path the
// live video display already relies on, so no new dependency and no new
// deployment risk (a missing imageformats/qjpeg.dll plugin) is introduced.
//
// One-shot: builds and tears down its own AVCodecContext per call (this is
// called only when a user selects one event's snapshot, not on any hot
// path), reusing FrameConverter for the final color-space step. Returns
// std::nullopt and fills errorOut on failure.
std::optional<VideoFrame> decodeJpegSnapshot(const QByteArray& jpegBytes, std::string& errorOut);
