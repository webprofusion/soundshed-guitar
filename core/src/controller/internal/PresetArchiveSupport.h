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
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "presets/PresetTypes.h"

namespace guitarfx
{
class IPluginHost;
class FileSystem;

namespace storage
{
class JsonStore;
}
} // namespace guitarfx

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
    std::vector<std::uint8_t> bytes; // empty when parsed without resource bytes
    std::uint64_t size = 0;          // the file's size, from the zip's directory
};

struct ParsedFactoryPresetArchive
{
    std::vector<Preset> presets;
    std::vector<nlohmann::json> blends;
    std::vector<FactoryArchiveResourceEntry> resources;
    nlohmann::json presetFolders = nlohmann::json::array();
    /// Effect presets, by effect type as the archive names it (EffectPresetArchiveSupport.h).
    /// Only factory archives offer them; installs and archive sessions leave them unread.
    nlohmann::json effectPresets = nlohmann::json::object();
    /// Models and IRs the archive names rather than carries, because their licence or provider
    /// does not allow passing the file on: {id, type, name, provider, providerKey, hash?,
    /// providerData?, ...}. Read from `remoteResources` and from the older `tone3000Resources`
    /// list, which comes back here as provider "tone3000" with its toneId and modelId in
    /// providerData. One entry per library id.
    nlohmann::json remoteResources = nlohmann::json::array();
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
    const std::string& archiveKey, const nlohmann::json& archivePresetFolders,
    const std::unordered_map<std::string, std::string>& presetIdMapping);

[[nodiscard]] bool IsFactoryArchiveTopLevelFolder(const std::string& archiveKey, const nlohmann::json& folder);

/// Merges an archive's folders into the stored folder list, replacing any
/// previous contribution from the same archive.
void UpdateFactoryPresetFolders(storage::JsonStore& store, const std::string& archiveKey,
                                const nlohmann::json& archivePresetFolders,
                                const std::unordered_map<std::string, std::string>& presetIdMapping,
                                const std::vector<std::string>& removedPresetIds);

[[nodiscard]] nlohmann::json BuildPresetArchiveSessionFolders(
    const std::string& archiveKey, const nlohmann::json& archivePresetFolders,
    const std::unordered_map<std::string, std::string>& presetIdMapping);

// -- Archive contents ------------------------------------------------

[[nodiscard]] std::optional<std::vector<std::uint8_t>> ExtractZipEntry(const std::vector<std::uint8_t>& zipBytes,
                                                                       const std::string& entryName);

struct ZipEntryContent
{
    std::string name;
    std::vector<std::uint8_t> bytes;
};

/// The first file, in the zip's own entry order, whose name ends with one of `extensions`
/// (given in lower case; names are compared without regard to case). Folders are skipped. Only
/// that first match is tried: nullopt when the bytes are not a zip, nothing matches, or the match
/// will not extract.
[[nodiscard]] std::optional<ZipEntryContent> ExtractFirstZipEntryWithExtension(
    const std::vector<std::uint8_t>& zipBytes, std::span<const std::string_view> extensions);

/// Parses an archive's manifest and payload. On failure returns nullopt and
/// sets `error` to a message suitable for the UI. Without `withResourceBytes`,
/// each resource's file is found and sized from the zip's directory but not
/// extracted, so its `bytes` stay empty: ExtractZipEntry reads one when needed.
[[nodiscard]] std::optional<ParsedFactoryPresetArchive> ParseFactoryPresetArchive(
    const std::filesystem::path& archivePath, const std::vector<std::uint8_t>& zipBytes, std::string& error,
    bool withResourceBytes = true);

/// Rewrites a graph's resource and blend references onto the archive-scoped ids.
void RemapPresetGraphResources(SignalGraph& graph, const std::unordered_map<std::string, std::string>& resourceIdMap,
                               const std::unordered_map<std::string, std::string>& blendIdMap);

void RemapPresetArchiveReferences(Preset& preset, const std::unordered_map<std::string, std::string>& resourceIdMap,
                                  const std::unordered_map<std::string, std::string>& blendIdMap);

/// True for a node id an archive may keep: 1 to 64 of A-Z, a-z, 0-9, `_` and `-`. The web UI
/// writes node ids into markup and selectors, so an id with anything else is replaced.
[[nodiscard]] bool IsSafeImportedNodeId(std::string_view id);

/// The resource types a preset archive, Tone Sharing item or remote import may install: NAM models
/// and impulse responses, which are data. A Custom Effect module ("wasm") is code from whoever
/// shared it, and any other type is unknown, so neither is taken from someone else's archive.
/// Sharing modules is planned: this is the one place to let "wasm" through once an install asks
/// for consent (see docs/wasm-effects-security-review-2026-10-19.md). "plugin" stays out.
[[nodiscard]] bool IsShareableResourceType(std::string_view resourceType);

/// Makes a preset read from an archive safe to store and show. A node id that fails
/// IsSafeImportedNodeId gets a fresh one — the same across the graph, every scene and the
/// global chain — and the edges follow; an edge naming no node is dropped. Resource file paths
/// are cleared: they name a file on the sharer's machine, or wherever the archive likes (a UNC
/// path would have the engine open a remote share). A Custom Effect node keeps its place in the
/// chain but loses its module and the descriptor cached from it, so it passes audio through until
/// the user picks a module of their own.
void SanitizeImportedPreset(Preset& preset);

[[nodiscard]] std::filesystem::path ResolveFactoryPresetDirectory(const IPluginHost& host,
                                                                  const std::filesystem::path& legacyResourceRoot);

/// Ensures a save path carries the archive extension and a safe filename.
[[nodiscard]] std::filesystem::path NormalizePresetArchiveSavePath(const std::filesystem::path& path);
} // namespace guitarfx::controller_detail
