#pragma once

#include <string>

extern "C" {
#include <libavutil/error.h>
}

inline std::string avErrorToString(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}
