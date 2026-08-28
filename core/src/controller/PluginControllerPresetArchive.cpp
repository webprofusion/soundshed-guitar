/**
 * PluginControllerPresetArchive.cpp - Factory archives and preset-archive
 * sessions.
 *
 * A session mounts an archive as a temporary library: its presets and
 * resources become visible without being written into the user's own library,
 * and ending the session takes them away again. Everything an archive
 * contributes is namespaced by an archive key so it can be withdrawn cleanly.
 */

#include "PluginController.h"

#include "util/Base64.h"

#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/FileIO.h"
#include "util/PathSanitizer.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

bool PluginController::IsFactoryPresetArchiveLoadingEnabled() const
{
    const auto it = mAppSettings.find(kFactoryArchiveLoadingEnabledSettingKey);
    if (it == mAppSettings.end() || !it->is_boolean())
        return true;
    return it->get<bool>();
}

bool PluginController::IsPresetArchiveSessionActive() const
{
    return mPresetArchiveSession.has_value();
}

std::filesystem::path PluginController::GetEffectiveUserPresetDirectory() const
{
    if (mPresetArchiveSession)
        return mPresetArchiveSession->presetDir;
    return mUserPresetsPath;
}

std::filesystem::path PluginController::GetEffectiveSettingsDirectory() const
{
    if (mPresetArchiveSession)
        return mPresetArchiveSession->rootPath;
    return mFileSystem.ResolveSettingsDirectory();
}

void PluginController::RefreshPresetLibraryViews()
{
    HandleGetPresetListRequest();
    HandleGetPresetFoldersRequest();
    HandleGetPresetFavoritesRequest();
    HandleGetPresetRatingsRequest();
    HandleGetSetlistsRequest();
}

void PluginController::SendPresetArchiveSessionStateToUI(const char* messageType,
                                                         const std::string& detail)
{
    nlohmann::json message;
    message["type"] = messageType == nullptr ? "presetArchiveSessionState" : messageType;
    message["active"] = IsPresetArchiveSessionActive();
    if (mPresetArchiveSession)
    {
        message["archiveName"] = mPresetArchiveSession->archiveName;
        message["archiveKey"] = mPresetArchiveSession->archiveKey;
        message["presetCount"] = mPresetArchiveSession->presetCount;
    }
    if (!detail.empty())
        message["detail"] = detail;
    SendMessageToUI(message.dump());
}

void PluginController::StartPresetArchiveSession(const std::string& archiveFileName,
                                                 const std::vector<std::uint8_t>& archiveBytes)
{
    std::string parseError;
    auto parsedOpt = ParseFactoryPresetArchive(std::filesystem::path(archiveFileName), archiveBytes, parseError);
    if (!parsedOpt)
        throw std::runtime_error(parseError.empty() ? "Failed to parse preset archive" : parseError);

    auto parsed = std::move(*parsedOpt);
    if (parsed.presets.empty())
        throw std::runtime_error("Archive contains no presets");
    if (parsed.tone3000ResourceCount > 0)
        throw std::runtime_error("Archive session mode does not support Tone3000-linked resources yet");

    if (IsPresetArchiveSessionActive())
        EndPresetArchiveSession(false);

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const std::string archiveKeyBase = BuildFactoryArchiveKey(std::filesystem::path(archiveFileName));
    const std::string sessionStamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const std::string archiveKey = archiveKeyBase + "-" + sessionStamp;
    const auto sessionRoot = settingsDir / kPresetArchiveSessionRootFolder / archiveKey;
    const auto presetDir = sessionRoot / "presets" / "user";
    const auto resourceContentDir = sessionRoot / "resources" / "content" / kPresetArchiveSessionResourceProvider / archiveKey;

    std::error_code ec;
    std::filesystem::remove_all(sessionRoot, ec);
    [[maybe_unused]] const auto ensuredPresetDir = mFileSystem.EnsureDirectory(presetDir);
    [[maybe_unused]] const auto ensuredResourceDir = mFileSystem.EnsureDirectory(resourceContentDir);

    std::unordered_map<std::string, std::string> resourceIdMap;
    for (const auto& resource : parsed.resources)
    {
        const std::string scopedResourceId = BuildScopedPresetArchiveSessionId(archiveKey, resource.id);
        std::string resolvedName = resource.fileName.empty() ? resource.id : resource.fileName;
        resolvedName = util::SanitizeFilename(resolvedName);
        if (resolvedName.empty())
            resolvedName = scopedResourceId + (resource.type == "ir" ? ".wav" : ".nam");

        const auto targetPath = resourceContentDir / resolvedName;
        if (!WriteFile(targetPath, resource.bytes))
            throw std::runtime_error("Failed to extract archive resource: " + resolvedName);

        resourceIdMap[resource.id] = scopedResourceId;

        LibraryResource libraryResource;
        libraryResource.type = resource.type;
        libraryResource.id = scopedResourceId;
        libraryResource.name = resource.name.empty() ? resource.id : resource.name;
        libraryResource.category = resource.category;
        libraryResource.description = "Session-only preset archive resource";
        libraryResource.filePath = targetPath;
        libraryResource.hash = resource.hash;
        libraryResource.metadata["provider"] = kPresetArchiveSessionResourceProvider;
        libraryResource.metadata["archiveName"] = archiveFileName;
        libraryResource.metadata["archiveKey"] = archiveKey;
        libraryResource.metadata["originalId"] = resource.id;
        if (resource.type == "nam")
            EnrichNamResourceMetadata(libraryResource, targetPath);
        libraryResource.category = ResolveResourceLibraryCategory(libraryResource, libraryResource.category);
        mResourceLibrary.AddResource(libraryResource);
    }

    std::unordered_map<std::string, std::string> blendIdMap;
    if (!mBlendLibrary.is_array())
        mBlendLibrary = nlohmann::json::array();
    for (auto blend : parsed.blends)
    {
        const std::string originalBlendId = blend.value("id", "");
        if (originalBlendId.empty())
            continue;

        const std::string scopedBlendId = BuildScopedPresetArchiveSessionId(archiveKey, originalBlendId);
        blendIdMap[originalBlendId] = scopedBlendId;
        blend["id"] = scopedBlendId;

        if (blend.contains("models") && blend["models"].is_array())
        {
            for (auto& modelId : blend["models"])
            {
                if (!modelId.is_string())
                    continue;
                const auto mapped = resourceIdMap.find(modelId.get<std::string>());
                if (mapped != resourceIdMap.end())
                    modelId = mapped->second;
            }
        }

        if (blend.contains("modelMappings") && blend["modelMappings"].is_array())
        {
            for (auto& mapping : blend["modelMappings"])
            {
                if (!mapping.is_object())
                    continue;
                const auto mapped = resourceIdMap.find(mapping.value("id", ""));
                if (mapped != resourceIdMap.end())
                    mapping["id"] = mapped->second;
            }
        }

        bool replaced = false;
        for (auto& existing : mBlendLibrary)
        {
            if (existing.is_object() && existing.value("id", "") == scopedBlendId)
            {
                existing = blend;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            mBlendLibrary.push_back(blend);
    }

    std::unordered_map<std::string, std::string> presetIdMap;
    std::optional<Preset> firstPreset;
    for (auto preset : parsed.presets)
    {
        RemapPresetArchiveReferences(preset, resourceIdMap, blendIdMap);
        NormalizePresetScenes(preset);

        const std::string sourcePresetId = preset.id.empty()
            ? (preset.name.empty() ? "preset" : preset.name)
            : preset.id;
        const std::string scopedPresetId = BuildScopedPresetArchiveSessionId(archiveKey, sourcePresetId);
        presetIdMap[sourcePresetId] = scopedPresetId;

        preset.id = scopedPresetId;
        if (preset.category.empty())
            preset.category = "Imported";

        const auto presetPath = presetDir / (preset.id + ".json");
        if (!PresetStorage::SaveToFile(preset, presetPath))
            throw std::runtime_error("Failed to write session preset: " + preset.name);

        if (!firstPreset.has_value())
            firstPreset = preset;
    }

    nlohmann::json presetFoldersPayload = nlohmann::json::object();
    presetFoldersPayload["folders"] = BuildPresetArchiveSessionFolders(archiveKey, parsed.presetFolders, presetIdMap);
    presetFoldersPayload["activeFolderId"] = "__all__";

    mPresetArchiveSession = PresetArchiveSessionState{
        archiveKey,
        std::filesystem::path(archiveFileName).filename().string(),
        sessionRoot,
        presetDir,
        presetIdMap.size(),
    };

    SaveUiStorageJson("preset-folders.json", presetFoldersPayload);

    ClearActivePresetMixerState();
    mActivePreset.reset();
    mActivePresetId.clear();
    mActivePresetJson.clear();
    mActiveSceneId.clear();

    if (firstPreset)
    {
        ApplyBlendDefinitions(*firstPreset);
        if (!SetPresetActiveScene(*firstPreset, "", &mActiveSceneId))
            mActiveSceneId = GetDefaultPresetSceneId(*firstPreset);
        mActivePresetId = firstPreset->id;
        ApplyPreset(*firstPreset);
    }

    InvalidateResourceUsageIndex();
    mPendingStateBroadcast = true;
    BroadcastState();
    RefreshPresetLibraryViews();
    SendPresetArchiveSessionStateToUI("presetArchiveSessionStarted");
}

void PluginController::EndPresetArchiveSession(bool notifyUi)
{
    if (!mPresetArchiveSession)
        return;

    const auto sessionRoot = mPresetArchiveSession->rootPath;
    mPresetArchiveSession.reset();

    std::error_code ec;
    std::filesystem::remove_all(sessionRoot, ec);

    LoadResourceLibraries();
    LoadBlendLibrary();
    LoadFactoryPresetArchives();
    InvalidateResourceUsageIndex();

    ClearActivePresetMixerState();
    mActivePreset.reset();
    mActivePresetId.clear();
    mActivePresetJson.clear();
    mActiveSceneId.clear();
    LoadLastSessionState();
    mPendingStateBroadcast = true;
    BroadcastState();
    RefreshPresetLibraryViews();
    if (notifyUi)
        SendPresetArchiveSessionStateToUI("presetArchiveSessionEnded");
}


void PluginController::HandleStartPresetArchiveSessionRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string archiveFileName = payload.value("fileName", "preset-archive.soundshed.presets");
    if (dataEncoded.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "presetArchiveSessionFailed"},
            {"message", "Missing archive data"}
        }.dump());
        return;
    }

    const auto archiveBytes = util::DecodeBase64(dataEncoded);
    if (archiveBytes.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "presetArchiveSessionFailed"},
            {"message", "Invalid archive payload"}
        }.dump());
        return;
    }

    try
    {
        StartPresetArchiveSession(archiveFileName, archiveBytes);
    }
    catch (const std::exception& e)
    {
        ReportErrorToUI("Preset archive session failed", e.what());
        SendMessageToUI(nlohmann::json{
            {"type", "presetArchiveSessionFailed"},
            {"message", e.what()}
        }.dump());
    }
}

void PluginController::HandleEndPresetArchiveSessionRequest()
{
    EndPresetArchiveSession(true);
}

void PluginController::HandleSavePresetArchiveRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    std::string suggestedName = util::SanitizeFilename(payload.value("fileName", "preset.soundshed.preset"));
    std::string lowerSuggested = suggestedName;
    std::transform(lowerSuggested.begin(), lowerSuggested.end(), lowerSuggested.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const std::string presetSuffix = ".soundshed.preset";
    const std::string presetsSuffix = ".soundshed.presets";

    const auto hasSuffix = [&lowerSuggested](const std::string& suffix) -> bool
    {
        return lowerSuggested.size() >= suffix.size() &&
               lowerSuggested.compare(lowerSuggested.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    const std::string selectedSuffix = hasSuffix(presetsSuffix) ? presetsSuffix : presetSuffix;

    while (hasSuffix(presetSuffix) || hasSuffix(presetsSuffix))
    {
        const std::string& suffixToTrim = hasSuffix(presetsSuffix) ? presetsSuffix : presetSuffix;
        suggestedName.erase(suggestedName.size() - suffixToTrim.size());
        lowerSuggested.erase(lowerSuggested.size() - suffixToTrim.size());
    }

    if (suggestedName.empty())
    {
        suggestedName = "preset";
    }
    suggestedName += selectedSuffix;

    if (dataEncoded.empty())
    { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Missing export data"}}.dump()); return; }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Save Preset Archive", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result)
        {
            if (!result.success)
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Save cancelled"}}.dump()); return; }

            const auto normalizedPath = NormalizePresetArchiveSavePath(result.path);

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Invalid export data"}}.dump()); return; }

            if (!WriteFile(normalizedPath, decodedBytes))
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Failed to save file"}}.dump()); return; }

            SendMessageToUI(nlohmann::json{{"type", "presetExportSaved"}, {"path", normalizedPath.generic_string()}}.dump());
            AppendSessionLog("Preset export saved: " + normalizedPath.generic_string());
        });
}

void PluginController::LoadFactoryPresetArchives()
{
    mFactoryArchivePresets.clear();
    mFactoryArchiveBlendIds.clear();
    mFactoryArchivePresetIds.clear();
    mTrackedFactoryArchivePresetIds.clear();
    mFactoryArchivePresetAliases.clear();

    auto factoryArchiveState = Store().Get(storage::ItemType::kDocument, kFactoryArchiveStateDocumentId)
                                   .value_or(nlohmann::json::object());
    if (!factoryArchiveState.is_object())
        factoryArchiveState = nlohmann::json::object();
    factoryArchiveState["schemaVersion"] = kFactoryArchiveStateSchemaVersion;
    if (!factoryArchiveState.contains("archives") || !factoryArchiveState["archives"].is_object())
        factoryArchiveState["archives"] = nlohmann::json::object();

    for (const auto& archiveEntry : factoryArchiveState["archives"].items())
    {
        const auto& mappings = archiveEntry.value().value("presetMappings", nlohmann::json::object());
        if (!mappings.is_object())
            continue;
        for (const auto& mapping : mappings.items())
        {
            if (!mapping.value().is_string())
                continue;
            const std::string importedId = mapping.value().get<std::string>();
            if (importedId.empty())
                continue;
            mTrackedFactoryArchivePresetIds.insert(importedId);
        }
    }

    if (!IsFactoryPresetArchiveLoadingEnabled())
    {
        AppendSessionLog("Factory preset archive loading disabled by app setting");
        return;
    }

    for (const auto& archiveEntry : factoryArchiveState["archives"].items())
    {
        const auto& mappings = archiveEntry.value().value("presetMappings", nlohmann::json::object());
        if (!mappings.is_object())
            continue;
        for (const auto& mapping : mappings.items())
        {
            if (!mapping.value().is_string())
                continue;
            const std::string importedId = mapping.value().get<std::string>();
            if (importedId.empty())
                continue;
            mFactoryArchivePresetAliases[mapping.key()] = importedId;
            mFactoryArchivePresetIds.insert(importedId);
            mTrackedFactoryArchivePresetIds.insert(importedId);
        }
    }

    const auto factoryDir = ResolveFactoryPresetDirectory(mHost, mResourceRoot);
    if (!std::filesystem::exists(factoryDir))
        return;

    const auto extractedRoot = mFileSystem.ResolveSettingsDirectory() / "resources" / "content" / kFactoryArchiveResourceProvider;
    [[maybe_unused]] const auto ensuredExtractedRoot = mFileSystem.EnsureDirectory(extractedRoot);

    std::unordered_set<std::string> occupiedPresetIds;
    for (const auto& presetId : Store().ListIds(storage::ItemType::kPreset))
        occupiedPresetIds.insert(presetId);
    for (const auto& entry : std::filesystem::directory_iterator(factoryDir))
    {
        if (entry.path().extension() == ".json")
            occupiedPresetIds.insert(entry.path().stem().string());
    }

    if (!mBlendLibrary.is_array())
        mBlendLibrary = nlohmann::json::array();

    for (const auto& entry : std::filesystem::directory_iterator(factoryDir))
    {
        if (!entry.is_regular_file() || !IsFactoryArchiveExtension(entry.path()))
            continue;

        const auto zipBytes = util::ReadFileBytes(entry.path());
        if (zipBytes.empty())
        {
            AppendSessionLog("Factory preset archive skipped (empty or unreadable): " + entry.path().string());
            continue;
        }

        std::string parseError;
        auto parsedOpt = ParseFactoryPresetArchive(entry.path(), zipBytes, parseError);
        if (!parsedOpt)
        {
            AppendSessionLog("Factory preset archive skipped (" + entry.path().filename().string() + "): " + parseError);
            continue;
        }

        auto parsed = std::move(*parsedOpt);
        const std::string archiveKey = BuildFactoryArchiveKey(entry.path());
        const std::string archiveHash = mHasher.HashFile(entry.path());
        auto archiveState = factoryArchiveState["archives"].contains(archiveKey)
            && factoryArchiveState["archives"][archiveKey].is_object()
            ? factoryArchiveState["archives"][archiveKey]
            : nlohmann::json::object();
        if (!archiveState.contains("presetMappings") || !archiveState["presetMappings"].is_object())
            archiveState["presetMappings"] = nlohmann::json::object();

        std::unordered_set<std::string> trackedPresetIds;
        for (const auto& mapping : archiveState["presetMappings"].items())
        {
            if (!mapping.value().is_string())
                continue;
            const std::string importedId = mapping.value().get<std::string>();
            if (importedId.empty())
                continue;
            trackedPresetIds.insert(importedId);
            mFactoryArchivePresetAliases[mapping.key()] = importedId;
            mFactoryArchivePresetIds.insert(importedId);
        }

        const bool archiveChanged = archiveHash.empty() || archiveState.value("hash", "") != archiveHash;
        std::unordered_map<std::string, std::string> resourceIdMap;
        std::unordered_map<std::string, std::string> blendIdMap;

        for (const auto& resource : parsed.resources)
        {
            const std::string scopedResourceId = BuildScopedFactoryArchiveId(archiveKey, resource.id);
            std::string resolvedName = resource.fileName.empty() ? resource.id : resource.fileName;
            resolvedName = util::SanitizeFilename(resolvedName);
            if (resolvedName.empty())
                resolvedName = scopedResourceId + (resource.type == "ir" ? ".wav" : ".nam");

            const auto archiveExtractDir = extractedRoot / archiveKey;
            [[maybe_unused]] const auto ensuredArchiveDir = mFileSystem.EnsureDirectory(archiveExtractDir);
            const auto targetPath = archiveExtractDir / resolvedName;
            const bool needsWrite = archiveChanged || !std::filesystem::exists(targetPath);
            if (needsWrite && !WriteFile(targetPath, resource.bytes))
            {
                AppendSessionLog("Factory preset archive resource write failed: " + targetPath.string());
                continue;
            }
            if (!std::filesystem::exists(targetPath))
            {
                AppendSessionLog("Factory preset archive resource missing after import: " + targetPath.string());
                continue;
            }

            resourceIdMap[resource.id] = scopedResourceId;

            LibraryResource libraryResource;
            libraryResource.type = resource.type;
            libraryResource.id = scopedResourceId;
            libraryResource.name = resource.name.empty() ? resource.id : resource.name;
            libraryResource.category = resource.category;
            libraryResource.description = "Bundled factory archive resource";
            libraryResource.filePath = targetPath;
            libraryResource.hash = resource.hash;
            libraryResource.metadata["provider"] = kFactoryArchiveResourceProvider;
            libraryResource.metadata["archive"] = entry.path().filename().string();
            libraryResource.metadata["factoryArchiveKey"] = archiveKey;
            libraryResource.metadata["factoryArchiveHash"] = archiveHash;
            libraryResource.metadata["originalId"] = resource.id;
            if (needsWrite || !mResourceLibrary.HasResource(libraryResource.type, libraryResource.id))
                AppendUserLibraryResource(libraryResource);
            else
                mResourceLibrary.AddResource(libraryResource);
        }

        for (auto blend : parsed.blends)
        {
            const std::string originalBlendId = blend.value("id", "");
            if (originalBlendId.empty())
                continue;

            const std::string scopedBlendId = BuildScopedFactoryArchiveId(archiveKey, originalBlendId);
            blendIdMap[originalBlendId] = scopedBlendId;
            blend["id"] = scopedBlendId;

            if (blend.contains("models") && blend["models"].is_array())
            {
                for (auto& modelId : blend["models"])
                {
                    if (!modelId.is_string())
                        continue;
                    const auto mapped = resourceIdMap.find(modelId.get<std::string>());
                    if (mapped != resourceIdMap.end())
                        modelId = mapped->second;
                }
            }

            if (blend.contains("modelMappings") && blend["modelMappings"].is_array())
            {
                for (auto& mapping : blend["modelMappings"])
                {
                    if (!mapping.is_object())
                        continue;
                    const auto mapped = resourceIdMap.find(mapping.value("id", ""));
                    if (mapped != resourceIdMap.end())
                        mapping["id"] = mapped->second;
                }
            }

            mFactoryArchiveBlendIds.insert(scopedBlendId);

            bool replaced = false;
            for (auto& existing : mBlendLibrary)
            {
                if (existing.is_object() && existing.value("id", "") == scopedBlendId)
                {
                    existing = blend;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                mBlendLibrary.push_back(blend);
        }

        std::unordered_map<std::string, std::string> presetIdMapping;
        std::vector<std::string> importedPresetIds;
        for (auto preset : parsed.presets)
        {
            RemapPresetArchiveReferences(preset, resourceIdMap, blendIdMap);
            NormalizePresetScenes(preset);

            const std::string sourcePresetId = preset.id.empty()
                ? BuildScopedFactoryArchiveId(archiveKey, preset.name.empty() ? "preset" : preset.name)
                : preset.id;

            std::string uniquePresetId = archiveState["presetMappings"].value(sourcePresetId, std::string{});
            if (uniquePresetId.empty())
            {
                const std::string basePresetId = BuildScopedFactoryArchiveId(archiveKey, sourcePresetId.empty() ? "preset" : sourcePresetId);
                std::size_t suffix = 2;
                uniquePresetId = basePresetId;
                while ((occupiedPresetIds.contains(uniquePresetId) || mFactoryArchivePresets.contains(uniquePresetId))
                       && !trackedPresetIds.contains(uniquePresetId))
                {
                    uniquePresetId = basePresetId + "-" + std::to_string(suffix++);
                }
            }

            archiveState["presetMappings"][sourcePresetId] = uniquePresetId;
            presetIdMapping[sourcePresetId] = uniquePresetId;
            importedPresetIds.push_back(uniquePresetId);
            mFactoryArchivePresetAliases[sourcePresetId] = uniquePresetId;
            mFactoryArchivePresetIds.insert(uniquePresetId);
            mTrackedFactoryArchivePresetIds.insert(uniquePresetId);
            occupiedPresetIds.insert(uniquePresetId);

            preset.id = uniquePresetId;
            preset.category = "Factory";

            if ((archiveChanged || !PresetStorage::ExistsInStore(Store(), preset.id))
                && !PresetStorage::SaveToStore(Store(), preset))
            {
                AppendSessionLog("Factory preset archive preset write failed: " + preset.id);
            }

            mFactoryArchivePresets[preset.id] = std::move(preset);
        }

        UpdateFactoryPresetFolders(Store(),
                       archiveKey,
                       parsed.presetFolders,
                       presetIdMapping,
                       importedPresetIds);

        archiveState["hash"] = archiveHash;
        archiveState["fileName"] = entry.path().filename().string();
        factoryArchiveState["archives"][archiveKey] = archiveState;

        if (parsed.tone3000ResourceCount > 0)
        {
            AppendSessionLog("Factory preset archive contains tone3000 resource references that are not auto-imported at startup: "
                             + entry.path().filename().string());
        }
    }

    Store().Put(storage::ItemType::kDocument, kFactoryArchiveStateDocumentId, factoryArchiveState);
    InvalidateResourceUsageIndex();
}

} // namespace guitarfx
