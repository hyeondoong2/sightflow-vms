#include "ClientConnection.h"

#include <QPointer>
#include <QTcpSocket>

namespace {
const char* reasonPhrase(int status)
{
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}
} // namespace

ClientConnection::ClientConnection(QTcpSocket* socket, RequestHandler handler, QObject* parent)
    : QObject(parent)
    , socket_(socket)
    , handler_(std::move(handler))
{
    socket_->setParent(this);
    connect(socket_, &QTcpSocket::readyRead, this, &ClientConnection::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, &ClientConnection::onDisconnected);
}

void ClientConnection::onReadyRead()
{
    buffer_.append(socket_->readAll());
    tryParseRequest();
}

void ClientConnection::tryParseRequest()
{
    if (responded_) {
        return; // ignore anything further on this connection
    }

    const int headerEnd = buffer_.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer_.size() > kMaxRequestBytes) {
            failAndClose(400, "Request too large");
        }
        return; // wait for more data
    }

    const QByteArray requestLine = buffer_.left(buffer_.indexOf("\r\n"));
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2) {
        failAndClose(400, "Malformed request line");
        return;
    }

    const QString method = QString::fromLatin1(parts[0]);
    QString target = QString::fromLatin1(parts[1]);
    const int queryStart = target.indexOf('?');
    if (queryStart >= 0) {
        target.truncate(queryStart);
    }

    if (!handler_) {
        failAndClose(500, "No handler configured");
        return;
    }

    QPointer<ClientConnection> self(this);
    handler_(method, target, [self](int status, const QByteArray& contentType, const QByteArray& body) {
        if (!self) {
            return; // connection already closed/destroyed -- nothing to write to
        }
        self->sendResponse(status, contentType, body);
    });
}

void ClientConnection::sendResponse(int status, const QByteArray& contentType, const QByteArray& body)
{
    if (responded_) {
        return;
    }
    responded_ = true;

    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(status) + " " + reasonPhrase(status) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;

    socket_->write(response);
    socket_->disconnectFromHost(); // flushes pending writes, then emits disconnected()
}

void ClientConnection::failAndClose(int status, const QByteArray& message)
{
    sendResponse(status, "text/plain", message);
}

void ClientConnection::onDisconnected()
{
    emit finished(this);
}

void ClientConnection::abortConnection()
{
    responded_ = true; // suppress any further response attempts
    socket_->abort();
}
