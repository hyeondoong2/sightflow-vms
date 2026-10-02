#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "CaptureWorker.h"
#include "FrameQueue.h"
#include "VideoFrame.h"

int main()
{
    const std::string url = "rtsp://127.0.0.1:8554/test";

    FrameQueue queue;
    CaptureWorker worker(url, queue);
    worker.start();

    // --- TEMPORARY console verification only ---
    // This polling loop stands in for the eventual Qt UI's fixed-interval
    // QTimer polling FrameQueue::tryPopLatest() (docs/ARCHITECTURE.md §3/§5).
    // It is not part of the CaptureWorker/FrameQueue design itself -- it
    // exists only so this console harness can observe frames and then
    // exercise an explicit stop request. CaptureWorker's own read loop does
    // NOT stop after any fixed frame count; only stop()/the destructor ends it.
    constexpr int maxFramesToShow = 30;
    constexpr auto pollInterval = std::chrono::milliseconds(50);
    constexpr int maxPolls = 100; // ~5s safety bound if the stream never produces a frame

    int shownFrames = 0;
    for (int poll = 0; poll < maxPolls && shownFrames < maxFramesToShow; ++poll) {
        std::optional<VideoFrame> frame = queue.tryPopLatest();
        if (frame) {
            ++shownFrames;
            std::cout << "frame " << shownFrames << " " << frame->width() << "x" << frame->height()
                      << " fmt=BGRA32"
                      << " stride=" << frame->strideBytes() << "B"
                      << " bufferSize=" << frame->bufferSize() << "B"
                      << std::endl;
        }
        std::this_thread::sleep_for(pollInterval);
    }

    std::cout << "Requesting stop..." << std::endl;
    worker.stop();
    std::cout << "Worker stopped, exiting. Shown " << shownFrames << " frame(s)." << std::endl;

    return 0;
}
