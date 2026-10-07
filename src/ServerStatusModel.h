#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

// Periodically polls sightflow-server.exe's two read-only status endpoints
// (GET /channels/test, GET /channels/test/metrics) over Qt's async network
// API and exposes the results as QML-bindable properties for a small status
// strip in Main.qml. Fully self-contained (owns its own
// QNetworkAccessManager and QTimer) -- unlike VideoDisplayItem, nothing
// needs to be wired in from main.cpp, so it can be declared directly in
// QML.
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

    // From GET /channels/test (MediaMTX's own view, via sightflow-server --
    // see docs/DECISIONS.md D14). mediaMtxLive/mediaMtxReachable are only
    // meaningful when channelStatusReachable is true.
    Q_PROPERTY(bool channelStatusReachable READ channelStatusReachable NOTIFY statusChanged)
    Q_PROPERTY(bool mediaMtxReachable READ mediaMtxReachable NOTIFY statusChanged)
    Q_PROPERTY(bool mediaMtxLive READ mediaMtxLive NOTIFY statusChanged)

    // From GET /channels/test/metrics -- sightflow-server.exe's own
    // DecodeWorker (D16/D17). decodeState/framesDecoded are only meaningful
    // when decodeMetricsReachable is true. framesDecoded is the SERVER's
    // decode worker's count for its current (or most recently ended)
    // connection -- never frames this client window has displayed.
    Q_PROPERTY(bool decodeMetricsReachable READ decodeMetricsReachable NOTIFY statusChanged)
    Q_PROPERTY(QString decodeState READ decodeState NOTIFY statusChanged)
    Q_PROPERTY(qlonglong framesDecoded READ framesDecoded NOTIFY statusChanged)

public:
    explicit ServerStatusModel(QObject* parent = nullptr);

    bool channelStatusReachable() const noexcept { return channelStatusReachable_; }
    bool mediaMtxReachable() const noexcept { return mediaMtxReachable_; }
    bool mediaMtxLive() const noexcept { return mediaMtxLive_; }

    bool decodeMetricsReachable() const noexcept { return decodeMetricsReachable_; }
    QString decodeState() const { return decodeState_; }
    qlonglong framesDecoded() const noexcept { return framesDecoded_; }

signals:
    void statusChanged();

private slots:
    void poll();

private:
    void queryChannelStatus();
    void queryDecodeMetrics();

    static constexpr int kPollIntervalMs = 2000;
    static constexpr int kRequestTimeoutMs = 2000;
    static const char* const kServerBaseUrl;
    static const char* const kChannelName;

    QNetworkAccessManager* network_; // owned (parent = this)
    QTimer timer_;

    // Non-owning in-flight guards: non-null while a request is outstanding,
    // so poll() never issues a second request before the first finishes
    // (the actual QNetworkReply is always deleteLater()'d from its own
    // finished handler regardless of this pointer).
    QNetworkReply* channelStatusReply_ = nullptr;
    QNetworkReply* decodeMetricsReply_ = nullptr;

    bool channelStatusReachable_ = false;
    bool mediaMtxReachable_ = false;
    bool mediaMtxLive_ = false;

    bool decodeMetricsReachable_ = false;
    QString decodeState_ = QStringLiteral("unknown");
    qlonglong framesDecoded_ = 0;
};
