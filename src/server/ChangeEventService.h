#pragma once

#include <cstdint>

#include <QString>

#include "ChangeEventLog.h"
#include "HttpTypes.h"

// Translates two GET routes for one channel's "화면 변화 감지" (screen
// change detection) data (ChangeEventLog, D22/D23):
//   GET /channels/<name>/events                -- recent event metadata
//   GET /channels/<name>/events/<id>/snapshot  -- that event's JPEG (D23)
// Distinct from both ChannelStatusService (MediaMTX source state, D14) and
// DecodeMetricsService (decode worker state, D16) -- this is specifically
// about measured picture changes and the still images captured at the
// moment one was confirmed, nothing else (see docs/DECISIONS.md D22/D23).
// Knows nothing about sockets, MediaMTX, or FFmpeg -- only reads
// ChangeEventLog::recentEvents()/snapshotFor(), neither of which blocks.
// `<id>` is parsed as a plain integer and used only as a lookup key into
// ChangeEventLog -- never as a filesystem path (D23: snapshots are kept in
// memory only; nothing is ever written to or read from disk here).
class ChangeEventService {
public:
    // `channelName` is the one channel this instance serves; any other name
    // in the request path 404s (mirrors DecodeMetricsService).
    ChangeEventService(QString channelName, ChangeEventLog& eventLog);

    // Matches RequestHandler's shape (see HttpTypes.h).
    void handleRequest(const QString& method, const QString& path, RespondFn respond);

private:
    void handleEventList(const QString& channel, const RespondFn& respond);
    void handleSnapshot(std::uint64_t eventId, const RespondFn& respond);

    QString channelName_;
    ChangeEventLog& eventLog_;
};
