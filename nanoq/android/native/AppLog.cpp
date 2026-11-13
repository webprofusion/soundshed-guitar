#include "AppLog.h"

#include "Log.h"

#include <android/log.h>

#include <cstdarg>
#include <cstdio>

namespace nanoq::android
{
void AppLog(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    __android_log_write(ANDROID_LOG_INFO, "NanoQ", line);
    nanoq::Log(line);
}
} // namespace nanoq::android
