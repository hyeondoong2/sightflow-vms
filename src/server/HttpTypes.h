#pragma once

#include <functional>

#include <QByteArray>
#include <QString>

// Shared between the I/O layer (HttpServer/ClientConnection) and the
// request-handling layer (ChannelStatusService). Neither side needs to know
// the other's internals -- they only exchange these two callback shapes.

// Invoked by a handler exactly once to send the final response for one
// request. Safe to call after the originating connection has already closed
// -- ClientConnection guards its own lifetime before acting on this.
using RespondFn = std::function<void(int status, const QByteArray& contentType, const QByteArray& body)>;

// Invoked by ClientConnection once a full request line has been parsed.
// `respond` may be invoked synchronously or later (e.g. after an async
// MediaMTX query completes).
using RequestHandler = std::function<void(const QString& method, const QString& path, RespondFn respond)>;
