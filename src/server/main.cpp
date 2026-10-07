#include <iostream>

#include <QCoreApplication>
#include <QUrl>

#ifdef _WIN32
#include <windows.h>
#endif

#include "ChannelStatusService.h"
#include "DecodeMetrics.h"
#include "DecodeMetricsService.h"
#include "DecodeWorker.h"
#include "HttpServer.h"
#include "MediaMtxClient.h"

namespace {
constexpr quint16 kListenPort = 8080;
const char* kMediaMtxApiBaseUrl = "http://127.0.0.1:9997";
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    MediaMtxClient mediaMtxClient(QUrl(QString::fromLatin1(kMediaMtxApiBaseUrl)));

    // ChannelStatusService is already channel-name-generic -- it forwards
    // whatever name is in the request path straight through to MediaMTX
    // (D14) -- so one instance already serves GET /channels/<any-name> for
    // both fixed channels below with no change.
    ChannelStatusService channelStatusService(mediaMtxClient);

    // Two fixed channels, "test" and "test2" (mirrors the client's D20):
    // each gets its own independent DecodeWorker thread + DecodeMetrics +
    // DecodeMetricsService, by literal duplication, not a channel
    // registry/array (docs/DECISIONS.md D21). Neither channel's worker
    // thread, metrics, or retry/shutdown behavior ever touches the other's.
    DecodeMetrics decodeMetricsTest;
    DecodeWorker decodeWorkerTest("rtsp://127.0.0.1:8554/test", decodeMetricsTest);
    decodeWorkerTest.start();
    DecodeMetricsService decodeMetricsServiceTest(QStringLiteral("test"), decodeMetricsTest);

    DecodeMetrics decodeMetricsTest2;
    DecodeWorker decodeWorkerTest2("rtsp://127.0.0.1:8554/test2", decodeMetricsTest2);
    decodeWorkerTest2.start();
    DecodeMetricsService decodeMetricsServiceTest2(QStringLiteral("test2"), decodeMetricsTest2);

    HttpServer httpServer;
    httpServer.setHandler([&channelStatusService, &decodeMetricsServiceTest, &decodeMetricsServiceTest2](
                               const QString& method, const QString& path, RespondFn respond) {
        // Exact-path routing for the two fixed metrics endpoints -- not a
        // generic "strip /metrics and look up a channel" dispatcher, since
        // there are deliberately only two channels to route to (D21).
        if (path == QStringLiteral("/channels/test/metrics")) {
            decodeMetricsServiceTest.handleRequest(method, path, std::move(respond));
        } else if (path == QStringLiteral("/channels/test2/metrics")) {
            decodeMetricsServiceTest2.handleRequest(method, path, std::move(respond));
        } else if (path.endsWith(QStringLiteral("/metrics"))) {
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
               << " (MediaMTX API at " << kMediaMtxApiBaseUrl << ")" << std::endl;

    // Shutdown: stop accepting + destroy every open connection first (so
    // their response callbacks' QPointer guards go null), then abort any
    // MediaMTX request still in flight -- in that order, nothing tries to
    // write to an already-closed socket. Both DecodeWorkers are stopped
    // last, one after the other: each stop() interrupts a blocking RTSP
    // I/O call if one is in progress (AVIOInterruptCB, same as
    // CaptureWorker §7), or wakes an in-progress retry-backoff wait
    // immediately (D17) if between attempts -- either way it then joins
    // that worker's thread before the next stop() call begins. Sequential,
    // not parallel, since each is independently fast (bounded by one
    // interrupt-callback poll or an immediate condition_variable wake) and
    // two fixed channels don't warrant added complexity to overlap them.
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
        [&httpServer, &mediaMtxClient, &decodeWorkerTest, &decodeWorkerTest2]() {
            httpServer.shutdown();
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
