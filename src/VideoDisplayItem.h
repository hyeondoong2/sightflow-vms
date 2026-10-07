#pragma once

#include <QImage>
#include <QQmlEngine>
#include <QQuickPaintedItem>
#include <QTimer>

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
class VideoDisplayItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit VideoDisplayItem(QQuickItem* parent = nullptr);
    ~VideoDisplayItem() override;

    // Must be called once from C++ after the QML tree is loaded. Starts the
    // polling timer immediately.
    void setFrameQueue(FrameQueue* queue);

    void paint(QPainter* painter) override;

private slots:
    void pollFrameQueue();

private:
    FrameQueue* queue_ = nullptr;
    QTimer timer_;
    QImage image_; // independent copy -- never aliases a VideoFrame's buffer (see .cpp)
};
