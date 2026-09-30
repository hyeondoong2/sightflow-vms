#include <iostream>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

namespace {

struct NetworkGuard {
    NetworkGuard() { avformat_network_init(); }
    ~NetworkGuard() { avformat_network_deinit(); }
};

struct AVFormatContextCloser {
    void operator()(AVFormatContext* ctx) const noexcept { avformat_close_input(&ctx); }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextCloser>;

struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const noexcept { av_packet_free(&pkt); }
};
using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const noexcept { avcodec_free_context(&ctx); }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

std::string avErrorToString(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

// Owns the AVCodecContext for one video stream. Single owner, RAII-closed.
class Decoder {
public:
    // Returns nullptr and fills errorOut on failure.
    static std::unique_ptr<Decoder> create(const AVCodecParameters* codecpar, std::string& errorOut)
    {
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            errorOut = "No decoder found for codec id " + std::to_string(codecpar->codec_id);
            return nullptr;
        }

        CodecContextPtr ctx(avcodec_alloc_context3(codec));
        if (!ctx) {
            errorOut = "Failed to allocate AVCodecContext";
            return nullptr;
        }

        int ret = avcodec_parameters_to_context(ctx.get(), codecpar);
        if (ret < 0) {
            errorOut = "Failed to copy codec parameters: " + avErrorToString(ret);
            return nullptr;
        }

        ret = avcodec_open2(ctx.get(), codec, nullptr);
        if (ret < 0) {
            errorOut = "Failed to open codec: " + avErrorToString(ret);
            return nullptr;
        }

        return std::unique_ptr<Decoder>(new Decoder(std::move(ctx)));
    }

    // Mirrors avcodec_send_packet's return value.
    int sendPacket(const AVPacket* pkt) { return avcodec_send_packet(ctx_.get(), pkt); }

    // Mirrors avcodec_receive_frame's return value. `frame` must be clean
    // (freshly allocated or just av_frame_unref'd) before this call.
    int receiveFrame(AVFrame* frame) { return avcodec_receive_frame(ctx_.get(), frame); }

private:
    explicit Decoder(CodecContextPtr ctx) : ctx_(std::move(ctx)) {}

    CodecContextPtr ctx_;
};

} // namespace

int main()
{
    const char* url = "rtsp://127.0.0.1:8554/test";

    // Owns network init/deinit for the whole run, regardless of return path.
    NetworkGuard network;

    AVFormatContext* rawFmtCtx = avformat_alloc_context();
    if (!rawFmtCtx) {
        std::cerr << "Failed to allocate AVFormatContext" << std::endl;
        return 1;
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0); // 5s connect/read timeout (microseconds)

    int ret = avformat_open_input(&rawFmtCtx, url, nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
        std::cerr << "Failed to open RTSP stream '" << url << "': " << avErrorToString(ret) << std::endl;
        // avformat_open_input frees/nulls rawFmtCtx on failure; this is then a safe no-op.
        avformat_close_input(&rawFmtCtx);
        return 1;
    }
    FormatContextPtr fmtCtx(rawFmtCtx); // owns the context from here on (RAII close)

    ret = avformat_find_stream_info(fmtCtx.get(), nullptr);
    if (ret < 0) {
        std::cerr << "Failed to read stream info: " << avErrorToString(ret) << std::endl;
        return 1;
    }

    const int videoStreamIndex = av_find_best_stream(fmtCtx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIndex < 0) {
        std::cerr << "No video stream found in '" << url << "'" << std::endl;
        return 1;
    }

    std::cout << "Connected to " << url << ", video stream index " << videoStreamIndex << std::endl;

    std::string decoderError;
    std::unique_ptr<Decoder> decoder =
        Decoder::create(fmtCtx->streams[videoStreamIndex]->codecpar, decoderError);
    if (!decoder) {
        std::cerr << "Failed to create decoder: " << decoderError << std::endl;
        return 1;
    }

    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        std::cerr << "Failed to allocate AVPacket" << std::endl;
        return 1;
    }

    FramePtr frame(av_frame_alloc());
    if (!frame) {
        std::cerr << "Failed to allocate AVFrame" << std::endl;
        return 1;
    }

    constexpr int targetFrames = 30;
    int decodedFrameCount = 0;

    while (decodedFrameCount < targetFrames) {
        ret = av_read_frame(fmtCtx.get(), packet.get());
        if (ret < 0) {
            std::cerr << "Stream ended or read error: " << avErrorToString(ret) << std::endl;
            break;
        }

        if (packet->stream_index != videoStreamIndex) {
            av_packet_unref(packet.get());
            continue;
        }

        ret = decoder->sendPacket(packet.get());
        av_packet_unref(packet.get()); // packet's data is copied into the decoder; released every code path

        if (ret < 0) {
            std::cerr << "avcodec_send_packet failed: " << avErrorToString(ret) << std::endl;
            break;
        }

        // Drain every frame this packet made available (0, 1, or more).
        while (decodedFrameCount < targetFrames) {
            ret = decoder->receiveFrame(frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break; // needs another packet, or stream ended
            }
            if (ret < 0) {
                std::cerr << "avcodec_receive_frame failed: " << avErrorToString(ret) << std::endl;
                break;
            }

            ++decodedFrameCount;
            std::cout << "frame " << decodedFrameCount << "/" << targetFrames
                      << " " << frame->width << "x" << frame->height
                      << " fmt=" << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format))
                      << std::endl;

            av_frame_unref(frame.get()); // must be clean before the next receiveFrame call
        }

        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            break; // a real receiveFrame error was reported above; stop
        }
    }

    std::cout << "Finished: decoded " << decodedFrameCount << " video frames." << std::endl;

    return (decodedFrameCount == targetFrames) ? 0 : 1;
}
