#pragma once

#include <vector>

#include <QObject>
#include <QTcpServer>

#include "HttpTypes.h"

class ClientConnection;

// Minimal event-loop-based HTTP server: no Qt HttpServer module (not
// installed in this project's Qt kit -- see docs/DECISIONS.md D14), no
// worker threads. QTcpServer + one ClientConnection per accepted socket,
// both driven entirely by the Qt event loop on the thread that calls
// listen(). Never blocks; concurrent connections are handled naturally by
// the event loop interleaving their I/O.
//
// Knows nothing about MediaMTX or channels -- setHandler() supplies that via
// a plain callback (RequestHandler, see HttpTypes.h).
class HttpServer : public QObject {
    Q_OBJECT

public:
    explicit HttpServer(QObject* parent = nullptr);

    // Must be called before listen().
    void setHandler(RequestHandler handler);

    // Returns false (and logs) if the port could not be bound.
    bool listen(quint16 port);

    // Stops accepting new connections, synchronously aborts every still-open
    // connection's socket (so OS resources are released immediately), and
    // schedules each connection object for deletion via deleteLater() --
    // never a direct delete here, since shutdown() can itself be called from
    // within a connection's own signal handling (see D15). Idempotent.
    void shutdown();

private slots:
    void onNewConnection();
    void onConnectionFinished(ClientConnection* connection);

private:
    // Non-owning: ownership is QObject parent/child (parent = this). Entries
    // are removed here as soon as a connection finishes or shutdown() runs;
    // actual C++ destruction always happens via deleteLater(), never a
    // direct `delete` (D15).
    QTcpServer server_;
    RequestHandler handler_;
    std::vector<ClientConnection*> connections_;
};
