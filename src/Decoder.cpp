#include "Decoder.h"

#include "AvError.h"

std::unique_ptr<Decoder> Decoder::create(const AVCodecParameters* codecpar, std::string& errorOut)
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

int Decoder::sendPacket(const AVPacket* pkt)
{
    return avcodec_send_packet(ctx_.get(), pkt);
}

int Decoder::receiveFrame(AVFrame* frame)
{
    return avcodec_receive_frame(ctx_.get(), frame);
}
