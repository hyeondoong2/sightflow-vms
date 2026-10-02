#include <QGuiApplication>
#include <QQmlApplicationEngine>

// Phase 1 Qt/QML milestone: show the empty "SightFlow VMS" window
// (src/qml/Main.qml) only. No RTSP connection, FrameQueue polling, or
// VideoFrame display is wired up yet -- CaptureWorker and friends are
// unchanged and still built, just not started from here yet.
int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    engine.loadFromModule("SightFlowVMS", "Main");

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
