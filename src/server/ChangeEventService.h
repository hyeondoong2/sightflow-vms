#pragma once

#include <QString>

#include "ChangeEventLog.h"
#include "HttpTypes.h"

// Translates GET /channels/<name>/events into a JSON list of this channel's
// most recent "화면 변화 감지" (screen change detection) events
// (ChangeEventLog). Distinct from both ChannelStatusService (MediaMTX
// source state, D14) and DecodeMetricsService (decode worker state, D16) --
// this is specifically about measured picture changes, nothing else (see
// docs/DECISIONS.md D22). Knows nothing about sockets, MediaMTX, or FFmpeg
// -- only reads ChangeEventLog::recentEvents(), which never blocks.
class ChangeEventService {
public:
    // `channelName` is the one channel this instance serves; any other name
    // in the request path 404s (mirrors DecodeMetricsService).
    ChangeEventService(QString channelName, ChangeEventLog& eventLog);

    // Matches RequestHandler's shape (see HttpTypes.h).
    void handleRequest(const QString& method, const QString& path, RespondFn respond);

private:
    QString channelName_;
    ChangeEventLog& eventLog_;
};
