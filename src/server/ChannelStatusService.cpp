#include "ChannelStatusService.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {
// "/channels/<name>" -> "<name>", or an empty string if `path` doesn't match
// that shape (including an empty name).
QString parseChannelName(const QString& path)
{
    const QString prefix = QStringLiteral("/channels/");
    if (!path.startsWith(prefix)) {
        return {};
    }
    return path.mid(prefix.size());
}
} // namespace

ChannelStatusService::ChannelStatusService(MediaMtxClient& mediaMtxClient)
    : mediaMtxClient_(mediaMtxClient)
{
}

void ChannelStatusService::handleRequest(const QString& method, const QString& path, RespondFn respond)
{
    if (method != QStringLiteral("GET")) {
        respond(405, "text/plain", "Method Not Allowed");
        return;
    }

    const QString channel = parseChannelName(path);
    if (channel.isEmpty()) {
        respond(404, "text/plain", "Not Found");
        return;
    }

    mediaMtxClient_.queryPathStatus(channel, [respond, channel](MediaMtxClient::PathStatus status) {
        QJsonObject obj;
        obj["channel"] = channel;
        obj["source"] = "mediamtx";
        obj["live"] = status.live;

        int httpStatus = 200;
        if (!status.reachable) {
            obj["error"] = "mediamtx_unreachable";
            httpStatus = 503;
        }

        respond(httpStatus, "application/json", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });
}
