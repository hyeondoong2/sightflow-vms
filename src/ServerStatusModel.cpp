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
#include <QWebSocket>

const char* const ServerStatusModel::kServerBaseUrl = "http://127.0.0.1:8080";
// Separate port from kServerBaseUrl's REST API (docs/DECISIONS.md D25) --
// sightflow-server.exe's QWebSocketServer owns its own listen socket and
// handshake, distinct from the hand-rolled HTTP server on 8080.
const char* const ServerStatusModel::kWebSocketBaseUrl = "ws://127.0.0.1:8081";

ServerStatusModel::ServerStatusModel(QObject* parent)
    : QObject(parent)
    , network_(new QNetworkAccessManager(this))
{
    connect(&timer_, &QTimer::timeout, this, &ServerStatusModel::poll);
    timer_.start(kPollIntervalMs);

    // Deferred, not called synchronously here: QML assigns declared
    // properties (including channelName) right after construction, before
    // control returns to the event loop -- a 0ms singleShot runs after
    // that, so this instance's first poll (and first WebSocket connect
    // attempt, D25) already uses the channel QML actually declared, not
    // channelName_'s default.
    QTimer::singleShot(0, this, &ServerStatusModel::poll);
    QTimer::singleShot(0, this, &ServerStatusModel::connectWebSocket);
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
                // "MM/dd hh:mm:ss", not just "hh:mm:ss" (docs/DECISIONS.md
                // D24): an event the server restored from before this
                // process started (D24) carries its true original
                // timestamp, which can be an earlier day -- always showing
                // the date keeps a restored history entry from reading as
                // "just happened" after a restart.
                entry[QStringLiteral("time")] =
                    eventTime.isValid() ? eventTime.toLocalTime().toString(QStringLiteral("MM/dd hh:mm:ss")) : QString();
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

void ServerStatusModel::connectWebSocket()
{
    if (webSocket_) {
        return; // a previous attempt is still connecting, or already connected
    }

    webSocket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(webSocket_, &QWebSocket::connected, this, &ServerStatusModel::onWebSocketConnected);
    connect(webSocket_, &QWebSocket::disconnected, this, &ServerStatusModel::onWebSocketDisconnected);
    connect(webSocket_, &QWebSocket::textMessageReceived, this, &ServerStatusModel::onWebSocketTextMessageReceived);
    connect(webSocket_, &QWebSocket::errorOccurred, this, &ServerStatusModel::onWebSocketError);

    const QUrl url(QString::fromLatin1(kWebSocketBaseUrl) + QStringLiteral("/channels/") + channelName_);
    webSocket_->open(url);
}

void ServerStatusModel::onWebSocketConnected()
{
    wsConnected_ = true;
    emit statusChanged();
}

void ServerStatusModel::onWebSocketError(QAbstractSocket::SocketError /*error*/)
{
    // No separate handling: a connection attempt that fails before ever
    // completing the WebSocket handshake still reaches disconnected()
    // shortly after this (Qt's documented behavior for QAbstractSocket-
    // derived classes), which already does the one thing that matters here
    // -- clear webSocket_ and schedule a retry. This slot only exists so the
    // error is observed (avoiding Qt's "unhandled signal" nothing-happens
    // silence) rather than to do its own cleanup.
}

void ServerStatusModel::onWebSocketDisconnected()
{
    if (!webSocket_) {
        return; // already handled (e.g. errorOccurred's disconnected already ran this once)
    }
    wsConnected_ = false;
    emit statusChanged();

    webSocket_->deleteLater();
    webSocket_ = nullptr;

    // The independent kPollIntervalMs REST poll (queryChangeEvents(), still
    // running on its own timer this whole time) is what keeps the UI
    // correct while this is down -- reconnecting promptly here only
    // improves latency, it is never required for correctness (D25).
    QTimer::singleShot(kWsReconnectIntervalMs, this, &ServerStatusModel::connectWebSocket);
}

void ServerStatusModel::onWebSocketTextMessageReceived(const QString& message)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return; // malformed -- ignore; the next periodic poll will still catch up regardless
    }

    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("channel")).toString() != channelName_) {
        // Defensive only -- sightflow-server already scopes this WebSocket
        // connection to this channel via its own request path
        // ("/channels/<channelName>", D25), so this should never actually
        // differ; never trust a network input blindly regardless.
        return;
    }

    // A push notification only ever means "go re-fetch the event list" --
    // it carries no snapshot/image data itself (D25) and is never treated
    // as the event list's own source of truth. queryChangeEvents() re-reads
    // the exact same REST endpoint the periodic poll already uses, so the
    // existing D24 "MM/dd hh:mm:ss" display and every other handling
    // applies identically regardless of what triggered this particular
    // fetch.
    queryChangeEvents();
}
