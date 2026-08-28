#pragma once

/**
 * PresetArchiveSupport.h — Reading .ssgpack preset archives.
 *
 * An archive is a zip of presets, blends and resources that must be merged
 * into the library without colliding with anything already there. Every id and
 * folder id it contributes is therefore scoped by a key derived from the
 * archive itself, and its presets' resource references are rewritten to match.
 * Getting that scoping wrong silently overwrites content the user already had,
 * so the rules live in one place.
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "presets/PresetTypes.h"

namespace guitarfx
{
class IPluginHost;
class FileSystem;
namespace storage { class JsonStore; }
}

namespace guitarfx::controller_detail
{

inline constexpr const char* kFactoryArchiveResourceProvider = "factory-archives";
inline constexpr int kFactoryArchiveStateSchemaVersion = 1;
inline constexpr const char* kFactoryArchiveLoadingEnabledSettingKey = "factoryPresets.archiveLoadingEnabled";
inline constexpr const char* kFactoryArchiveStateDocumentId = "factory-archive-state";
inline constexpr const char* kPresetArchiveSessionRootFolder = "sessions/preset-archive";
inline constexpr const char* kPresetArchiveSessionResourceProvider = "preset-archive-session";

struct FactoryArchiveResourceEntry
{
    std::string id;
    std::string name;
    std::string category;
    std::string type;
    std::string fileName;
    std::string hash;
    std::vector<std::uint8_t> bytes;
};

struct ParsedFactoryPresetArchive
{
    std::vector<Preset> presets;
    std::vector<nlohmann::json> blends;
    std::vector<FactoryArchiveResourceEntry> resources;
    nlohmann::json presetFolders = nlohmann::json::array();
    std::size_t tone3000ResourceCount = 0;
};

/// Stable key identifying one archive, derived from its filename. Everything
/// the archive contributes is namespaced by this.
[[nodiscard]] std::string BuildFactoryArchiveKey(const std::filesystem::path& archivePath);

[[nodiscard]] std::string BuildScopedFactoryArchiveId(const std::string& archiveKey, const std::string& rawId);
[[nodiscard]] std::string BuildScopedPresetArchiveSessionId(const std::string& archiveKey, const std::string& rawId);

[[nodiscard]] bool IsFactoryArchiveExtension(const std::filesystem::path& path);

/// Strips ordering prefixes and extensions from an archived preset title.
[[nodiscard]] std::string NormalizePresetTitle(std::string value);

// -- Preset folders contributed by an archive ------------------------

[[nodiscard]] nlohmann::json MakePresetFolderEntry(const std::string& id, const std::string& name);
[[nodiscard]] bool IsFactoryArchiveFolderId(const std::string& folderId);
[[nodiscard]] std::string BuildFactoryArchiveNestedFolderId(const std::string& archiveKey,
                                                            const std::string& folderPath);
[[nodiscard]] std::string BuildPresetArchiveSessionFolderId(const std::string& archiveKey,
                                                            const std::string& folderPath);

[[nodiscard]] nlohmann::json BuildFactoryArchiveFolders(
    const std::string& archiveKey,
    const nlohmann::json& archivePresetFolders,
    const std::unordered_map<std::string, std::string>& presetIdMapping);

[[nodiscard]] bool IsFactoryArchiveTopLevelFolder(const std::string& archiveKey, const nlohmann::json& folder);

/// Merges an archive's folders into the stored folder list, replacing any
/// previous contribution from the same archive.
void UpdateFactoryPresetFolders(storage::JsonStore& store,
                                const std::string& archiveKey,
                                const nlohmann::json& archivePresetFolders,
                                const std::unordered_map<std::string, std::string>& presetIdMapping,
                                const std::vector<std::string>& removedPresetIds);

[[nodiscard]] nlohmann::json BuildPresetArchiveSessionFolders(
    const std::string& archiveKey,
    const nlohmann::json& archivePresetFolders,
    const std::unordered_map<std::string, std::string>& presetIdMapping);

// -- Archive contents ------------------------------------------------

[[nodiscard]] std::optional<std::vector<std::uint8_t>> ExtractZipEntry(const std::vector<std::uint8_t>& zipBytes,
                                                                       const std::string& entryName);

/// Parses an archive's manifest and payload. On failure returns nullopt and
/// sets `error` to a message suitable for the UI.
[[nodiscard]] std::optional<ParsedFactoryPresetArchive> ParseFactoryPresetArchive(
    const std::filesystem::path& archivePath,
    const std::vector<std::uint8_t>& zipBytes,
    std::string& error);

/// Rewrites a graph's resource and blend references onto the archive-scoped ids.
void RemapPresetGraphResources(SignalGraph& graph,
                               const std::unordered_map<std::string, std::string>& resourceIdMap,
                               const std::unordered_map<std::string, std::string>& blendIdMap);

void RemapPresetArchiveReferences(Preset& preset,
                                  const std::unordered_map<std::string, std::string>& resourceIdMap,
                                  const std::unordered_map<std::string, std::string>& blendIdMap);

[[nodiscard]] std::filesystem::path ResolveFactoryPresetDirectory(const IPluginHost& host,
                                                                  const std::filesystem::path& legacyResourceRoot);

/// Ensures a save path carries the archive extension and a safe filename.
[[nodiscard]] std::filesystem::path NormalizePresetArchiveSavePath(const std::filesystem::path& path);

} // namespace guitarfx::controller_detail
