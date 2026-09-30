#include "RtspSource.h"

#include "AvError.h"

extern "C" {
#include <libavutil/dict.h>
}

std::unique_ptr<RtspSource> RtspSource::open(const std::string& url, std::string& errorOut)
{
    std::unique_ptr<RtspSource> source(new RtspSource());
    if (!source->openInternal(url, errorOut)) {
        return nullptr; // destroying `source` here releases whatever it already acquired
    }
    return source;
}

bool RtspSource::openInternal(const std::string& url, std::string& errorOut)
{
    AVFormatContext* rawFmtCtx = avformat_alloc_context();
    if (!rawFmtCtx) {
        errorOut = "Failed to allocate AVFormatContext";
        return false;
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0); // 5s connect/read timeout (microseconds)

    int ret = avformat_open_input(&rawFmtCtx, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
        errorOut = "Failed to open RTSP stream '" + url + "': " + avErrorToString(ret);
        // avformat_open_input frees/nulls rawFmtCtx on failure; this is then a safe no-op.
        avformat_close_input(&rawFmtCtx);
        return false;
    }
    fmtCtx_.reset(rawFmtCtx); // owned from here on; destructor closes it even if a later step fails

    ret = avformat_find_stream_info(fmtCtx_.get(), nullptr);
    if (ret < 0) {
        errorOut = "Failed to read stream info: " + avErrorToString(ret);
        return false;
    }

    videoStreamIndex_ = av_find_best_stream(fmtCtx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIndex_ < 0) {
        errorOut = "No video stream found in '" + url + "'";
        return false;
    }

    return true;
}

const AVCodecParameters* RtspSource::videoCodecParameters() const
{
    return fmtCtx_->streams[videoStreamIndex_]->codecpar;
}

int RtspSource::readPacket(AVPacket* packet)
{
    return av_read_frame(fmtCtx_.get(), packet);
}
