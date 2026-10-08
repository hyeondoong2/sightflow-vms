#pragma once

#include <QImage>
#include <QQmlEngine>
#include <QQuickPaintedItem>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// QML-facing display surface for one selected "화면 변화 감지" event's JPEG
// snapshot (docs/DECISIONS.md D23) -- a static still image fetched over
// HTTP, decoded via FFmpeg (SnapshotDecoder.h, the same mjpeg-decode-then-
// BGRA32-QImage-copy path VideoDisplayItem's live frames already use, D10/
// D12) and painted with the same KeepAspectRatio logic as VideoDisplayItem.
//
// Fully self-contained (owns its own QNetworkAccessManager), like
// ServerStatusModel -- no main.cpp wiring needed.
//
// `snapshotUrl` is the one thing QML drives: setting it to a non-empty
// absolute URL fetches and displays that snapshot; setting it back to an
// empty string immediately clears whatever was shown -- no stale image is
// ever left on screen. This is exactly how Main.qml clears the view when an
// event is deselected, no longer in the recent list, or the server becomes
// unreachable. A reply for a URL that is no longer the current
// `snapshotUrl` by the time it finishes (the user picked a different event,
// or cleared the selection, while the first request was still in flight) is
// discarded, never applied -- the same "never show stale data" principle
// VideoDisplayItem already follows for the live view (D19).
class SnapshotDisplayItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString snapshotUrl READ snapshotUrl WRITE setSnapshotUrl NOTIFY snapshotUrlChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

public:
    explicit SnapshotDisplayItem(QQuickItem* parent = nullptr);

    QString snapshotUrl() const { return url_; }
    void setSnapshotUrl(const QString& url);

    bool loading() const noexcept { return loading_; }
    bool hasImage() const { return !image_.isNull(); }
    QString errorText() const { return errorText_; }

    void paint(QPainter* painter) override;

signals:
    void snapshotUrlChanged();
    void stateChanged();

private:
    void fetch();
    void clear();

    static constexpr int kRequestTimeoutMs = 3000;

    QNetworkAccessManager* network_; // owned (parent = this)
    QNetworkReply* reply_ = nullptr; // non-owning in-flight guard, same pattern as ServerStatusModel

    QString url_;
    QString inFlightUrl_; // the url_ value the current reply_ was issued for; empty if none in flight
    QImage image_;
    bool loading_ = false;
    QString errorText_;
};
