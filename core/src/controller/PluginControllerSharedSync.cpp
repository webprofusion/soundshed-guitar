/**
 * PluginControllerSharedSync.cpp - Keeping several instances of the app in step.
 *
 * Presets, resources, settings and the rest live in one document store that
 * every instance (and the standalone app) writes to. A writer bumps a version
 * counter in a single shared-sync document naming the domains it touched
 * (TouchSharedSyncState); every other instance notices the new version on its
 * idle poll and reloads those sources from disk.
 *
 * The counter is read and written inside one transaction on purpose: reading it
 * outside lets two instances see the same version and both write version+1, so
 * one notification is lost and everyone else shows stale data until the next
 * unrelated change.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "resources/ResourceLibrary.h"
#include "util/PathEncoding.h"

#include <iostream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::ReloadSharedSyncSourcesFromDisk()
{
    // LoadAppSettings() replaces mAppSettings wholesale from the shared store, which would
    // otherwise drag this instance's NAM quality and editor layout back to whatever another
    // instance (or the standalone app) last wrote. kPreserveInstanceOwned re-asserts them
    // instead; the live values are members, so nothing needs snapshotting first.
    LoadAppSettings();

    if (ApplySettingsToRuntime(SettingsApplyMode::kPreserveInstanceOwned))
    {
        SaveAppSettings();
    }

    LoadResourceLibraries();
    LoadBlendLibrary();
    LoadCustomEffectLibrary();

    std::vector<std::string> definitionIds;
    definitionIds.reserve(mCompositeLibrary.GetAllDefinitions().size());

    for (const auto& def : mCompositeLibrary.GetAllDefinitions())
    {
        definitionIds.push_back(def.id);
    }

    for (const auto& id : definitionIds)
    {
        mCompositeLibrary.RemoveDefinition(id);
    }

    LoadCompositeLibrary();

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        mRiffLibraryIndex = LoadRiffLibraryIndex();
    }

    const auto automationData = LoadUiStorageJson("automation.json", nlohmann::json::object());

    if (!automationData.empty())
    {
        mAutomationSlots.LoadFromJson(automationData);
    }

    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    mSetlistBankSize = setlistsData.value("bankSize", 8);
    mSetlistCursorIndex = setlistsData.value("cursorIndex", 0);

    InvalidateResourceUsageIndex();
}

void PluginController::PollSharedSyncState()
{
    const auto now = std::chrono::steady_clock::now();

    if (now < mNextSharedSyncPollAt)
    {
        return;
    }

    mNextSharedSyncPollAt = now + kSharedSyncPollInterval;

    const auto payload =
        Store().Get(storage::ItemType::kDocument, kSharedSyncStateDocumentId).value_or(nlohmann::json::object());

    if (!payload.is_object())
    {
        return;
    }

    const auto versionIt = payload.find("version");

    if (versionIt == payload.end() || !versionIt->is_number_unsigned())
    {
        return;
    }

    const auto version = versionIt->get<std::uint64_t>();

    if (!mSharedSyncVersionSeenInitialized)
    {
        mSharedSyncVersionSeen = version;
        mSharedSyncVersionSeenInitialized = true;
        return;
    }

    if (version <= mSharedSyncVersionSeen)
    {
        return;
    }

    mSharedSyncVersionSeen = version;

    if (!mUIReady)
    {
        return;
    }

    nlohmann::json msg;
    msg["type"] = "sharedSyncUpdated";
    msg["version"] = version;

    if (payload.contains("domains") && payload["domains"].is_array())
    {
        msg["domains"] = payload["domains"];
    }

    if (payload.contains("updatedAt"))
    {
        msg["updatedAt"] = payload["updatedAt"];
    }

    SendMessageToUI(msg.dump());
}

void PluginController::HandleGetSharedSyncStateRequest()
{
    // Only act if the shared sync state has a new version since we last responded.
    const auto filePayload =
        Store().Get(storage::ItemType::kDocument, kSharedSyncStateDocumentId).value_or(nlohmann::json::object());
    std::uint64_t currentVersion = 0;

    if (filePayload.is_object())
    {
        const auto it = filePayload.find("version");

        if (it != filePayload.end() && it->is_number_unsigned())
        {
            currentVersion = it->get<std::uint64_t>();
        }
    }

    if (currentVersion > 0 && currentVersion == mSharedSyncVersionHandled)
    {
        return;
    }

    mSharedSyncVersionHandled = currentVersion;

    if (!mSharedSyncVersionSeenInitialized || currentVersion > mSharedSyncVersionSeen)
    {
        mSharedSyncVersionSeen = currentVersion;
        mSharedSyncVersionSeenInitialized = true;
    }

    ReloadSharedSyncSourcesFromDisk();

    nlohmann::json state;
    state["type"] = "sharedSyncState";
    state["appSettings"] = mAppSettings;
    state["uiSettings"] = mUiSettings;
    state["blendLibrary"] = mBlendLibrary;
    state["presetArchiveSession"] = {{"active", IsPresetArchiveSessionActive()}};

    if (mPresetArchiveSession)
    {
        state["presetArchiveSession"]["archiveName"] = mPresetArchiveSession->archiveName;
        state["presetArchiveSession"]["archiveKey"] = mPresetArchiveSession->archiveKey;
        state["presetArchiveSession"]["presetCount"] = mPresetArchiveSession->presetCount;
    }

    // Resource library summary + per-type entries for UI rendering
    nlohmann::json libraryInfo = nlohmann::json::object();
    auto allResources = mResourceLibrary.GetAllResources();
    libraryInfo["totalCount"] = allResources.size();

    for (const auto& resource : allResources)
    {
        const std::string type = resource.type;

        if (!libraryInfo.contains(type) || !libraryInfo[type].is_array())
        {
            libraryInfo[type] = nlohmann::json::array();
        }

        nlohmann::json entry;
        entry["id"] = resource.id;
        entry["name"] = resource.name;
        entry["category"] = resource.category;
        entry["description"] = resource.description;
        entry["tags"] = resource.tags;
        entry["filePath"] = resource.filePath.empty() ? "" : util::PathToUtf8(resource.filePath);
        entry["hash"] = resource.hash;

        if (!resource.metadata.empty())
        {
            entry["metadata"] = resource.metadata;
        }

        const bool hasPath = !resource.filePath.empty();
        const bool exists = hasPath && std::filesystem::exists(resource.filePath);
        entry["fileMissing"] = !(hasPath && exists);

        libraryInfo[type].push_back(entry);
    }

    state["resourceLibrary"] = std::move(libraryInfo);

    {
        nlohmann::json customEffects = nlohmann::json::array();

        for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
        {
            customEffects.push_back(SerializeCustomEffectLibraryEntry(entry));
        }

        state["customEffectLibrary"] = std::move(customEffects);
    }

    SendMessageToUI(state.dump());

    // Send auxiliary shared datasets over their existing message contracts.
    HandleGetThemeRequest();
    HandleGetPresetListRequest();
    HandleGetPresetFoldersRequest();
    HandleGetPresetFavoritesRequest();
    HandleGetPresetRatingsRequest();
    HandleGetSetlistsRequest();
    HandleGetAutomationRequest();
    SendCompositeLibraryToUI();
    SendCompositePresetListToUI();
    SendRiffLibraryStateToUI();
}

void PluginController::TouchSharedSyncState(const std::vector<std::string>& domains) const
{
    if (domains.empty())
    {
        return;
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["updatedAt"] = BuildUtcIsoTimestamp();
    payload["domains"] = nlohmann::json::array();

    for (const auto& domain : domains)
    {
        if (domain.empty())
        {
            continue;
        }

        payload["domains"].push_back(domain);
    }

    const auto instanceIdIt = mAppSettings.find("app.instanceId");

    if (instanceIdIt != mAppSettings.end() && instanceIdIt->is_string())
    {
        payload["writerInstanceId"] = instanceIdIt->get<std::string>();
    }

    // The counter has to be read and written under the same lock. Reading it
    // outside the transaction lets two instances see the same version and both
    // write version+1, so one instance's notification is lost and every other
    // instance keeps showing stale data until the next unrelated change.
    std::uint64_t nextVersion = 1;
    const bool wrote = Store().Transact([&]() {
        nextVersion = 1;

        if (const auto previous = Store().Get(storage::ItemType::kDocument, kSharedSyncStateDocumentId))
        {
            const auto versionIt = previous->find("version");

            if (versionIt != previous->end() && versionIt->is_number_unsigned())
            {
                nextVersion = versionIt->get<std::uint64_t>() + 1;
            }
        }

        payload["version"] = nextVersion;
        return Store().Put(storage::ItemType::kDocument, kSharedSyncStateDocumentId, payload);
    });

    if (!wrote)
    {
        std::cerr << "[Plugin] TouchSharedSyncState failed" << std::endl;
        return;
    }

    mSharedSyncVersionSeen = nextVersion;
    mSharedSyncVersionSeenInitialized = true;
}
} // namespace guitarfx
