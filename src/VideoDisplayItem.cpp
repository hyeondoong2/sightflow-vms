#include "VideoDisplayItem.h"

#include <QPainter>

#include "FrameQueue.h"
#include "VideoFrame.h"

namespace {
constexpr int kPollIntervalMs = 33; // docs/ARCHITECTURE.md §5: fixed, independent of source fps
}

VideoDisplayItem::VideoDisplayItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    connect(&timer_, &QTimer::timeout, this, &VideoDisplayItem::pollFrameQueue);
}

VideoDisplayItem::~VideoDisplayItem()
{
    timer_.stop();
}

void VideoDisplayItem::setFrameQueue(FrameQueue* queue)
{
    queue_ = queue;
    timer_.start(kPollIntervalMs);
}

void VideoDisplayItem::pollFrameQueue()
{
    if (!queue_) {
        return;
    }

    std::optional<VideoFrame> frame = queue_->tryPopLatest();
    if (!frame) {
        return; // nothing new this tick -- keep showing the last image
    }

    // `view` aliases `frame`'s buffer (no copy yet); .copy() deep-copies into
    // QImage's own buffer immediately, so `image_` is fully independent
    // before `frame` goes out of scope and frees its buffer below (D10).
    QImage view(frame->pixels(), frame->width(), frame->height(), frame->strideBytes(), QImage::Format_ARGB32);
    image_ = view.copy();

    update();
}

void VideoDisplayItem::paint(QPainter* painter)
{
    if (image_.isNull()) {
        return;
    }

    const QSizeF targetSize = boundingRect().size();
    const QSizeF scaledSize = image_.size().scaled(targetSize.toSize(), Qt::KeepAspectRatio);
    const QRectF drawRect(
        (targetSize.width() - scaledSize.width()) / 2.0,
        (targetSize.height() - scaledSize.height()) / 2.0,
        scaledSize.width(),
        scaledSize.height());

    painter->drawImage(drawRect, image_);
}
