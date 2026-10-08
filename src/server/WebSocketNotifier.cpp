#include "WebSocketNotifier.h"

#include <algorithm>
#include <iostream>
#include <utility>

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QWebSocket>

WebSocketNotifier::WebSocketNotifier(ChangeEventLog& changeEventLogTest, ChangeEventLog& changeEventLogTest2, QObject* parent)
    : QObject(parent)
    , changeEventLogTest_(changeEventLogTest)
    , changeEventLogTest2_(changeEventLogTest2)
    , server_(QStringLiteral("SightFlowVMS"), QWebSocketServer::NonSecureMode, this)
{
    connect(&server_, &QWebSocketServer::newConnection, this, &WebSocketNotifier::onNewConnection);
}

bool WebSocketNotifier::listen(quint16 port)
{
    return server_.listen(QHostAddress::Any, port);
}

void WebSocketNotifier::onNewConnection()
{
    while (QWebSocket* socket = server_.nextPendingConnection()) {
        socket->setParent(this);
        const QString path = socket->requestUrl().path();

        std::vector<QWebSocket*>* clients = nullptr;
        if (path == QStringLiteral("/channels/test")) {
            clients = &testClients_;
        } else if (path == QStringLiteral("/channels/test2")) {
            clients = &test2Clients_;
        } else {
            // Not one of the two fixed channel paths -- no generic routing
            // table, same two-literal-channels style as every other
            // per-channel class in src/server/. The handshake itself
            // already succeeded (QWebSocketServer completes it before
            // emitting newConnection), so the only option left is to close
            // right back down.
            socket->close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("unknown channel path"));
            socket->deleteLater();
            continue;
        }

        clients->push_back(socket);
        connect(socket, &QWebSocket::disconnected, this, [this, socket, clients]() {
            const auto it = std::find(clients->begin(), clients->end(), socket);
            if (it != clients->end()) {
                clients->erase(it);
            }
            // Never a direct delete from within this socket's own
            // disconnected-signal handling -- same D15 discipline
            // ClientConnection/HttpServer already follow for the plain-HTTP
            // side.
            socket->deleteLater();
        });
    }
}

void WebSocketNotifier::shutdown()
{
    server_.close(); // stop accepting new connections
    closeAllClients(testClients_);
    closeAllClients(test2Clients_);
}

void WebSocketNotifier::closeAllClients(std::vector<QWebSocket*>& clients)
{
    // Snapshot and clear before touching any socket -- a synchronous
    // close() can re-enter the disconnected lambda above, which would
    // otherwise mutate `clients` while this loop is iterating it (exact
    // same hazard HttpServer::shutdown() already guards against, D15).
    const std::vector<QWebSocket*> toClose = std::move(clients);
    clients.clear();

    for (QWebSocket* client : toClose) {
        disconnect(client, &QWebSocket::disconnected, this, nullptr);
        client->close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("server shutting down"));
        client->deleteLater();
    }
}

void WebSocketNotifier::requestNotifyTestChanged()
{
    // exchange(true) returns the PREVIOUS value: false means no call was
    // already outstanding, so this caller is the one that should post it;
    // true means one already is, so posting another would let queued calls
    // accumulate without bound while the Qt event loop is merely busy (the
    // gap this method exists to close -- see the class comment).
    if (!testNotifyPending_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, &WebSocketNotifier::notifyTestChanged, Qt::QueuedConnection);
    }
}

void WebSocketNotifier::requestNotifyTest2Changed()
{
    if (!test2NotifyPending_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, &WebSocketNotifier::notifyTest2Changed, Qt::QueuedConnection);
    }
}

void WebSocketNotifier::notifyTestChanged()
{
    // Cleared *before* reading ChangeEventLog below, not after: if
    // DecodeWorker's thread calls requestNotifyTestChanged() again right
    // after this clear (a new event recorded concurrently with this call
    // running), that call will see testNotifyPending_ already false and
    // correctly claim+post a fresh one -- so a race here can only ever
    // cause one extra, harmless queued call (caught by the dedupe-by-id
    // check in notifyChannelChanged once it runs), never a missed one.
    testNotifyPending_.store(false, std::memory_order_release);
    notifyChannelChanged(QStringLiteral("test"), changeEventLogTest_, lastBroadcastIdTest_, testClients_);
}

void WebSocketNotifier::notifyTest2Changed()
{
    test2NotifyPending_.store(false, std::memory_order_release);
    notifyChannelChanged(QStringLiteral("test2"), changeEventLogTest2_, lastBroadcastIdTest2_, test2Clients_);
}

void WebSocketNotifier::notifyChannelChanged(const QString& channelName, ChangeEventLog& log,
    std::uint64_t& lastBroadcastId, std::vector<QWebSocket*>& clients)
{
    // Never blocks: ChangeEventLog::recentEvents() only copies out small
    // mutex-guarded metadata, no FFmpeg/socket/disk I/O involved (D22) --
    // the exact same call ChangeEventService already makes for
    // GET /channels/<name>/events.
    const std::vector<ChangeEvent> events = log.recentEvents(); // newest-first
    if (events.empty()) {
        return;
    }

    const ChangeEvent& newest = events.front();
    if (newest.id == lastBroadcastId) {
        // Prevents a *duplicate* broadcast of an event already sent --
        // a separate concern from, and not a substitute for, the
        // pending-call bound enforced by testNotifyPending_/
        // test2NotifyPending_ above (see the class comment).
        return;
    }
    lastBroadcastId = newest.id;

    if (clients.empty()) {
        return; // no one connected on this channel's path right now
    }

    // Small notification only -- channel, id, and a couple of already-known
    // small fields -- never the JPEG snapshot bytes (D25). The final
    // event-list/snapshot lookup stays on the existing REST routes
    // (D22/D23), unchanged; this is purely a "go re-fetch" signal.
    QJsonObject obj;
    obj["channel"] = channelName;
    obj["id"] = static_cast<qint64>(newest.id);
    obj["changeRatio"] = newest.changeRatio;
    obj["snapshotAvailable"] = newest.hasSnapshot;
    const QString text = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));

    // Backpressure cap: a client that never reads would otherwise let Qt's
    // own per-socket write buffer (QWebSocket::bytesToWrite()) grow without
    // any bound for as long as it stays connected and this keeps sending.
    // Collected into a separate list and closed only *after* this loop
    // finishes -- never close() a socket while still iterating `clients`
    // itself, since that can re-enter the disconnected lambda (set up in
    // onNewConnection) that erases from this same vector mid-iteration
    // (the exact D15 hazard closeAllClients() already guards against).
    std::vector<QWebSocket*> stuck;
    for (QWebSocket* client : clients) {
        client->sendTextMessage(text);
        if (client->bytesToWrite() > kMaxBufferedBytesPerClient) {
            stuck.push_back(client);
        }
    }
    for (QWebSocket* client : stuck) {
        std::cerr << "WebSocketNotifier: dropping a client on channel '" << channelName.toStdString()
                   << "' that fell behind reading (" << client->bytesToWrite() << " bytes still buffered)\n";
        // close() starts a graceful close handshake; the existing
        // disconnected lambda (onNewConnection) does the actual
        // erase-from-`clients`-and-deleteLater() cleanup once it completes
        // -- no duplicated removal logic needed here.
        client->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("client did not read fast enough"));
    }
}
