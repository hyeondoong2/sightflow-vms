#include "DecodeMetricsService.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {
const char* stateToString(DecodeMetrics::State state)
{
    switch (state) {
        case DecodeMetrics::State::Connecting: return "connecting";
        case DecodeMetrics::State::Running: return "running";
        case DecodeMetrics::State::Error: return "error";
        case DecodeMetrics::State::Stopped: return "stopped";
    }
    return "unknown";
}

// "/channels/<name>/metrics" -> "<name>", or an empty string if `path`
// doesn't match that shape (including an empty name).
QString parseChannelName(const QString& path)
{
    const QString prefix = QStringLiteral("/channels/");
    const QString suffix = QStringLiteral("/metrics");
    if (!path.startsWith(prefix) || !path.endsWith(suffix)) {
        return {};
    }
    const int nameLength = path.size() - prefix.size() - suffix.size();
    if (nameLength <= 0) {
        return {}; // "/channels//metrics" or shorter -- no room for a name
    }
    return path.mid(prefix.size(), nameLength);
}
} // namespace

DecodeMetricsService::DecodeMetricsService(QString channelName, DecodeMetrics& metrics)
    : channelName_(std::move(channelName))
    , metrics_(metrics)
{
}

void DecodeMetricsService::handleRequest(const QString& method, const QString& path, RespondFn respond)
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

    // Never blocks: DecodeMetrics::snapshot() only copies out a small
    // mutex-guarded struct, no FFmpeg or socket call involved.
    const DecodeMetrics::Snapshot snapshot = metrics_.snapshot();

    QJsonObject obj;
    obj["channel"] = channel;
    obj["source"] = "decoder"; // this server's own decode worker -- see GET /channels/<name> for MediaMTX's view
    obj["state"] = stateToString(snapshot.state);
    obj["framesDecoded"] = static_cast<qint64>(snapshot.framesDecoded);
    obj["lastFrameWidth"] = snapshot.lastFrameWidth;
    obj["lastFrameHeight"] = snapshot.lastFrameHeight;
    if (snapshot.state == DecodeMetrics::State::Error) {
        obj["error"] = QString::fromStdString(snapshot.lastError);
    }

    respond(200, "application/json", QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
