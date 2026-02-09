#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace guitarfx
{
class FileSystem
{
public:
  [[nodiscard]] std::filesystem::path ResolvePresetDirectory() const;
  [[nodiscard]] std::filesystem::path ResolveCacheDirectory() const;
  [[nodiscard]] std::filesystem::path ResolveSettingsDirectory() const;
  [[nodiscard]] std::filesystem::path ResolveSettingsFile() const;
  [[nodiscard]] std::optional<std::filesystem::path> EnsureDirectory(const std::filesystem::path& dir) const;
};
} // namespace guitarfx
