#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "CaptureState.h"
#include "CaptureWorker.h"
#include "FrameQueue.h"
#include "VideoDisplayItem.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    const std::string url = "rtsp://127.0.0.1:8554/test";

    // Declaration order matters: at scope exit below, C++ destroys these in
    // reverse order, which is exactly the shutdown sequence
    // docs/ARCHITECTURE.md §7 requires -- `engine` first (tearing down the
    // QML tree, which stops VideoDisplayItem's poll timer), then `worker`
    // (CaptureWorker::~CaptureWorker() requests stop -- interrupting both a
    // blocking RTSP call and an in-progress retry-backoff wait, D19 -- and
    // joins the capture thread), then `captureState`/`queue` last, once
    // nothing can read or write them anymore.
    FrameQueue queue;
    CaptureState captureState;
    CaptureWorker worker(url, queue, captureState);
    worker.start();

    QQmlApplicationEngine engine;
    engine.loadFromModule("SightFlowVMS", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    auto* displayItem = engine.rootObjects().constFirst()->findChild<VideoDisplayItem*>(QStringLiteral("videoDisplay"));
    if (displayItem) {
        displayItem->setFrameQueue(&queue);
        displayItem->setCaptureState(&captureState);
    }

    return app.exec();
}
