#pragma once

#include "HttpTypes.h"
#include "MediaMtxClient.h"

// Translates HTTP requests into MediaMTX path-status queries, and the
// result back into a JSON response. This is the "channel state" layer: it
// knows the GET /channels/<name> route and what MediaMTX's `ready` field
// means, but nothing about sockets or HTTP parsing (that's HttpServer/
// ClientConnection). Does not own the request/connection lifetime -- see
// RespondFn in HttpTypes.h for how that's guarded.
class ChannelStatusService {
public:
    explicit ChannelStatusService(MediaMtxClient& mediaMtxClient);

    // Matches RequestHandler's shape (see HttpTypes.h) -- can be bound
    // directly as HttpServer's handler.
    void handleRequest(const QString& method, const QString& path, RespondFn respond);

private:
    MediaMtxClient& mediaMtxClient_;
};
