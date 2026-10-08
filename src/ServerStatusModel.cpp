#include "ServerStatusModel.h"

#include <algorithm>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariantMap>

const char* const ServerStatusModel::kServerBaseUrl = "http://127.0.0.1:8080";

ServerStatusModel::ServerStatusModel(QObject* parent)
    : QObject(parent)
    , network_(new QNetworkAccessManager(this))
{
    connect(&timer_, &QTimer::timeout, this, &ServerStatusModel::poll);
    timer_.start(kPollIntervalMs);

    // Deferred, not called synchronously here: QML assigns declared
    // properties (including channelName) right after construction, before
    // control returns to the event loop -- a 0ms singleShot runs after
    // that, so this instance's first poll already uses the channel QML
    // actually declared, not channelName_'s default.
    QTimer::singleShot(0, this, &ServerStatusModel::poll);
}

void ServerStatusModel::setChannelName(const QString& name)
{
    if (channelName_ == name) {
        return;
    }
    channelName_ = name;
    emit channelNameChanged();
}

void ServerStatusModel::poll()
{
    queryChannelStatus();
    queryDecodeMetrics();
    queryChangeEvents();
}

void ServerStatusModel::queryChannelStatus()
{
    if (channelStatusReply_) {
        return; // previous query still in flight -- never stack up requests
    }

    QNetworkRequest request(QUrl(QString::fromLatin1(kServerBaseUrl) + QStringLiteral("/channels/") + channelName_));
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

    QNetworkRequest request(
        QUrl(QString::fromLatin1(kServerBaseUrl) + QStringLiteral("/channels/") + channelName_ + QStringLiteral("/metrics")));
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

void ServerStatusModel::queryChangeEvents()
{
    if (changeEventsReply_) {
        return; // previous query still in flight -- never stack up requests
    }

    QNetworkRequest request(
        QUrl(QString::fromLatin1(kServerBaseUrl) + QStringLiteral("/channels/") + channelName_ + QStringLiteral("/events")));
    request.setTransferTimeout(kRequestTimeoutMs);

    changeEventsReply_ = network_->get(request);
    connect(changeEventsReply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = changeEventsReply_;
        changeEventsReply_ = nullptr;
        reply->deleteLater();

        const QVariant httpStatusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);

        if (!httpStatusAttr.isValid() || parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            changeEventsReachable_ = false;
            recentChangeEvents_.clear();
            emit statusChanged();
            return;
        }

        const QJsonArray events = doc.object().value(QStringLiteral("events")).toArray();
        changeEventsReachable_ = true;

        recentChangeEvents_.clear();
        if (!events.isEmpty()) {
            // Server returns newest-first (ChangeEventLog::recentEvents()).
            const int shown = std::min(static_cast<int>(events.size()), kMaxDisplayedEvents);
            recentChangeEvents_.reserve(shown);
            for (int i = 0; i < shown; ++i) {
                const QJsonObject eventObj = events.at(i).toObject();
                const QDateTime eventTime =
                    QDateTime::fromString(eventObj.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
                QVariantMap entry;
                entry[QStringLiteral("time")] =
                    eventTime.isValid() ? eventTime.toLocalTime().toString(QStringLiteral("hh:mm:ss")) : QString();
                entry[QStringLiteral("ratio")] = eventObj.value(QStringLiteral("changeRatio")).toDouble();
                // Added by D23 -- lets QML offer a per-event snapshot without
                // guessing an id or re-deriving the server's URL shape itself.
                entry[QStringLiteral("id")] = eventObj.value(QStringLiteral("id")).toVariant();
                entry[QStringLiteral("snapshotAvailable")] =
                    eventObj.value(QStringLiteral("snapshotAvailable")).toBool(false);
                recentChangeEvents_.append(entry);
            }
        }
        emit statusChanged();
    });
}
