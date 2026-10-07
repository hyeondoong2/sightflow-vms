#pragma once

#include <memory>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
}

// Shared RAII wrappers for FFmpeg's AVPacket/AVFrame, used by every worker
// that owns a demux/decode loop (CaptureWorker, DecodeWorker) so the
// allocate/free pairing lives in exactly one place (D4: RAII for every
// FFmpeg resource).
struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const noexcept { av_packet_free(&pkt); }
};
using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
