#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QWebSocketServer>

#include "ChangeEventLog.h"

class QWebSocket;

// Owns the WebSocket listen socket that pushes a small "a new event exists"
// notification to connected clients the moment either channel's
// DecodeWorker records one -- so a running Qt client can refresh that
// channel's event list (GET /channels/<name>/events, unchanged, D22/D23)
// without waiting for its next periodic poll (docs/DECISIONS.md D25).
//
// Entirely a Qt-event-loop-thread object: the listen socket, every accepted
// QWebSocket, and all send/receive/close handling live here and nowhere
// else. DecodeWorker (a plain std::thread, not a QObject) never touches a
// QWebSocket or any socket this class owns -- the only thing it does is
// call requestNotifyTestChanged()/requestNotifyTest2Changed() (below),
// which is safe to call directly from a foreign thread (it only ever
// touches a plain std::atomic<bool>, never Qt's meta-object system or a
// socket) and internally posts a QMetaObject::invokeMethod(...,
// Qt::QueuedConnection) call -- the Qt-sanctioned way to safely reach a
// QObject living on a different thread -- only when one is not already
// outstanding for that channel. That queued call carries no payload at all
// (not even the channel name) -- it is purely a "go check channel X now"
// doorbell; the actual event data broadcast comes from re-reading that
// channel's own already-thread-safe ChangeEventLog (D22) at the moment
// this object's own thread processes the call, never from anything
// DecodeWorker constructed or passed across the boundary.
//
// Two independent, explicit bounds on pending notifications (per the
// project's "no unbounded queue, anywhere" rule) -- these are separate
// concerns, not one mechanism wearing two hats:
//   1. How many *queued invokeMethod calls* can ever be outstanding for one
//      channel at a time: at most ONE, enforced by testNotifyPending_/
//      test2NotifyPending_ (a plain atomic compare-and-set claimed by
//      requestNotifyTestChanged()/requestNotifyTest2Changed() before
//      posting, cleared by notifyTestChanged()/notifyTest2Changed() as the
//      first thing they do once actually processed). Without this, nothing
//      stops DecodeWorker from posting a new queued call for every single
//      event while the Qt event loop is merely busy, not stalled forever --
//      the count of pending QMetaCallEvents in Qt's own internal queue
//      would then grow for as long as the stall lasts, which is exactly
//      the unbounded-growth shape this project's rules forbid.
//   2. How many *broadcast messages* can ever sit unsent in one client's own
//      QWebSocket write buffer: capped at kMaxBufferedBytesPerClient
//      (notifyChannelChanged() checks QWebSocket::bytesToWrite() after each
//      send and closes/drops any client over the limit). Without this nothing
//      stops a client that connects but never reads from accumulating an
//      ever-growing backlog in its own socket's send buffer for as long as
//      it stays connected.
// Neither is the same thing as lastBroadcastIdTest_/lastBroadcastIdTest2_,
// which only prevents sending a *duplicate* WebSocket message for an event
// already broadcast -- that check runs only once a queued call is actually
// processed, so by itself it does nothing to bound how many such calls (or
// how much per-client buffered data) can accumulate before that happens.
//
// Two fixed channels (mirrors D20-D24's literal-duplication style): one
// listen socket (can't have two sockets on one port), but every piece of
// per-channel state -- the connected-client list, the dedupe id, the
// pending-notify flag -- is two separate members, never a channel-keyed
// map/registry. A client's request path ("/channels/test" or
// "/channels/test2") decides which bucket it joins; QWebSocketServer
// completes the WebSocket handshake itself before this class ever sees the
// connection, so an unrecognized path is simply closed immediately after
// accepting it, not rejected during the handshake.
class WebSocketNotifier : public QObject {
    Q_OBJECT

public:
    // `changeEventLogTest`/`changeEventLogTest2` must outlive this object --
    // same lifetime contract as DecodeWorker's own ChangeEventLog&
    // reference (D22).
    WebSocketNotifier(ChangeEventLog& changeEventLogTest, ChangeEventLog& changeEventLogTest2, QObject* parent = nullptr);

    // Returns false (and the caller should log, not treat as fatal) if the
    // port could not be bound -- sightflow-server keeps running HTTP/RTSP
    // decode exactly as before either way; clients simply fall back to
    // their existing periodic REST polling without push notifications this
    // run (D25's resilience policy, same shape as D24's for persistence).
    bool listen(quint16 port);

    // Stops accepting new connections and closes every connected client --
    // mirrors HttpServer::shutdown()'s discipline exactly (D15): snapshots
    // and clears each channel's client list before touching any socket, so
    // a synchronous close -> disconnected re-entry cannot mutate the list
    // mid-iteration; every actual C++ destruction goes through
    // deleteLater(), never a direct delete from inside a signal handler.
    void shutdown();

    // Safe to call directly from a DecodeWorker thread (never from this
    // object's own thread) -- touches only a plain std::atomic<bool>, no
    // Qt meta-object machinery, no socket. Posts exactly one
    // notifyTestChanged()/notifyTest2Changed() queued call if none is
    // already outstanding for this channel; otherwise does nothing (the
    // already-queued call will still pick up this event, since it re-reads
    // ChangeEventLog's current newest id rather than carrying this specific
    // event as a payload) -- see the class comment above for why this is a
    // separate bound from the dedupe-by-last-broadcast-id check.
    void requestNotifyTestChanged();
    void requestNotifyTest2Changed();

private slots:
    void onNewConnection();

private:
    // Run only via QMetaObject::invokeMethod(..., Qt::QueuedConnection),
    // posted exclusively by requestNotifyTestChanged()/
    // requestNotifyTest2Changed() above -- never called any other way.
    void notifyTestChanged();
    void notifyTest2Changed();

    void notifyChannelChanged(const QString& channelName, ChangeEventLog& log, std::uint64_t& lastBroadcastId,
        std::vector<QWebSocket*>& clients);
    void closeAllClients(std::vector<QWebSocket*>& clients);

    // Generous, explicit ceiling on one client's own unsent-data backlog --
    // not measured against a real slow client, chosen the same way other
    // initial-value constants in this codebase are (D9's FrameQueue
    // capacity, D22's detection thresholds): big enough that an ordinarily-
    // slow-but-alive client (briefly busy, not permanently stuck) is never
    // mistaken for a dead one -- at ~100 bytes/message and roughly one
    // message per ChangeDetector::kEventCooldown (3s) per channel, 64 KiB is
    // many hundreds of messages' worth, tens of minutes of a client reading
    // nothing at all -- while still being a real, enforced bound rather
    // than none.
    static constexpr qint64 kMaxBufferedBytesPerClient = 64 * 1024;

    ChangeEventLog& changeEventLogTest_;
    ChangeEventLog& changeEventLogTest2_;

    QWebSocketServer server_;
    std::vector<QWebSocket*> testClients_;
    std::vector<QWebSocket*> test2Clients_;

    std::uint64_t lastBroadcastIdTest_ = 0;
    std::uint64_t lastBroadcastIdTest2_ = 0;

    // At most one outstanding notifyTestChanged()/notifyTest2Changed()
    // queued call per channel -- see the class comment's bound (1) above.
    std::atomic<bool> testNotifyPending_{false};
    std::atomic<bool> test2NotifyPending_{false};
};
