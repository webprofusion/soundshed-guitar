#pragma once

#include "presets/PresetTypes.h"
#include "storage/JsonStore.h"
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

    // Store operations — one row per preset, keyed by preset id.
    [[nodiscard]] static bool SaveToStore(storage::JsonStore& store, const Preset& preset);
    [[nodiscard]] static std::optional<Preset> LoadFromStore(const storage::JsonStore& store, const std::string& id);
    [[nodiscard]] static std::vector<Preset> LoadAllFromStore(const storage::JsonStore& store);
    static bool RemoveFromStore(storage::JsonStore& store, const std::string& id);
    [[nodiscard]] static bool ExistsInStore(const storage::JsonStore& store, const std::string& id);

    // Directory operations. Still used for reading factory presets shipped as
    // files and for import/export; user presets live in the store.
    [[nodiscard]] static std::vector<Preset> LoadAllFromDirectory(const std::filesystem::path& directory);
    static void SaveAllToDirectory(const std::vector<Preset>& presets, const std::filesystem::path& directory);
};

} // namespace guitarfx
