#include "SnapshotDisplayItem.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QUrl>

#include "SnapshotDecoder.h"

namespace {
QString describeServerError(const QByteArray& body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return QStringLiteral("스냅샷을 불러올 수 없습니다");
    }
    const QString error = doc.object().value(QStringLiteral("error")).toString();
    if (error == QStringLiteral("event_not_found")) {
        return QStringLiteral("이벤트를 찾을 수 없습니다 (보관 기간 만료)");
    }
    if (error == QStringLiteral("snapshot_not_available")) {
        return QStringLiteral("이 이벤트에는 저장된 스냅샷이 없습니다");
    }
    return QStringLiteral("스냅샷을 불러올 수 없습니다");
}
} // namespace

SnapshotDisplayItem::SnapshotDisplayItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
    , network_(new QNetworkAccessManager(this))
{
}

void SnapshotDisplayItem::setSnapshotUrl(const QString& url)
{
    if (url_ == url) {
        return;
    }
    url_ = url;
    emit snapshotUrlChanged();

    if (url_.isEmpty()) {
        clear();
        return;
    }
    fetch();
}

void SnapshotDisplayItem::clear()
{
    inFlightUrl_.clear(); // a reply still in flight will see this no longer matches url_ and be discarded
    loading_ = false;
    image_ = QImage();
    errorText_.clear();
    emit stateChanged();
    update();
}

void SnapshotDisplayItem::fetch()
{
    image_ = QImage();
    errorText_.clear();
    loading_ = true;
    inFlightUrl_ = url_;
    emit stateChanged();
    update();

    const QUrl requestUrl(url_);
    QNetworkRequest request(requestUrl);
    request.setTransferTimeout(kRequestTimeoutMs);

    reply_ = network_->get(request);
    const QString requestedUrl = url_;
    connect(reply_, &QNetworkReply::finished, this, [this, requestedUrl]() {
        QNetworkReply* reply = reply_;
        reply_ = nullptr;
        reply->deleteLater();

        if (inFlightUrl_ != requestedUrl) {
            return; // superseded by a different selection (or cleared) while in flight -- discard
        }
        inFlightUrl_.clear();
        loading_ = false;

        const QVariant httpStatusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const QByteArray body = reply->readAll();

        if (!httpStatusAttr.isValid() || httpStatusAttr.toInt() != 200) {
            image_ = QImage();
            errorText_ = describeServerError(body);
            emit stateChanged();
            update();
            return;
        }

        std::string decodeError;
        std::optional<VideoFrame> frame = decodeJpegSnapshot(body, decodeError);
        if (!frame) {
            image_ = QImage();
            errorText_ = QStringLiteral("스냅샷 이미지를 해석할 수 없습니다");
            emit stateChanged();
            update();
            return;
        }

        // `view` aliases frame's buffer; .copy() deep-copies immediately so
        // image_ is fully independent once `frame` goes out of scope (D10,
        // same discipline VideoDisplayItem uses for live frames).
        QImage view(frame->pixels(), frame->width(), frame->height(), frame->strideBytes(), QImage::Format_ARGB32);
        image_ = view.copy();
        errorText_.clear();
        emit stateChanged();
        update();
    });
}

void SnapshotDisplayItem::paint(QPainter* painter)
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
