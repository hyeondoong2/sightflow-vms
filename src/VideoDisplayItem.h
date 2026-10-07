#pragma once

#include <QImage>
#include <QQmlEngine>
#include <QQuickPaintedItem>
#include <QString>
#include <QTimer>

#include "CaptureState.h"

class FrameQueue;

// QML-facing display surface for the single Phase 1 video stream
// (docs/ARCHITECTURE.md §2/§4). Owns the UI-thread QTimer that polls
// FrameQueue::tryPopLatest() on a fixed interval -- never connects to a
// per-frame worker signal (D7 in docs/DECISIONS.md).
//
// `queue` (set via setFrameQueue) is a non-owning pointer: the FrameQueue it
// points to is owned by main() and is guaranteed to outlive this item by
// main()'s local-variable declaration order (engine destroyed, tearing down
// this item and stopping its timer, before FrameQueue is destroyed).
// `captureState` (set via setCaptureState) is the same kind of non-owning
// pointer, read on the same timer tick -- see D19: since CaptureWorker now
// retries automatically, this item must not keep showing the last displayed
// frame while not actually connected.
class VideoDisplayItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    // CaptureWorker's own connection lifecycle ("connecting"/"retrying"/
    // "running"/"stopped") -- distinct from, and not to be confused with,
    // the server's own decode state shown by ServerStatusModel/D18. This is
    // the client's own RTSP connection to the camera, nothing about
    // sightflow-server.exe.
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)

public:
    explicit VideoDisplayItem(QQuickItem* parent = nullptr);
    ~VideoDisplayItem() override;

    // Must be called once from C++ after the QML tree is loaded. Starts the
    // polling timer immediately.
    void setFrameQueue(FrameQueue* queue);

    // May be called any time before or after setFrameQueue(); both are
    // polled on the same timer tick.
    void setCaptureState(CaptureState* captureState);

    QString connectionState() const { return connectionState_; }

    void paint(QPainter* painter) override;

signals:
    void connectionStateChanged();

private slots:
    void pollFrameQueue();

private:
    FrameQueue* queue_ = nullptr;
    CaptureState* captureState_ = nullptr;
    QTimer timer_;
    QImage image_; // independent copy -- never aliases a VideoFrame's buffer (see .cpp)
    QString connectionState_ = QStringLiteral("connecting");
};
