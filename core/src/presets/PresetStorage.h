#pragma once

#include "presets/PresetTypes.h"
#include "storage/JsonStore.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace guitarfx
{
/// What a preset list shows of a preset.
struct PresetListEntry
{
    std::string id;
    std::string name;
    std::string category;
    std::string description;
    std::vector<std::string> tags;
};

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
    /// A preset's list fields, read as DeserializeFromJson reads them, without building its graph:
    /// nullopt where DeserializeFromJson would fail on them. The graph and scenes are not looked
    /// at, so a preset whose graph alone will not load is summarised all the same.
    [[nodiscard]] static std::optional<PresetListEntry> SummarizeJson(const std::string& json);
    static bool RemoveFromStore(storage::JsonStore& store, const std::string& id);
    [[nodiscard]] static bool ExistsInStore(const storage::JsonStore& store, const std::string& id);

    // Directory operations. Still used for reading factory presets shipped as
    // files and for import/export; user presets live in the store.
    [[nodiscard]] static std::vector<Preset> LoadAllFromDirectory(const std::filesystem::path& directory);
    static void SaveAllToDirectory(const std::vector<Preset>& presets, const std::filesystem::path& directory);
};

/**
 * Summaries of the store's presets, kept between calls: a call reads again only the presets
 * written since the last one, by this instance or any other, as told by each row's updated_at.
 * The preset list is sent on every editor open, delete and shared-sync catch-up, and building it
 * from LoadAllFromStore() parsed every preset in full each time.
 */
class PresetSummaryCache
{
  public:
    /// In the store's order (by id).
    [[nodiscard]] std::vector<PresetListEntry> Load(const storage::JsonStore& store);

  private:
    struct Entry
    {
        std::int64_t updatedAt = 0;
        std::optional<PresetListEntry> summary; // nullopt: the row does not read as a preset
    };

    std::mutex mMutex;
    std::unordered_map<std::string, Entry> mEntries; // by store id
};
} // namespace guitarfx
