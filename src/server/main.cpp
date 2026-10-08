#include <iostream>
#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QUrl>

#ifdef _WIN32
#include <windows.h>
#endif

#include "ChangeEventLog.h"
#include "ChangeEventService.h"
#include "ChannelStatusService.h"
#include "DecodeMetrics.h"
#include "DecodeMetricsService.h"
#include "DecodeWorker.h"
#include "EventStore.h"
#include "HttpServer.h"
#include "MediaMtxClient.h"
#include "WebSocketNotifier.h"

namespace {
constexpr quint16 kListenPort = 8080;

// WebSocket push-notification port (D25) -- deliberately separate from
// kListenPort: QWebSocketServer owns its own listen socket and does its own
// HTTP Upgrade handshake internally, which cannot share a port with the
// hand-rolled, GET-only HttpServer/ClientConnection pair above (D14) without
// teaching that minimal parser to recognize and hand off an Upgrade
// request -- deliberately not done, to keep ClientConnection's "exactly one
// minimal HTTP request per connection" model unchanged. Both are plain,
// unauthenticated, 127.0.0.1-only sockets, same trust boundary as the
// existing REST API (no auth key/token exists anywhere in this project to
// keep out of source control -- D25 adds none). A client reaches channel
// "test"'s notifications at ws://127.0.0.1:8081/channels/test (and "test2"
// the same way) -- see WebSocketNotifier.h for the path-routing rule.
constexpr quint16 kWebSocketPort = 8081;

const char* kMediaMtxApiBaseUrl = "http://127.0.0.1:9997";

// How many of a channel's most recent "화면 변화 감지" events stay in
// memory (ChangeEventLog, D22) *and* on disk (EventStore, D24) -- the two
// are always kept at the same bound, so neither can hold history the other
// doesn't. 20 is enough to show a short recent history per channel without
// either growing unbounded; older events (and their on-disk rows/snapshot
// blobs) are simply dropped, not archived anywhere else.
constexpr std::size_t kChangeEventLogCapacity = 20;

// Loads this channel's persisted events (if any) into `log`, via a
// transient EventStore connection opened just for this call -- used only
// here, on the main thread, before any DecodeWorker thread exists (D24).
// Never fatal: a missing/unreadable store just leaves `log` empty, exactly
// as if this feature didn't exist.
void restoreChannelHistory(EventStore* startupStore, const QString& channelName, ChangeEventLog& log)
{
    if (!startupStore) {
        return;
    }
    std::vector<ChangeEventLog::PersistedEntry> entries;
    QString loadError;
    if (!startupStore->loadRecent(channelName, kChangeEventLogCapacity, entries, loadError)) {
        std::cerr << "sightflow-server: failed to load persisted events for '" << channelName.toStdString()
                   << "' (" << loadError.toStdString() << ") -- starting with an empty history for this channel\n";
        return;
    }
    if (!entries.empty()) {
        std::cout << "sightflow-server: restored " << entries.size() << " persisted event(s) for '"
                   << channelName.toStdString() << "'\n";
    }
    log.restoreFromPersisted(std::move(entries));
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // Named so QStandardPaths::AppLocalDataLocation resolves to a stable,
    // per-user path (e.g. "<user>/AppData/Local/SightFlowVMS" on Windows)
    // computed at runtime -- never a machine-specific absolute path literal
    // in source or config (docs/DECISIONS.md D24). Outside both the
    // repository and any CMake build directory, so it survives a clean
    // rebuild and isn't picked up by git. Deliberately AppLocalDataLocation,
    // not the plain AppDataLocation: on Windows the latter maps to the
    // *Roaming* profile (synced across machines in managed/domain setups),
    // which is the wrong place for a growing binary SQLite file with JPEG
    // blobs in it -- AppLocalDataLocation maps to the Local profile
    // (explicitly excluded from roaming), the conventional home for this
    // kind of per-machine application data/cache.
    QCoreApplication::setApplicationName(QStringLiteral("SightFlowVMS"));
    const QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(appDataDir); // idempotent; also creates any missing parent directories
    const QString dbFilePath = appDataDir + QStringLiteral("/events.sqlite3");

    MediaMtxClient mediaMtxClient(QUrl(QString::fromLatin1(kMediaMtxApiBaseUrl)));

    // ChannelStatusService is already channel-name-generic -- it forwards
    // whatever name is in the request path straight through to MediaMTX
    // (D14) -- so one instance already serves GET /channels/<any-name> for
    // both fixed channels below with no change.
    ChannelStatusService channelStatusService(mediaMtxClient);

    // Two fixed channels, "test" and "test2" (mirrors the client's D20):
    // each gets its own independent DecodeWorker thread + DecodeMetrics +
    // DecodeMetricsService + ChangeEventLog + ChangeEventService, by literal
    // duplication, not a channel registry/array (D21/D22). Neither
    // channel's worker thread, metrics, retry/shutdown behavior, or change
    // detection state ever touches the other's.
    DecodeMetrics decodeMetricsTest;
    ChangeEventLog changeEventLogTest(kChangeEventLogCapacity);
    DecodeMetrics decodeMetricsTest2;
    ChangeEventLog changeEventLogTest2(kChangeEventLogCapacity);

    // One WebSocket push-notification listener for both channels (D25) --
    // declared before either DecodeWorker below so it is destroyed *after*
    // them (C++ reverse-declaration-order destruction): both workers hold a
    // reference to it and may still have a queued notify call in flight
    // right up until stop() joins their threads during shutdown. A failed
    // listen() is logged, not fatal -- sightflow-server keeps serving
    // HTTP/RTSP exactly as before; connected clients simply fall back to
    // their existing periodic REST polling with no push notifications this
    // run (same resilience shape as D24's persistence).
    WebSocketNotifier wsNotifier(changeEventLogTest, changeEventLogTest2);
    if (!wsNotifier.listen(kWebSocketPort)) {
        std::cerr << "sightflow-server: WebSocket notifications unavailable (failed to listen on port "
                   << kWebSocketPort << ") -- clients will still work via periodic REST polling\n";
    }

    // Startup load (D24): one transient connection on the main thread,
    // opened, used for both channels, and destroyed here -- strictly before
    // either DecodeWorker thread (each opening its own connection to the
    // same file) is started below, so there is no concurrent access to the
    // database yet at this point. Never fatal: if persistence is
    // unavailable at all (driver missing, directory not writable, ...),
    // this logs once and both channels simply start with an empty
    // in-memory log, exactly like before this feature existed.
    {
        QString storeError;
        std::unique_ptr<EventStore> startupStore =
            EventStore::open(QStringLiteral("events-startup"), dbFilePath, storeError);
        if (!startupStore) {
            std::cerr << "sightflow-server: persistence unavailable (" << storeError.toStdString()
                       << ") -- continuing with in-memory-only event history for both channels\n";
        }
        restoreChannelHistory(startupStore.get(), QStringLiteral("test"), changeEventLogTest);
        restoreChannelHistory(startupStore.get(), QStringLiteral("test2"), changeEventLogTest2);
        // `startupStore` destroyed here (end of scope) -- its connection is
        // closed before DecodeWorkerTest/DecodeWorkerTest2 start and open
        // their own.
    }

    DecodeWorker decodeWorkerTest("rtsp://127.0.0.1:8554/test", "test", decodeMetricsTest, changeEventLogTest,
        dbFilePath, kChangeEventLogCapacity, wsNotifier);
    decodeWorkerTest.start();
    DecodeMetricsService decodeMetricsServiceTest(QStringLiteral("test"), decodeMetricsTest);
    ChangeEventService changeEventServiceTest(QStringLiteral("test"), changeEventLogTest);

    DecodeWorker decodeWorkerTest2("rtsp://127.0.0.1:8554/test2", "test2", decodeMetricsTest2, changeEventLogTest2,
        dbFilePath, kChangeEventLogCapacity, wsNotifier);
    decodeWorkerTest2.start();
    DecodeMetricsService decodeMetricsServiceTest2(QStringLiteral("test2"), decodeMetricsTest2);
    ChangeEventService changeEventServiceTest2(QStringLiteral("test2"), changeEventLogTest2);

    HttpServer httpServer;
    httpServer.setHandler([&channelStatusService, &decodeMetricsServiceTest, &decodeMetricsServiceTest2,
                               &changeEventServiceTest, &changeEventServiceTest2](
                               const QString& method, const QString& path, RespondFn respond) {
        // Exact-path routing for the fixed per-channel endpoints -- not a
        // generic "strip the suffix and look up a channel" dispatcher,
        // since there are deliberately only two channels to route to
        // (D21/D22). GET /channels/<name> and GET /channels/<name>/metrics
        // keep their existing response contracts unchanged (D14/D16/D17);
        // GET /channels/<name>/events and its snapshot sub-route (D22/D23)
        // are unaffected by D24 -- still served entirely from in-memory
        // ChangeEventLog, never from the database.
        if (path == QStringLiteral("/channels/test/metrics")) {
            decodeMetricsServiceTest.handleRequest(method, path, std::move(respond));
        } else if (path == QStringLiteral("/channels/test2/metrics")) {
            decodeMetricsServiceTest2.handleRequest(method, path, std::move(respond));
        } else if (path == QStringLiteral("/channels/test/events")
                   || path.startsWith(QStringLiteral("/channels/test/events/"))) {
            // Covers both the plain event list and the per-event
            // "/events/<id>/snapshot" sub-route (D23) -- ChangeEventService
            // itself distinguishes the two; any other sub-path under
            // ".../events/" that it doesn't recognize is its own 404.
            changeEventServiceTest.handleRequest(method, path, std::move(respond));
        } else if (path == QStringLiteral("/channels/test2/events")
                   || path.startsWith(QStringLiteral("/channels/test2/events/"))) {
            changeEventServiceTest2.handleRequest(method, path, std::move(respond));
        } else if (path.endsWith(QStringLiteral("/metrics")) || path.endsWith(QStringLiteral("/events"))
                   || path.contains(QStringLiteral("/events/"))) {
            respond(404, "text/plain", "Not Found");
        } else {
            channelStatusService.handleRequest(method, path, std::move(respond));
        }
    });

    if (!httpServer.listen(kListenPort)) {
        std::cerr << "sightflow-server: failed to listen on port " << kListenPort << std::endl;
        return -1;
    }
    std::cout << "sightflow-server: listening on http://127.0.0.1:" << kListenPort
               << " (MediaMTX API at " << kMediaMtxApiBaseUrl << "), WebSocket notifications on ws://127.0.0.1:"
               << kWebSocketPort << std::endl;

    // Shutdown: stop accepting + destroy every open connection first (so
    // their response callbacks' QPointer guards go null), then abort any
    // MediaMTX request still in flight -- in that order, nothing tries to
    // write to an already-closed socket. Both DecodeWorkers are stopped
    // last, one after the other: each stop() interrupts a blocking RTSP
    // I/O call if one is in progress (AVIOInterruptCB, same as
    // CaptureWorker §7), or wakes an in-progress retry-backoff wait
    // immediately (D17) if between attempts -- either way it then joins
    // that worker's thread before the next stop() call begins (this also
    // closes that thread's own EventStore connection, D24, since it is a
    // local variable in DecodeWorker::run()). Sequential, not parallel,
    // since each is independently fast (bounded by one interrupt-callback
    // poll or an immediate condition_variable wake) and two fixed channels
    // don't warrant added complexity to overlap them.
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
        [&httpServer, &wsNotifier, &mediaMtxClient, &decodeWorkerTest, &decodeWorkerTest2]() {
            httpServer.shutdown();
            wsNotifier.shutdown();
            mediaMtxClient.abortAll();
            decodeWorkerTest.stop();
            decodeWorkerTest2.stop();
        });

#ifdef _WIN32
    // Ctrl+C / console close: SetConsoleCtrlHandler's callback runs on a
    // separate thread, but QCoreApplication::quit() is documented as safe to
    // call from any thread (it only posts a quit event to the main loop).
    SetConsoleCtrlHandler(
        [](DWORD) -> BOOL {
            QCoreApplication::quit();
            return TRUE;
        },
        TRUE);
#endif

    return app.exec();
}
