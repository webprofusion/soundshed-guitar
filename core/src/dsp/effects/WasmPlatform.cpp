// WasmPlatform.cpp - See WasmPlatform.h.

#include "dsp/effects/WasmPlatform.h"

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <Security/Security.h>
    #include <sys/types.h>
    #include <unistd.h>

    #include <cstdint>

// The code-signing status call (sys/codesign.h, which the SDK does not ship). Exported by
// libSystem on every macOS this builds for.
extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);
#endif

namespace guitarfx
{
#if defined(__APPLE__)
namespace
{
constexpr unsigned int kCsOpsStatus = 0;      // CS_OPS_STATUS
constexpr std::uint32_t kCsRuntime = 0x10000; // CS_RUNTIME: the Hardened Runtime is on

bool HasEntitlement(SecTaskRef task, CFStringRef name)
{
    CFTypeRef value = SecTaskCopyValueForEntitlement(task, name, nullptr);

    if (value == nullptr)
    {
        return false;
    }

    const bool on = CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    CFRelease(value);
    return on;
}

bool Decide()
{
    std::uint32_t flags = 0;

    // Unreadable: nothing says the runtime is hardened, so nothing says the code would be refused.
    if (csops(getpid(), kCsOpsStatus, &flags, sizeof(flags)) != 0 || (flags & kCsRuntime) == 0)
    {
        return true;
    }

    SecTaskRef task = SecTaskCreateFromSelf(nullptr);

    if (task == nullptr)
    {
        return false;
    }

    const bool allowed = HasEntitlement(task, CFSTR("com.apple.security.cs.allow-unsigned-executable-memory")) ||
                         HasEntitlement(task, CFSTR("com.apple.security.cs.disable-executable-page-protection"));
    CFRelease(task);
    return allowed;
}
} // namespace
#endif

bool WasmGeneratedCodeAllowed(std::string* why)
{
#if defined(__APPLE__)
    // Fixed for the life of the process: its signature and entitlements cannot change.
    static const bool allowed = Decide();

    if (!allowed && why != nullptr)
    {
        *why = "Custom Effects cannot run in this host: macOS only lets them run where the app allows "
               "generated code (com.apple.security.cs.allow-unsigned-executable-memory). They run in the "
               "Soundshed Guitar app.";
    }

    return allowed;
#else
    (void)why;
    return true;
#endif
}
} // namespace guitarfx
