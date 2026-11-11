/**
 * PluginControllerArchiveInstall.cpp - Installing tones downloaded from tone sharing into the
 * user's library, and removing them again.
 *
 * A UI (the web UI's Tones panel, Nano Q's) downloads a shared preset, or each preset of a
 * pack, and hands the zips over whole ("installPresetArchives"). They are unpacked as the web
 * UI's file import does it (importPresetArchive in core/ui/ts/presets/archive.ts): models and
 * IRs into the resource library, blends, the presets under new ids, and a folder for a pack.
 * What was added is recorded in the toneSharing.installedPacks setting both UIs read, in the
 * web UI's schema, so the install can be listed and taken out again
 * ("deleteInstalledPresetArchive").
 *
 * Where it differs from that path, on purpose: a model or IR the library already holds is
 * reused rather than copied again (matched on the engine's own content hash); the running
 * preset is left alone rather than replaced by the last one saved; and every archive is read
 * and every remote model resolved before anything is written, so a tone that cannot be
 * installed leaves nothing half-installed behind.
 */

#include "PluginController.h"

#include "controller/internal/BlendSupport.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "controller/internal/RemoteResourceRefs.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <unordered_set>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
constexpr const char* kInstalledPacksSettingKey = "toneSharing.installedPacks";
constexpr const char* kInstalledResourceProvider = "presetArchive";
constexpr const char* kInstalledResourceSubfolder = "preset-imports";
constexpr const char* kPackFolderIdPrefix = "tone-sharing::";

/// One downloaded archive, read and checked, waiting to be written.
struct PendingArchive
{
    ParsedFactoryPresetArchive parsed;
    std::string title;  // names the preset of a one-preset archive: the shared item's title
    std::string itemId; // the tone sharing item it was downloaded as, when the UI says
};

std::string ResourceKey(const std::string& type, const std::string& id)
{
    return type + ":" + id;
}

void EraseIds(nlohmann::json& array, const std::unordered_set<std::string>& ids)
{
    if (!array.is_array())
    {
        return;
    }

    array.erase(std::remove_if(array.begin(), array.end(),
                               [&](const nlohmann::json& value) {
                                   return value.is_string() && ids.contains(value.get<std::string>());
                               }),
                array.end());
}

/// Takes the presets out of every folder, and drops a folder they leave empty: one that held
/// some of them, or held only subfolders that went with them.
void PrunePresetFolders(nlohmann::json& folders, const std::unordered_set<std::string>& presetIds)
{
    if (!folders.is_array())
    {
        return;
    }

    nlohmann::json kept = nlohmann::json::array();

    for (auto folder : folders)
    {
        if (!folder.is_object())
        {
            continue;
        }

        auto& ids = folder["presetIds"];
        const auto idsBefore = ids.is_array() ? ids.size() : 0;
        EraseIds(ids, presetIds);

        auto& children = folder["children"];
        const auto childrenBefore = children.is_array() ? children.size() : 0;
        PrunePresetFolders(children, presetIds);

        const auto idsAfter = ids.is_array() ? ids.size() : 0;
        const auto childrenAfter = children.is_array() ? children.size() : 0;
        const bool emptied = idsAfter == 0 && childrenAfter == 0 && (idsBefore > 0 || childrenBefore > 0);

        if (!emptied)
        {
            kept.push_back(std::move(folder));
        }
    }

    folders = std::move(kept);
}

/// The folders a pack's own archive lays its presets out in (`presetFolders`), rebuilt under the
/// pack's folder with the installed presets' ids. A folder left with nothing in it is skipped.
nlohmann::json BuildPackSubfolders(const nlohmann::json& source, const std::string& parentId,
                                   const std::unordered_map<std::string, std::string>& presetIds,
                                   std::unordered_set<std::string>& placed)
{
    nlohmann::json result = nlohmann::json::array();

    if (!source.is_array())
    {
        return result;
    }

    for (const auto& node : source)
    {
        const std::string name = node.is_object() ? node.value("name", "") : std::string{};

        if (name.empty())
        {
            continue;
        }

        auto folder = MakePresetFolderEntry(parentId + "::" + GenerateGuidV4String(), name);

        for (const auto& id : node.value("presetIds", nlohmann::json::array()))
        {
            if (const auto mapped = id.is_string() ? presetIds.find(id.get<std::string>()) : presetIds.end();
                mapped != presetIds.end())
            {
                folder["presetIds"].push_back(mapped->second);
                placed.insert(mapped->second);
            }
        }

        folder["children"] = BuildPackSubfolders(node.value("children", nlohmann::json::array()),
                                                 folder.value("id", ""), presetIds, placed);

        if (!folder["presetIds"].empty() || !folder["children"].empty())
        {
            result.push_back(std::move(folder));
        }
    }

    return result;
}
} // namespace

void PluginController::HandleInstallPresetArchivesRequest(const nlohmann::json& payload)
{
    const std::string requestId = payload.value("requestId", "");
    const nlohmann::json entryPayload = payload.value("entry", nlohmann::json::object());
    const std::string entryId = entryPayload.value("id", "");
    const std::string entryTitle = entryPayload.value("title", "");
    const std::string folderName = payload.value("folder", "");
    const nlohmann::json archivesPayload = payload.value("archives", nlohmann::json::array());
    const bool allowMissingRemote = payload.value("allowMissingRemote", false);
    const nlohmann::json fetchedPayload = payload.value("fetchedResources", nlohmann::json::array());

    const auto fail = [&](const std::string& message, const std::string& detail) {
        ReportErrorToUI(message, detail);
        SendMessageToUI(nlohmann::json{
            {"type", "presetArchivesInstallFailed"},
            {"requestId", requestId},
            {"entryId", entryId},
            {"message", message},
            {"detail",
             detail}}.dump());
    };

    if (entryId.empty() || !archivesPayload.is_array() || archivesPayload.empty())
    {
        fail("Install failed", "Nothing to install");
        return;
    }

    // ── Read and check everything first ─────────────────────────────────────────
    std::vector<PendingArchive> archives;

    for (const auto& archivePayload : archivesPayload)
    {
        const auto bytes = util::DecodeBase64(archivePayload.value("data", ""));
        const std::string title = archivePayload.value("title", "");
        std::string error;
        auto parsed = bytes.empty() ? std::nullopt
                                    : ParseFactoryPresetArchive(util::PathFromUtf8(title.empty() ? "archive" : title),
                                                                bytes, error);

        if (!parsed)
        {
            fail("Install failed", (title.empty() ? std::string("A download") : "\"" + title + "\"") +
                                       " could not be read: " + (error.empty() ? "not a preset archive" : error));
            return;
        }

        archives.push_back({std::move(*parsed), title, archivePayload.value("itemId", "")});
    }

    // A remote model or IR is shared as a reference, not a file: it has to be in the library
    // already (installed in Sepia, which can download it from its provider).
    std::unordered_map<std::string, std::string> remoteIds;
    std::vector<std::string> missingRemote;
    std::vector<std::string> missingProviders;

    for (const auto& archive : archives)
    {
        for (const auto& reference : archive.parsed.remoteResources)
        {
            const std::string id = reference.value("id", "");
            const std::string found = FindRemoteResource(mResourceLibrary, reference);

            if (found.empty())
            {
                const std::string name = reference.value("name", "");
                const std::string provider = reference.value("provider", "");
                missingRemote.push_back(name.empty() ? id : name);

                if (std::find(missingProviders.begin(), missingProviders.end(), provider) == missingProviders.end())
                {
                    missingProviders.push_back(provider);
                }
            }
            else if (!id.empty())
            {
                remoteIds[id] = found;
            }
        }
    }

    // A UI that has already tried to fetch them may install without the ones it could not have
    // (a model its creator removed or hid): their slots stay empty, and the reply names them.
    if (!missingRemote.empty() && !allowMissingRemote)
    {
        const auto join = [](const std::vector<std::string>& items) {
            std::string joined;

            for (const auto& item : items)
            {
                joined += (joined.empty() ? "" : ", ") + item;
            }

            return joined;
        };
        const bool onlyTone3000 = missingProviders.size() == 1 && missingProviders.front() == "tone3000";
        const std::string from = onlyTone3000 ? "Tone3000 models" : "models from " + join(missingProviders);

        fail(onlyTone3000 ? "Needs Tone3000 models" : "Needs remote models",
             "This tone uses " + from + " that are not in your library (" + join(missingRemote) +
                 "). Install it in Sepia, which can download them.");
        return;
    }

    // ── Write ───────────────────────────────────────────────────────────────────
    nlohmann::json installedResources = nlohmann::json::array();
    nlohmann::json installedBlends = nlohmann::json::array();
    std::vector<std::string> presetIds;
    nlohmann::json presetSignatures = nlohmann::json::object();
    nlohmann::json presetItems = nlohmann::json::object(); // preset id -> its tone sharing item
    std::unordered_set<std::string> recordedResources;
    nlohmann::json packSubfolders = nlohmann::json::array();
    std::unordered_set<std::string> presetsInSubfolders;
    const std::string packId = entryPayload.value("packId", "");
    const std::string packFolderId = kPackFolderIdPrefix + (packId.empty() ? GenerateGuidV4String() : packId);
    bool blendsChanged = false;

    if (!mBlendLibrary.is_array())
    {
        mBlendLibrary = nlohmann::json::array();
    }

    for (auto& archive : archives)
    {
        auto resourceIds = remoteIds;
        std::unordered_map<std::string, std::string> blendIds;

        for (const auto& file : archive.parsed.resources)
        {
            // The library may hold these bytes already, from this tone or another. Matched on the
            // engine's hash; failing that, a file of the archive's own name (the web UI names them
            // <sha256>-<name>) holding the same bytes, since an older import may have recorded
            // another kind of hash.
            const std::string contentHash = mHasher.HashBytes(file.bytes);
            const auto archiveFileName = util::SanitizeFilename(file.fileName);
            std::string libraryId;

            for (const auto& resource : mResourceLibrary.GetAllResources())
            {
                std::error_code ec;

                if (resource.type != file.type || resource.filePath.empty() ||
                    !std::filesystem::exists(resource.filePath, ec))
                {
                    continue;
                }

                if (resource.hash == contentHash ||
                    (!archiveFileName.empty() && util::PathToUtf8(resource.filePath.filename()) == archiveFileName &&
                     mHasher.HashFile(resource.filePath) == contentHash))
                {
                    libraryId = resource.id;
                    break;
                }
            }

            // Only what this install added is recorded, so taking it out again never removes a
            // model the user had before it.
            if (libraryId.empty())
            {
                LibraryResource resource;
                resource.type = file.type;
                resource.id = GenerateGuidV4String();
                resource.name = file.name.empty() ? file.id : file.name;
                resource.category = file.category;
                resource.metadata["archiveProvider"] = kInstalledResourceProvider;
                resource.metadata["sourceFile"] = file.fileName;

                std::string error;
                const auto imported = ImportResourceFile(std::move(resource), kInstalledResourceProvider,
                                                         kInstalledResourceSubfolder, file.fileName, file.bytes, error);

                if (!imported)
                {
                    fail("Install failed", "Could not add \"" + file.name + "\" to the library: " + error);
                    return;
                }

                libraryId = imported->id;

                if (recordedResources.insert(ResourceKey(file.type, libraryId)).second)
                {
                    installedResources.push_back({{"type", file.type}, {"id", libraryId}});
                }
            }

            resourceIds[file.id] = libraryId;
        }

        for (auto blend : archive.parsed.blends)
        {
            const std::string originalId = blend.value("id", "");

            if (originalId.empty())
            {
                continue;
            }

            const std::string newId = GenerateGuidV4String();
            blendIds[originalId] = newId;
            blend["id"] = newId;
            blend.erase(kFactoryBlendFlag);

            if (blend.contains("models") && blend["models"].is_array())
            {
                for (auto& modelId : blend["models"])
                {
                    if (modelId.is_string())
                    {
                        if (const auto mapped = resourceIds.find(modelId.get<std::string>());
                            mapped != resourceIds.end())
                        {
                            modelId = mapped->second;
                        }
                    }
                }
            }

            if (blend.contains("modelMappings") && blend["modelMappings"].is_array())
            {
                for (auto& mapping : blend["modelMappings"])
                {
                    if (mapping.is_object())
                    {
                        if (const auto mapped = resourceIds.find(mapping.value("id", "")); mapped != resourceIds.end())
                        {
                            mapping["id"] = mapped->second;
                        }
                    }
                }
            }

            mBlendLibrary.push_back(blend);
            installedBlends.push_back(newId);
            blendsChanged = true;
        }

        const bool onePreset = archive.parsed.presets.size() == 1;
        std::unordered_map<std::string, std::string> presetIdMap;

        for (auto preset : archive.parsed.presets)
        {
            const std::string archivePresetId = preset.id;
            RemapPresetArchiveReferences(preset, resourceIds, blendIds);

            // The web UI's archives also name a model or IR in these older config keys.
            const auto remapConfigKeys = [&](SignalGraph& graph) {
                for (auto& node : graph.nodes)
                {
                    for (const char* key : {"audioFxModelId", "irId"})
                    {
                        if (auto it = node.config.find(key); it != node.config.end())
                        {
                            if (const auto mapped = resourceIds.find(it->second); mapped != resourceIds.end())
                            {
                                it->second = mapped->second;
                            }
                        }
                    }
                }
            };

            remapConfigKeys(preset.graph);

            for (auto& scene : preset.scenes)
            {
                remapConfigKeys(scene.graph);
            }

            // A shared tone carries its own sound, not the sharer's rig: no global chain or trims.
            preset.globalSignalChain.reset();
            preset.global = GlobalSettings{};
            preset.id = GenerateUserPresetId();

            if (onePreset && !archive.title.empty())
            {
                preset.name = archive.title;
            }

            if (preset.name.empty())
            {
                preset.name = entryTitle.empty() ? "Shared preset" : entryTitle;
            }

            if (preset.category.empty())
            {
                preset.category = "User";
            }

            NormalizePresetScenes(preset);
            EnsurePresetBoundaryGainNodes(preset);

            if (!SaveUserPreset(preset))
            {
                fail("Install failed", "Could not save \"" + preset.name + "\"");
                return;
            }

            const auto presetJson = PresetStorage::SerializeToJson(preset);
            presetSignatures[preset.id] = mHasher.HashBytes(
                std::span(reinterpret_cast<const std::uint8_t*>(presetJson.data()), presetJson.size()));
            presetIds.push_back(preset.id);

            if (!archive.itemId.empty())
            {
                presetItems[preset.id] = archive.itemId;
            }

            if (!archivePresetId.empty())
            {
                presetIdMap[archivePresetId] = preset.id;
            }
        }

        if (!folderName.empty())
        {
            for (auto& folder :
                 BuildPackSubfolders(archive.parsed.presetFolders, packFolderId, presetIdMap, presetsInSubfolders))
            {
                packSubfolders.push_back(std::move(folder));
            }
        }
    }

    if (blendsChanged)
    {
        SaveBlendLibrary();
    }

    // The models the UI fetched for this install's remote references just before sending it: they
    // were added for it, so they are recorded with it and leave with it.
    for (const auto& fetched : fetchedPayload)
    {
        const std::string type = fetched.is_object() ? fetched.value("type", "") : std::string{};
        const std::string id = fetched.is_object() ? fetched.value("id", "") : std::string{};

        if (!type.empty() && !id.empty() && mResourceLibrary.LookupResource(type, id).has_value() &&
            recordedResources.insert(ResourceKey(type, id)).second)
        {
            installedResources.push_back({{"type", type}, {"id", id}});
        }
    }

    // ── A pack gets a folder of its own ─────────────────────────────────────────
    bool foldersChanged = false;

    if (!folderName.empty() && !presetIds.empty())
    {
        auto document = LoadUiStorageJson("preset-folders.json", nlohmann::json::object());
        auto folders = document.value("folders", nlohmann::json::array());

        if (!folders.is_array())
        {
            folders = nlohmann::json::array();
        }

        const std::string& folderId = packFolderId;
        auto folder = MakePresetFolderEntry(folderId, folderName);

        // The pack's own subfolders, when its archive has them; the rest sit in the pack's folder.
        for (const auto& id : presetIds)
        {
            if (!presetsInSubfolders.contains(id))
            {
                folder["presetIds"].push_back(id);
            }
        }

        folder["children"] = packSubfolders;

        const auto existing = std::find_if(folders.begin(), folders.end(), [&](const nlohmann::json& candidate) {
            return candidate.is_object() && candidate.value("id", "") == folderId;
        });

        if (existing != folders.end())
        {
            *existing = folder;
        }
        else
        {
            folders.push_back(folder);
        }

        document["folders"] = folders;

        if (!document.contains("activeFolderId"))
        {
            document["activeFolderId"] = "__all__";
        }

        SaveUiStorageJson("preset-folders.json", document);
        foldersChanged = true;
    }

    // ── Record it, in the web UI's installedPacks schema ────────────────────────
    nlohmann::json entry = {
        {"id", entryId},
        {"title", entryTitle.empty() ? (presetIds.size() == 1 ? archives.front().title : std::string("Shared tones"))
                                     : entryTitle},
        {"source", entryPayload.value("source", "toneSharingApi")},
        {"importedAt", BuildUtcIsoTimestamp()},
        {"presetIds", presetIds},
        {"presetSignatures", presetSignatures},
        {"resources", installedResources},
        {"blends", installedBlends},
    };

    if (!packId.empty())
    {
        entry["packId"] = packId;
    }

    // Which item each preset came from, so a favourite or rating given to it can follow it back
    // to the service (a pack's presets are its items; a single item's id is in the entry's id).
    if (!presetItems.empty())
    {
        entry["presetItems"] = presetItems;
    }

    // Shown on the web UI's Installed card.
    if (const std::string archiveFileName = entryPayload.value("archiveFileName", ""); !archiveFileName.empty())
    {
        entry["archiveFileName"] = archiveFileName;
    }

    auto installed = mAppSettings.value(kInstalledPacksSettingKey, nlohmann::json::array());

    if (!installed.is_array())
    {
        installed = nlohmann::json::array();
    }

    installed.erase(
        std::remove_if(installed.begin(), installed.end(),
                       [&](const nlohmann::json& e) { return e.is_object() && e.value("id", "") == entryId; }),
        installed.end());
    installed.insert(installed.begin(), entry);
    mAppSettings[kInstalledPacksSettingKey] = installed;
    SaveAppSettings();

    // ── Tell everyone ───────────────────────────────────────────────────────────
    InvalidateResourceUsageIndex();
    TouchSharedSyncState({"presetLibrary", "resourceLibrary", "appSettings"});
    BroadcastState();
    SendPresetListToUI();

    if (foldersChanged)
    {
        HandleGetPresetFoldersRequest();
    }

    SendMessageToUI(
        nlohmann::json{{"type", "appSettingChanged"}, {"key", kInstalledPacksSettingKey}, {"value", installed}}.dump());
    nlohmann::json installedReply = {{"type", "presetArchivesInstalled"},
                                     {"requestId", requestId},
                                     {"entryId", entryId},
                                     {"presetIds", presetIds},
                                     {"resourceCount", installedResources.size()}};

    if (!missingRemote.empty())
    {
        installedReply["missingRemote"] = missingRemote;
    }

    SendMessageToUI(installedReply.dump());
    AppendSessionLog("Installed " + entryId + ": " + std::to_string(presetIds.size()) + " preset(s), " +
                     std::to_string(installedResources.size()) + " resource(s)");
}

void PluginController::HandleDeleteInstalledPresetArchiveRequest(const nlohmann::json& payload)
{
    const std::string entryId = payload.value("id", "");
    auto installed = mAppSettings.value(kInstalledPacksSettingKey, nlohmann::json::array());

    if (entryId.empty() || !installed.is_array())
    {
        return;
    }

    const auto entryIt = std::find_if(installed.begin(), installed.end(), [&](const nlohmann::json& e) {
        return e.is_object() && e.value("id", "") == entryId;
    });

    // Still answered, removing nothing, so a UI waiting on it (one whose list was out of date)
    // can drop the entry and stop waiting.
    if (entryIt == installed.end())
    {
        ReportErrorToUI("Not installed", entryId);
        SendMessageToUI(nlohmann::json{
            {"type", "installedPresetArchiveDeleted"}, {"id", entryId}, {"presetIds", nlohmann::json::array()}}
                            .dump());
        return;
    }

    const nlohmann::json entry = *entryIt;
    installed.erase(entryIt);

    // The presets that are still there.
    std::unordered_set<std::string> removedPresetIds;

    for (const auto& value : entry.value("presetIds", nlohmann::json::array()))
    {
        if (value.is_string() && UserPresetExists(value.get<std::string>()) &&
            DeleteUserPreset(value.get<std::string>()))
        {
            removedPresetIds.insert(value.get<std::string>());
            ForgetPresetAutomation(value.get<std::string>());
        }
    }

    // Out of the folders, favourites and ratings too.
    auto folderDocument = LoadUiStorageJson("preset-folders.json", nlohmann::json::object());
    auto folders = folderDocument.value("folders", nlohmann::json::array());
    PrunePresetFolders(folders, removedPresetIds);
    folderDocument["folders"] = folders;
    SaveUiStorageJson("preset-folders.json", folderDocument);

    auto favoritesDocument = LoadUiStorageJson("preset-favorites.json", nlohmann::json::object());
    auto favorites = favoritesDocument.value("favorites", nlohmann::json::array());
    EraseIds(favorites, removedPresetIds);
    favoritesDocument["favorites"] = favorites;
    SaveUiStorageJson("preset-favorites.json", favoritesDocument);

    auto ratingsDocument = LoadUiStorageJson("preset-ratings.json", nlohmann::json::object());
    auto ratings = ratingsDocument.value("ratings", nlohmann::json::object());

    for (const auto& id : removedPresetIds)
    {
        if (ratings.is_object())
        {
            ratings.erase(id);
        }
    }

    ratingsDocument["ratings"] = ratings;
    SaveUiStorageJson("preset-ratings.json", ratingsDocument);

    // Its blends, unless a preset still plays one.
    std::unordered_set<std::string> blendIds;

    for (const auto& value : entry.value("blends", nlohmann::json::array()))
    {
        if (value.is_string())
        {
            blendIds.insert(value.get<std::string>());
        }
    }

    if (!blendIds.empty() && mBlendLibrary.is_array())
    {
        std::unordered_set<std::string> blendsInUse;

        for (const auto& preset : LoadAllUserPresets())
        {
            ForEachPresetGraph(preset, [&](const SignalGraph& graph) {
                for (const auto& node : graph.nodes)
                {
                    if (const auto it = node.config.find("blendId"); it != node.config.end())
                    {
                        blendsInUse.insert(it->second);
                    }
                }
            });
        }

        const auto before = mBlendLibrary.size();
        mBlendLibrary.erase(std::remove_if(mBlendLibrary.begin(), mBlendLibrary.end(),
                                           [&](const nlohmann::json& blend) {
                                               const std::string id = blend.value("id", "");
                                               return blendIds.contains(id) && !blendsInUse.contains(id);
                                           }),
                            mBlendLibrary.end());

        if (mBlendLibrary.size() != before)
        {
            SaveBlendLibrary();
        }
    }

    mAppSettings[kInstalledPacksSettingKey] = installed;
    SaveAppSettings();
    InvalidateResourceUsageIndex();

    // Its models and IRs: the cleanup keeps anything a remaining preset or blend still uses,
    // and the running preset's, so a pack being played keeps its sound until another loads.
    const auto resources = entry.value("resources", nlohmann::json::array());

    if (resources.is_array() && !resources.empty())
    {
        HandleCleanupResourceLibraryRequest({{"scope", "all"}, {"removeFiles", true}, {"resources", resources}});
    }

    TouchSharedSyncState({"presetLibrary", "resourceLibrary", "appSettings"});
    BroadcastState();
    SendPresetListToUI();
    HandleGetPresetFoldersRequest();
    HandleGetPresetFavoritesRequest();
    HandleGetPresetRatingsRequest();
    SendMessageToUI(
        nlohmann::json{{"type", "appSettingChanged"}, {"key", kInstalledPacksSettingKey}, {"value", installed}}.dump());

    nlohmann::json removed = nlohmann::json::array();

    for (const auto& id : removedPresetIds)
    {
        removed.push_back(id);
    }

    SendMessageToUI(
        nlohmann::json{{"type", "installedPresetArchiveDeleted"}, {"id", entryId}, {"presetIds", removed}}.dump());
    AppendSessionLog("Removed " + entryId + ": " + std::to_string(removedPresetIds.size()) + " preset(s)");
}
} // namespace guitarfx
