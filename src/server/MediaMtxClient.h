#pragma once

#include <functional>

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

// Async client for MediaMTX's Control API (see docs/DECISIONS.md D14). Owns
// the single QNetworkAccessManager used for every query -- not per-request.
// Every method is non-blocking: no waitForFinished(), no blocking socket
// calls. Knows nothing about HTTP server connections or channel-name
// parsing (see ChannelStatusService) -- only "ask MediaMTX about one path".
class MediaMtxClient : public QObject {
    Q_OBJECT

public:
    // What MediaMTX reported about one path. `reachable == false` means
    // MediaMTX itself could not be queried (down, unreachable, timed out) --
    // in that case `live` carries no information and must not be read as
    // "confirmed not streaming".
    struct PathStatus {
        bool reachable = false;
        bool live = false;
    };

    explicit MediaMtxClient(QUrl apiBaseUrl, QObject* parent = nullptr);

    // Asynchronous, never blocks. `callback` fires exactly once, on this
    // object's thread, when the query completes -- including when it
    // completes via abortAll() during shutdown.
    void queryPathStatus(const QString& pathName, std::function<void(PathStatus)> callback);

    // Aborts every still-in-flight request. Each one still runs its
    // `finished` handling (callback included) with reachable == false, and
    // is deleted -- used on shutdown so nothing is left dangling.
    void abortAll();

private:
    static constexpr int kRequestTimeoutMs = 2000;

    QNetworkAccessManager network_;
    QUrl apiBaseUrl_;
    QList<QNetworkReply*> inFlight_; // non-owning; QNetworkReply is deleted via deleteLater() on finished
};
