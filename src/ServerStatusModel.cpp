#include "ServerStatusModel.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

const char* const ServerStatusModel::kServerBaseUrl = "http://127.0.0.1:8080";
const char* const ServerStatusModel::kChannelName = "test";

ServerStatusModel::ServerStatusModel(QObject* parent)
    : QObject(parent)
    , network_(new QNetworkAccessManager(this))
{
    connect(&timer_, &QTimer::timeout, this, &ServerStatusModel::poll);
    timer_.start(kPollIntervalMs);
    poll(); // first result on screen right away, not after the first interval
}

void ServerStatusModel::poll()
{
    queryChannelStatus();
    queryDecodeMetrics();
}

void ServerStatusModel::queryChannelStatus()
{
    if (channelStatusReply_) {
        return; // previous query still in flight -- never stack up requests
    }

    QNetworkRequest request(QUrl(QString::fromLatin1(kServerBaseUrl) + QStringLiteral("/channels/")
        + QString::fromLatin1(kChannelName)));
    request.setTransferTimeout(kRequestTimeoutMs);

    channelStatusReply_ = network_->get(request);
    connect(channelStatusReply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = channelStatusReply_;
        channelStatusReply_ = nullptr; // clear the in-flight guard before anything else can observe it
        reply->deleteLater();

        // Mirrors MediaMtxClient's own reachability check (D14): an HTTP
        // status code attribute being present means sightflow-server.exe
        // answered at all, regardless of which status it used -- a 503
        // body (MediaMTX unreachable, from the server's perspective) is
        // still a successful read of THIS server's response.
        const QVariant httpStatusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);

        if (!httpStatusAttr.isValid() || parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            channelStatusReachable_ = false;
            mediaMtxReachable_ = false;
            mediaMtxLive_ = false;
            emit statusChanged();
            return;
        }

        const QJsonObject obj = doc.object();
        channelStatusReachable_ = true;
        mediaMtxReachable_ = !obj.contains(QStringLiteral("error"));
        mediaMtxLive_ = mediaMtxReachable_ && obj.value(QStringLiteral("live")).toBool(false);
        emit statusChanged();
    });
}

void ServerStatusModel::queryDecodeMetrics()
{
    if (decodeMetricsReply_) {
        return; // previous query still in flight -- never stack up requests
    }

    QNetworkRequest request(QUrl(QString::fromLatin1(kServerBaseUrl) + QStringLiteral("/channels/")
        + QString::fromLatin1(kChannelName) + QStringLiteral("/metrics")));
    request.setTransferTimeout(kRequestTimeoutMs);

    decodeMetricsReply_ = network_->get(request);
    connect(decodeMetricsReply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = decodeMetricsReply_;
        decodeMetricsReply_ = nullptr;
        reply->deleteLater();

        const QVariant httpStatusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);

        if (!httpStatusAttr.isValid() || parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            decodeMetricsReachable_ = false;
            decodeState_ = QStringLiteral("unknown");
            framesDecoded_ = 0;
            emit statusChanged();
            return;
        }

        const QJsonObject obj = doc.object();
        decodeMetricsReachable_ = true;
        decodeState_ = obj.value(QStringLiteral("state")).toString(QStringLiteral("unknown"));
        framesDecoded_ = obj.value(QStringLiteral("framesDecoded")).toInteger();
        emit statusChanged();
    });
}
