#pragma once

#include <QString>

#include "DecodeMetrics.h"
#include "HttpTypes.h"

// Translates GET /channels/<name>/metrics into a JSON snapshot of this
// server's own DecodeWorker (frames decoded, last frame size, run state).
// This is this server's own decode-worker status -- distinct from, and
// never to be confused with, ChannelStatusService's MediaMTX-source status
// (see docs/DECISIONS.md D16). Knows nothing about sockets, MediaMTX, or
// FFmpeg -- only reads DecodeMetrics::snapshot(), which never blocks.
class DecodeMetricsService {
public:
    // `channelName` is the one channel this server's single DecodeWorker
    // actually decodes; any other name in the request path 404s.
    DecodeMetricsService(QString channelName, DecodeMetrics& metrics);

    // Matches RequestHandler's shape (see HttpTypes.h).
    void handleRequest(const QString& method, const QString& path, RespondFn respond);

private:
    QString channelName_;
    DecodeMetrics& metrics_;
};
