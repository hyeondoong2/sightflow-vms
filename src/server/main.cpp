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
const char* kChannelName = "test";
const char* kChannelRtspUrl = "rtsp://127.0.0.1:8554/test";
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    MediaMtxClient mediaMtxClient(QUrl(QString::fromLatin1(kMediaMtxApiBaseUrl)));
    ChannelStatusService channelStatusService(mediaMtxClient);

    // Decode worker: one dedicated thread, started once, decoding the one
    // test channel for this step (docs/DECISIONS.md D16). DecodeMetrics is
    // the only thing that crosses into the Qt event-loop thread.
    DecodeMetrics decodeMetrics;
    DecodeWorker decodeWorker(kChannelRtspUrl, decodeMetrics);
    decodeWorker.start();

    DecodeMetricsService decodeMetricsService(QString::fromLatin1(kChannelName), decodeMetrics);

    HttpServer httpServer;
    httpServer.setHandler([&channelStatusService, &decodeMetricsService](
                               const QString& method, const QString& path, RespondFn respond) {
        if (path.endsWith(QStringLiteral("/metrics"))) {
            decodeMetricsService.handleRequest(method, path, std::move(respond));
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
    // write to an already-closed socket. DecodeWorker::stop() runs last: it
    // interrupts its blocking RTSP I/O (same AVIOInterruptCB mechanism as
    // CaptureWorker, §7) and joins the thread -- a bounded wait, not
    // unconditional, but still a synchronous one on the Qt thread here.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&httpServer, &mediaMtxClient, &decodeWorker]() {
        httpServer.shutdown();
        mediaMtxClient.abortAll();
        decodeWorker.stop();
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
