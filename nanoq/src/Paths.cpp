#include "Paths.h"

#include "util/FileSystem.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
    #include <shlobj.h>
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <dlfcn.h>
    #if defined(__APPLE__)
        #include <TargetConditionals.h>
        #include <mach-o/dyld.h>
    #endif
#endif

namespace nanoq
{
std::filesystem::path ProfileFolder()
{
    // The engine decides where its data lives (core/src/util/FileSystem.cpp); this asks it, so the
    // two cannot disagree. The same folder Soundshed Guitar and Nano use.
    return guitarfx::FileSystem().ResolvePlatformRootDirectory();
}

void ApplyProfileOverride()
{
    // For tests and development: a data root of its own, so a run never touches the real profile.
    // Must run before the engine is built.
    if (const auto root = guitarfx::FileSystem::ReadEnvironmentPath("SOUNDSHED_NANOQ_PROFILE"); !root.empty())
    {
        guitarfx::FileSystem::SetPlatformRootOverride(root);
    }
}

std::filesystem::path ModuleDirectory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module))
    {
        return {};
    }

    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));

    if (n == 0 || n >= std::size(buffer))
    {
        return {};
    }

    return std::filesystem::path(buffer).parent_path();
#else
    Dl_info info{};

    if (dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) == 0 || info.dli_fname == nullptr)
    {
        return {};
    }

    return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

bool RunningAsStandalone()
{
#if defined(__ANDROID__)
    // The Android app hosts the plugin linked into its own library (nanoq/android), which the
    // system loads into its zygote-forked process: never a plugin in someone else's host.
    return true;
#elif defined(_WIN32)
    HMODULE self = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&RunningAsStandalone), &self))
    {
        return false;
    }

    return self == GetModuleHandleW(nullptr);
#else
    // This code is the program when the object that holds it is the executable.
    Dl_info info{};

    if (dladdr(reinterpret_cast<void*>(&RunningAsStandalone), &info) == 0 || info.dli_fname == nullptr)
    {
        return false;
    }

    std::error_code ec;
    const auto module = std::filesystem::weakly_canonical(info.dli_fname, ec);
    #if defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);

    if (_NSGetExecutablePath(buffer, &size) != 0)
    {
        return false;
    }

    const auto exe = std::filesystem::weakly_canonical(buffer, ec);
    #else
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    #endif
    return !ec && module == exe;
#endif
}

std::string StandaloneCommandLine()
{
    if (!RunningAsStandalone())
    {
        return {};
    }

#if defined(_WIN32)
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);

    if (args == nullptr)
    {
        return {};
    }

    std::wstring joined;

    for (int i = 1; i < count; ++i)
    {
        joined += (joined.empty() ? L"" : L" ") + std::wstring(args[i]);
    }

    LocalFree(args);

    if (joined.empty())
    {
        return {};
    }

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, joined.c_str(), static_cast<int>(joined.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(std::max(0, size)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, joined.c_str(), static_cast<int>(joined.size()), utf8.data(), size, nullptr,
                        nullptr);
    return utf8;
#else
    return {};
#endif
}

std::vector<std::filesystem::path> ResourceCandidates()
{
    std::vector<std::filesystem::path> out;

#if defined(__ANDROID__)
    // An APK's assets are not files: the Android app unpacks them into its data folder and names
    // it here before the plugin loads (nanoq/android, NanoQActivity).
    if (const auto root = guitarfx::FileSystem::ReadEnvironmentPath("SOUNDSHED_NANOQ_RESOURCES"); !root.empty())
    {
        out.push_back(root);
    }
#endif

    const auto dir = ModuleDirectory();

    if (dir.empty())
    {
        return out;
    }

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    // An iOS app bundle is flat: ui/ sits beside the executable, at the bundle's root. Not in a
    // folder called resources there, which makes codesign take the bundle for a macOS one.
    out.push_back(dir);
#endif
    // Next to the binary (standalone, CLAP, a Windows VST3 folder's x86_64-win), then up through
    // a macOS bundle's Contents/MacOS -> Contents/Resources and a VST3 folder's Contents.
    out.push_back(dir / (std::string(NANOQ_NAME) + " Resources")); // CLAP and the standalone app
    out.push_back(dir / "resources");
    out.push_back(dir / "Resources");
    out.push_back(dir.parent_path() / "Resources");
    out.push_back(dir.parent_path().parent_path() / "resources");
    out.push_back(dir.parent_path().parent_path() / "Resources");
    return out;
}
} // namespace nanoq
