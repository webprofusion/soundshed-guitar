#include "FileSystem.h"

#include <cstdlib>

namespace guitarfx
{
std::filesystem::path FileSystem::ResolvePlatformRootDirectory() const
{
#ifdef _WIN32

    if (const char* appData = std::getenv("APPDATA"); appData != nullptr && appData[0] != '\0')
    {
        return std::filesystem::path{appData} / "Soundshed Guitar";
    }

#elif defined(__APPLE__)

    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return std::filesystem::path{home} / "Library" / "Soundshed Guitar";
    }

#else

    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return std::filesystem::path{home} / ".config" / "Soundshed Guitar";
    }

#endif

    return std::filesystem::path{"settings"};
}

std::filesystem::path FileSystem::ResolveDataDirectory() const
{
    return ResolvePlatformRootDirectory() / "data";
}

std::filesystem::path FileSystem::ResolveDataV1Directory() const
{
    return ResolveDataDirectory() / "v1";
}

std::filesystem::path FileSystem::ResolvePresetDirectory() const
{
    return ResolveSettingsDirectory() / "presets";
}

std::filesystem::path FileSystem::ResolveCacheDirectory() const
{
    return ResolveSettingsDirectory() / "cache";
}

std::filesystem::path FileSystem::ResolveSettingsDirectory() const
{
    return ResolveDataV1Directory();
}

std::filesystem::path FileSystem::ResolveSettingsFile() const
{
    return ResolveSettingsDirectory() / "settings" / "app.json";
}

std::optional<std::filesystem::path> FileSystem::EnsureDirectory(const std::filesystem::path& dir) const
{
    std::error_code ec;

    if (std::filesystem::create_directories(dir, ec) || std::filesystem::exists(dir))
    {
        return dir;
    }

    return std::nullopt;
}
} // namespace guitarfx
