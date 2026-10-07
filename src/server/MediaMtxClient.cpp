#include "MediaMtxClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

MediaMtxClient::MediaMtxClient(QUrl apiBaseUrl, QObject* parent)
    : QObject(parent)
    , apiBaseUrl_(std::move(apiBaseUrl))
{
}

void MediaMtxClient::queryPathStatus(const QString& pathName, std::function<void(PathStatus)> callback)
{
    QUrl url = apiBaseUrl_;
    url.setPath(apiBaseUrl_.path() + QStringLiteral("/v3/paths/get/") + pathName);

    QNetworkRequest request(url);
    request.setTransferTimeout(kRequestTimeoutMs); // bounds the wait; never blocks the caller either way

    QNetworkReply* reply = network_.get(request);
    inFlight_.push_back(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        inFlight_.removeOne(reply);
        reply->deleteLater();

        PathStatus status;
        const QVariant httpStatusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (httpStatusAttr.isValid()) {
            // MediaMTX answered with an HTTP response -- it is reachable,
            // regardless of which status code it returned (e.g. a 404 for an
            // unconfigured path still means MediaMTX itself is up).
            status.reachable = true;
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            status.live = doc.object().value(QStringLiteral("ready")).toBool(false);
        } else {
            // No HTTP response reached us at all: connection refused, DNS
            // failure, timeout, or an abort() during shutdown.
            status.reachable = false;
            status.live = false;
        }
        callback(status);
    });
}

void MediaMtxClient::abortAll()
{
    const QList<QNetworkReply*> snapshot = inFlight_;
    for (QNetworkReply* reply : snapshot) {
        reply->abort();
    }
}
