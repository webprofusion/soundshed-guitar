#pragma once

#include "presets/PresetTypesV2.h"
#include <optional>
#include <string>
#include <vector>

namespace namguitar
{
  /**
   * Serialization and storage for v2 presets.
   */
  class PresetStorageV2
  {
  public:
    // JSON serialization
    [[nodiscard]] static std::string SerializeToJson(const PresetV2& preset);
    [[nodiscard]] static std::optional<PresetV2> DeserializeFromJson(const std::string& json);

    // File operations
    [[nodiscard]] static bool SaveToFile(const PresetV2& preset, const std::filesystem::path& path);
    [[nodiscard]] static std::optional<PresetV2> LoadFromFile(const std::filesystem::path& path);

    // Directory operations
    [[nodiscard]] static std::vector<PresetV2> LoadAllFromDirectory(const std::filesystem::path& directory);
    static void SaveAllToDirectory(const std::vector<PresetV2>& presets, const std::filesystem::path& directory);
  };

} // namespace namguitar
