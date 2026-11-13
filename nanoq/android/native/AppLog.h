#pragma once

// The app's own lines: to logcat (tag NanoQ) and to nanoq.log in the profile's logs folder, as
// the iOS app's AppLog does.

namespace nanoq::android
{
void AppLog(const char* format, ...) __attribute__((format(printf, 1, 2)));
} // namespace nanoq::android
