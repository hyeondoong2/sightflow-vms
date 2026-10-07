#pragma once

#include <QByteArray>
#include <QObject>

#include "HttpTypes.h"

class QTcpSocket;

// Owns one accepted client socket for its whole lifetime: reads/parses
// exactly one minimal HTTP request line, invokes the handler, writes the
// response, then closes. Knows nothing about MediaMTX or channels -- only
// raw HTTP I/O (see docs/DECISIONS.md D14).
//
// Lifetime: owned by HttpServer via connections_. Destroyed either when the
// socket disconnects (response sent and flushed, or the client hung up
// early) or when HttpServer::shutdown() tears down all connections. Any
// response callback still pending against a destroyed ClientConnection is a
// safe no-op (RespondFn captures a QPointer guard to `this`, not a raw
// pointer).
class ClientConnection : public QObject {
    Q_OBJECT

public:
    ClientConnection(QTcpSocket* socket, RequestHandler handler, QObject* parent = nullptr);

    // Immediately closes the underlying socket (no graceful flush/close --
    // used only during HttpServer::shutdown()). Does not delete `this`; the
    // caller is responsible for that (via deleteLater()).
    void abortConnection();

signals:
    // Emitted exactly once, right before this object should be removed from
    // its owner's connection list (the owner deletes it in response).
    void finished(ClientConnection* self);

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    void tryParseRequest();
    void sendResponse(int status, const QByteArray& contentType, const QByteArray& body);
    void failAndClose(int status, const QByteArray& message);

    static constexpr int kMaxRequestBytes = 8192;

    QTcpSocket* socket_; // child of this -- owned
    RequestHandler handler_;
    QByteArray buffer_;
    bool responded_ = false;
};
