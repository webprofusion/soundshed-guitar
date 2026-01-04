#include "FileSystem.h"

#include <cstdlib>

namespace namguitar
{

std::filesystem::path FileSystem::ResolvePresetDirectory() const
{
  return ResolveSettingsDirectory() / "presets";
}

std::filesystem::path FileSystem::ResolveCacheDirectory() const
{
  return std::filesystem::path{"cache"};
}

std::filesystem::path FileSystem::ResolveSettingsDirectory() const
{
  // Use AppData on Windows, ~/.config on Linux/Mac
#ifdef _WIN32
  const char* appData = std::getenv("APPDATA");
  if (appData)
  {
    return std::filesystem::path{appData} / "NAMGuitarFX";
  }
#else
  const char* home = std::getenv("HOME");
  if (home)
  {
    return std::filesystem::path{home} / ".config" / "NAMGuitarFX";
  }
#endif
  return std::filesystem::path{"settings"};
}

std::filesystem::path FileSystem::ResolveSettingsFile() const
{
  return ResolveSettingsDirectory() / "settings.json";
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

} // namespace namguitar
