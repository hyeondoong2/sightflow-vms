#include "ChangeEventService.h"

#include <chrono>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
// "/channels/<name>/events" -> "<name>", or an empty string if `path`
// doesn't match that shape (including an empty name).
QString parseChannelName(const QString& path)
{
    const QString prefix = QStringLiteral("/channels/");
    const QString suffix = QStringLiteral("/events");
    if (!path.startsWith(prefix) || !path.endsWith(suffix)) {
        return {};
    }
    const int nameLength = path.size() - prefix.size() - suffix.size();
    if (nameLength <= 0) {
        return {}; // "/channels//events" or shorter -- no room for a name
    }
    return path.mid(prefix.size(), nameLength);
}
} // namespace

ChangeEventService::ChangeEventService(QString channelName, ChangeEventLog& eventLog)
    : channelName_(std::move(channelName))
    , eventLog_(eventLog)
{
}

void ChangeEventService::handleRequest(const QString& method, const QString& path, RespondFn respond)
{
    if (method != QStringLiteral("GET")) {
        respond(405, "text/plain", "Method Not Allowed");
        return;
    }

    const QString channel = parseChannelName(path);
    if (channel.isEmpty() || channel != channelName_) {
        respond(404, "text/plain", "Not Found");
        return;
    }

    // Never blocks: ChangeEventLog::recentEvents() only copies out a small
    // mutex-guarded container, no FFmpeg or socket call involved.
    const std::vector<ChangeEvent> events = eventLog_.recentEvents();

    QJsonArray eventsArray;
    for (const ChangeEvent& event : events) {
        const qint64 epochMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(event.timestamp.time_since_epoch()).count();
        QJsonObject eventObj;
        eventObj["timestamp"] = QDateTime::fromMSecsSinceEpoch(epochMs, Qt::UTC).toString(Qt::ISODateWithMs);
        eventObj["changeRatio"] = event.changeRatio;
        eventsArray.append(eventObj);
    }

    QJsonObject obj;
    obj["channel"] = channel;
    obj["source"] = "change-detector"; // this channel's own ChangeDetector -- see docs/DECISIONS.md D22
    obj["events"] = eventsArray;

    respond(200, "application/json", QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
