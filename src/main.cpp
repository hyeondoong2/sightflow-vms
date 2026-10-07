#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "CaptureState.h"
#include "CaptureWorker.h"
#include "FrameQueue.h"
#include "VideoDisplayItem.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    // This step fixes exactly two channels, "test" and "test2" -- no
    // dynamic channel list/manager (docs/DECISIONS.md D20). Each channel
    // gets its own independent FrameQueue/CaptureState/CaptureWorker
    // triplet, the exact same classes the single-channel step used,
    // instantiated twice; neither channel's objects are ever touched by the
    // other channel's worker thread.
    const std::string urlTest = "rtsp://127.0.0.1:8554/test";
    const std::string urlTest2 = "rtsp://127.0.0.1:8554/test2";

    // Declaration order matters: at scope exit below, C++ destroys these in
    // reverse order, which is exactly the shutdown sequence
    // docs/ARCHITECTURE.md §7 requires, per channel -- each channel's own
    // `queue`/`captureState` must be declared (and therefore destroyed
    // *after*, since destruction is reverse order) before that channel's
    // `worker`, so the worker's destructor (stop + join) can still safely
    // touch them while it runs. `engine` is declared last of all (so it is
    // destroyed *first*), tearing down both VideoDisplayItems and stopping
    // their poll timers before either channel's FrameQueue/CaptureState is
    // destroyed. The two channels' declarations may otherwise interleave in
    // any order -- they share no object, so the order between them doesn't
    // matter, only each channel's own internal order.
    FrameQueue queueTest;
    CaptureState captureStateTest;
    CaptureWorker workerTest(urlTest, queueTest, captureStateTest);
    workerTest.start();

    FrameQueue queueTest2;
    CaptureState captureStateTest2;
    CaptureWorker workerTest2(urlTest2, queueTest2, captureStateTest2);
    workerTest2.start();

    QQmlApplicationEngine engine;
    engine.loadFromModule("SightFlowVMS", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    QObject* root = engine.rootObjects().constFirst();

    auto* displayTest = root->findChild<VideoDisplayItem*>(QStringLiteral("videoDisplayTest"));
    if (displayTest) {
        displayTest->setFrameQueue(&queueTest);
        displayTest->setCaptureState(&captureStateTest);
    }

    auto* displayTest2 = root->findChild<VideoDisplayItem*>(QStringLiteral("videoDisplayTest2"));
    if (displayTest2) {
        displayTest2->setFrameQueue(&queueTest2);
        displayTest2->setCaptureState(&captureStateTest2);
    }

    return app.exec();
}
