#include "HttpServer.h"

#include <algorithm>

#include <QHostAddress>
#include <QTcpSocket>

#include "ClientConnection.h"

HttpServer::HttpServer(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

void HttpServer::setHandler(RequestHandler handler)
{
    handler_ = std::move(handler);
}

bool HttpServer::listen(quint16 port)
{
    return server_.listen(QHostAddress::Any, port);
}

void HttpServer::onNewConnection()
{
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
        auto* connection = new ClientConnection(socket, handler_, this);
        connect(connection, &ClientConnection::finished, this, &HttpServer::onConnectionFinished);
        connections_.push_back(connection);
    }
}

void HttpServer::onConnectionFinished(ClientConnection* connection)
{
    const auto it = std::find(connections_.begin(), connections_.end(), connection);
    if (it != connections_.end()) {
        connections_.erase(it);
    }
    // Never delete here: onConnectionFinished runs synchronously from within
    // `connection`'s own disconnected-socket signal handling, and deleting a
    // QObject mid-emission of its own signal is undefined behavior (D15).
    connection->deleteLater();
}

void HttpServer::shutdown()
{
    server_.close(); // stop accepting new connections

    // Snapshot and clear the member list *before* touching any connection:
    // abortConnection() can synchronously re-enter onConnectionFinished()
    // (socket abort -> disconnected signal -> finished signal), which would
    // otherwise mutate connections_ while this loop is iterating it.
    const std::vector<ClientConnection*> toClose = std::move(connections_);
    connections_.clear();

    for (ClientConnection* connection : toClose) {
        disconnect(connection, &ClientConnection::finished, this, &HttpServer::onConnectionFinished);
        connection->abortConnection(); // release the OS socket now, synchronously
        connection->deleteLater(); // defer C++ destruction -- see D15
    }
}
