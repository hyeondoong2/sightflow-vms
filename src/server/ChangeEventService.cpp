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

// "/channels/<channelName>/events/<id>/snapshot" -> <id>, or false if
// `path` doesn't match that exact shape for this channel (wrong channel,
// non-numeric id, or any other suffix). Never touches the filesystem --
// `<id>` is only ever used as a ChangeEventLog lookup key (D23).
bool parseSnapshotEventId(const QString& path, const QString& channelName, std::uint64_t& outId)
{
    const QString prefix = QStringLiteral("/channels/") + channelName + QStringLiteral("/events/");
    const QString suffix = QStringLiteral("/snapshot");
    if (!path.startsWith(prefix) || !path.endsWith(suffix)) {
        return false;
    }
    const int idLength = path.size() - prefix.size() - suffix.size();
    if (idLength <= 0) {
        return false;
    }
    const QString idPart = path.mid(prefix.size(), idLength);

    bool ok = false;
    const qulonglong parsed = idPart.toULongLong(&ok);
    if (!ok) {
        return false; // non-numeric -- not a valid id; falls through to a plain 404
    }
    outId = static_cast<std::uint64_t>(parsed);
    return true;
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

    std::uint64_t eventId = 0;
    if (parseSnapshotEventId(path, channelName_, eventId)) {
        handleSnapshot(eventId, respond);
        return;
    }

    const QString channel = parseChannelName(path);
    if (channel.isEmpty() || channel != channelName_) {
        respond(404, "text/plain", "Not Found");
        return;
    }
    handleEventList(channel, respond);
}

void ChangeEventService::handleEventList(const QString& channel, const RespondFn& respond)
{
    // Never blocks: ChangeEventLog::recentEvents() only copies out small
    // mutex-guarded metadata, no FFmpeg or socket call, no image bytes.
    const std::vector<ChangeEvent> events = eventLog_.recentEvents();

    QJsonArray eventsArray;
    for (const ChangeEvent& event : events) {
        const qint64 epochMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(event.timestamp.time_since_epoch()).count();
        QJsonObject eventObj;
        eventObj["id"] = static_cast<qint64>(event.id); // added by D23 -- existing fields below unchanged from D22
        eventObj["timestamp"] = QDateTime::fromMSecsSinceEpoch(epochMs, Qt::UTC).toString(Qt::ISODateWithMs);
        eventObj["changeRatio"] = event.changeRatio;
        eventObj["snapshotAvailable"] = event.hasSnapshot; // added by D23
        if (event.hasSnapshot) {
            eventObj["snapshotUrl"] = QStringLiteral("/channels/") + channel + QStringLiteral("/events/")
                + QString::number(event.id) + QStringLiteral("/snapshot");
        }
        eventsArray.append(eventObj);
    }

    QJsonObject obj;
    obj["channel"] = channel;
    obj["source"] = "change-detector"; // this channel's own ChangeDetector -- see docs/DECISIONS.md D22
    obj["events"] = eventsArray;

    respond(200, "application/json", QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void ChangeEventService::handleSnapshot(std::uint64_t eventId, const RespondFn& respond)
{
    std::vector<std::uint8_t> jpeg;
    const ChangeEventLog::SnapshotLookup result = eventLog_.snapshotFor(eventId, jpeg);

    switch (result) {
        case ChangeEventLog::SnapshotLookup::Found: {
            QByteArray body(reinterpret_cast<const char*>(jpeg.data()), static_cast<int>(jpeg.size()));
            respond(200, "image/jpeg", body);
            return;
        }
        case ChangeEventLog::SnapshotLookup::NoSnapshot:
            respond(404, "application/json",
                QJsonDocument(QJsonObject{{"error", "snapshot_not_available"}}).toJson(QJsonDocument::Compact));
            return;
        case ChangeEventLog::SnapshotLookup::EventNotFound:
            respond(404, "application/json",
                QJsonDocument(QJsonObject{{"error", "event_not_found"}}).toJson(QJsonDocument::Compact));
            return;
    }
}
