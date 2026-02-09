#pragma once

#include "presets/PresetTypes.h"
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{
  /**
   * Serialization and storage for presets.
   */
  class PresetStorage
  {
  public:
    // JSON serialization
    [[nodiscard]] static std::string SerializeToJson(const Preset& preset);
    [[nodiscard]] static std::optional<Preset> DeserializeFromJson(const std::string& json);

    // File operations
    [[nodiscard]] static bool SaveToFile(const Preset& preset, const std::filesystem::path& path);
    [[nodiscard]] static std::optional<Preset> LoadFromFile(const std::filesystem::path& path);

    // Directory operations
    [[nodiscard]] static std::vector<Preset> LoadAllFromDirectory(const std::filesystem::path& directory);
    static void SaveAllToDirectory(const std::vector<Preset>& presets, const std::filesystem::path& directory);
  };

} // namespace guitarfx
