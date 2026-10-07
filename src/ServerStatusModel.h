#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTimer>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;

// Periodically polls sightflow-server.exe's three read-only status
// endpoints for one channel -- GET /channels/<channelName>, GET
// /channels/<channelName>/metrics, GET /channels/<channelName>/events --
// over Qt's async network API and exposes the results as QML-bindable
// properties. Fully self-contained
// (owns its own QNetworkAccessManager and QTimer) -- unlike
// VideoDisplayItem, nothing needs to be wired in from main.cpp, so it can
// be declared directly in QML.
//
// `channelName` is a settable property (default "test") so QML can declare
// one instance per fixed channel -- e.g. `ServerStatusModel { channelName:
// "test2" }` -- each polling independently (docs/DECISIONS.md D21). The
// first poll is deferred to the next event-loop tick (QTimer::singleShot(0,
// ...)) rather than fired synchronously from the constructor, specifically
// so it runs *after* QML has finished assigning `channelName` (QML sets
// declared properties right after construction, before control returns to
// the event loop) -- firing synchronously in the constructor would poll
// whatever channelName's default value is, not the one QML declared.
//
// This is a read-only STATUS DISPLAY, entirely separate from the existing
// RTSP video path (RtspSource -> CaptureWorker -> FrameQueue ->
// VideoDisplayItem; docs/ARCHITECTURE.md §1-5): it shares no object, no
// thread, and no queue with it. If sightflow-server.exe is unreachable, the
// *Reachable properties below go false and the video path is completely
// unaffected -- see docs/DECISIONS.md D18.
//
// Every property pair below is deliberately split into "did this query
// reach the server at all" vs. "what did it say", because a request that
// fails outright (server down, timeout, malformed JSON) must not leave the
// last successful value looking current: callers must check *Reachable
// before trusting the paired value.
class ServerStatusModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    // Which channel this instance polls. Not a general N-channel list --
    // this project fixes exactly "test"/"test2" (D20/D21); QML simply
    // declares one ServerStatusModel per fixed channel.
    Q_PROPERTY(QString channelName READ channelName WRITE setChannelName NOTIFY channelNameChanged)

    // From GET /channels/<channelName> (MediaMTX's own view, via
    // sightflow-server -- see docs/DECISIONS.md D14). mediaMtxLive/
    // mediaMtxReachable are only meaningful when channelStatusReachable is
    // true.
    Q_PROPERTY(bool channelStatusReachable READ channelStatusReachable NOTIFY statusChanged)
    Q_PROPERTY(bool mediaMtxReachable READ mediaMtxReachable NOTIFY statusChanged)
    Q_PROPERTY(bool mediaMtxLive READ mediaMtxLive NOTIFY statusChanged)

    // From GET /channels/<channelName>/metrics -- sightflow-server.exe's
    // own DecodeWorker for this channel (D16/D17/D21). decodeState/
    // framesDecoded are only meaningful when decodeMetricsReachable is
    // true. framesDecoded is the SERVER's decode worker's count for its
    // current (or most recently ended) connection -- never frames this
    // client window has displayed.
    Q_PROPERTY(bool decodeMetricsReachable READ decodeMetricsReachable NOTIFY statusChanged)
    Q_PROPERTY(QString decodeState READ decodeState NOTIFY statusChanged)
    Q_PROPERTY(qlonglong framesDecoded READ framesDecoded NOTIFY statusChanged)

    // From GET /channels/<channelName>/events -- this channel's own
    // "화면 변화 감지" (screen change detection) history (D22). Only
    // meaningful when changeEventsReachable is true. Never to be read as a
    // person/object/motion detection result -- see docs/DECISIONS.md D22
    // for exactly what the underlying measurement is.
    Q_PROPERTY(bool changeEventsReachable READ changeEventsReachable NOTIFY statusChanged)

    // Up to kMaxDisplayedEvents (5) individual recent events, newest first,
    // each a QVariantMap{"time": QString ("hh:mm:ss", local time),
    // "ratio": double (0.0-1.0)} -- QML reads recentChangeEvents.length for
    // a count and recentChangeEvents[0] for "the last one" rather than
    // separate summary properties, so there is exactly one source of truth
    // for this data.
    Q_PROPERTY(QVariantList recentChangeEvents READ recentChangeEvents NOTIFY statusChanged)

public:
    explicit ServerStatusModel(QObject* parent = nullptr);

    QString channelName() const { return channelName_; }
    void setChannelName(const QString& name);

    bool channelStatusReachable() const noexcept { return channelStatusReachable_; }
    bool mediaMtxReachable() const noexcept { return mediaMtxReachable_; }
    bool mediaMtxLive() const noexcept { return mediaMtxLive_; }

    bool decodeMetricsReachable() const noexcept { return decodeMetricsReachable_; }
    QString decodeState() const { return decodeState_; }
    qlonglong framesDecoded() const noexcept { return framesDecoded_; }

    bool changeEventsReachable() const noexcept { return changeEventsReachable_; }
    QVariantList recentChangeEvents() const { return recentChangeEvents_; }

signals:
    void channelNameChanged();
    void statusChanged();

private slots:
    void poll();

private:
    void queryChannelStatus();
    void queryDecodeMetrics();
    void queryChangeEvents();

    static constexpr int kPollIntervalMs = 2000;
    static constexpr int kRequestTimeoutMs = 2000;
    static constexpr int kMaxDisplayedEvents = 5; // per-pane list is a glance, not a log viewer
    static const char* const kServerBaseUrl;

    QString channelName_ = QStringLiteral("test");

    QNetworkAccessManager* network_; // owned (parent = this)
    QTimer timer_;

    // Non-owning in-flight guards: non-null while a request is outstanding,
    // so poll() never issues a second request before the first finishes
    // (the actual QNetworkReply is always deleteLater()'d from its own
    // finished handler regardless of this pointer).
    QNetworkReply* channelStatusReply_ = nullptr;
    QNetworkReply* decodeMetricsReply_ = nullptr;
    QNetworkReply* changeEventsReply_ = nullptr;

    bool channelStatusReachable_ = false;
    bool mediaMtxReachable_ = false;
    bool mediaMtxLive_ = false;

    bool decodeMetricsReachable_ = false;
    QString decodeState_ = QStringLiteral("unknown");
    qlonglong framesDecoded_ = 0;

    bool changeEventsReachable_ = false;
    QVariantList recentChangeEvents_; // up to kMaxDisplayedEvents entries, newest first
};
