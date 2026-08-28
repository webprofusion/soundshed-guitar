#pragma once

#include "presets/CompositePresetTypes.h"
#include "storage/JsonStore.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{
/**
 * File I/O for CompositePreset (Multi-Rig) configurations.
 *
 * Files are stored as <id>.composite.json inside the composite-presets/
 * subdirectory of the user data directory.
 */
class CompositePresetStorage
{
  public:
    static constexpr const char* kSubdir = "composite-presets";
    static constexpr const char* kExtension = ".composite.json";

    [[nodiscard]] static std::string SerializeToJson(const CompositePreset& cp);
    [[nodiscard]] static std::optional<CompositePreset> DeserializeFromJson(const std::string& json);

    [[nodiscard]] static bool SaveToFile(const CompositePreset& cp, const std::filesystem::path& dir);

    [[nodiscard]] static std::optional<CompositePreset> LoadById(const std::string& id,
                                                                 const std::filesystem::path& dir);

    [[nodiscard]] static std::vector<CompositePreset> ListAll(const std::filesystem::path& dir);

    static bool DeleteById(const std::string& id, const std::filesystem::path& dir);

    // Store-backed persistence: one row per composite, keyed by composite id.
    [[nodiscard]] static bool SaveToStore(storage::JsonStore& store, const CompositePreset& cp);
    [[nodiscard]] static std::optional<CompositePreset> LoadFromStore(const storage::JsonStore& store,
                                                                      const std::string& id);
    [[nodiscard]] static std::vector<CompositePreset> ListAllFromStore(const storage::JsonStore& store);
    static bool DeleteFromStore(storage::JsonStore& store, const std::string& id);
};
} // namespace guitarfx
