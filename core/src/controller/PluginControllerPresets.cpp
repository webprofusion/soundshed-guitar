/**
 * PluginControllerPresets.cpp - Loading, saving and applying presets, and the
 * user preset store behind them.
 *
 * ApplyPreset is the single path from a stored preset to a running graph;
 * everything that changes what is playing goes through it so runtime
 * callbacks, blend expansion and hosted-plugin state are handled once.
 */

#include "PluginController.h"

#include "controller/TunerService.h"

#include "GuitarFXConfig.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "presets/PresetStorage.h"

#include <algorithm>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::HandlePresetLoadRequest(const nlohmann::json& payload)
{
    // A UI scene switch arrives here as a load of the same preset with a different sceneId,
    // so the outgoing scene's live plugin state has to be banked before anything else runs.
    CaptureLiveHostedPluginStateIntoActivePreset();

    try
    {
        Preset preset;
        std::optional<Preset> presetOpt;

        if (payload.contains("preset"))
        {
            presetOpt = PresetStorage::DeserializeFromJson(payload["preset"].dump());
        }
        else
        {
            presetOpt = PresetStorage::DeserializeFromJson(payload.dump());
        }

        if (!presetOpt)
        {
            return;
        }

        preset = std::move(*presetOpt);

        const std::string requestedPresetId = payload.value("presetId", preset.id);
        const bool scrubbedHostedState = PresetHasScrubbedHostedPluginState(preset);
        AppendSessionLog("Hosted plugin load request presetId=" +
                         (requestedPresetId.empty() ? std::string{"<none>"} : requestedPresetId) +
                         ", scrubbed=" + std::string{scrubbedHostedState ? "true" : "false"} +
                         ", payload=" + SummarizeHostedPluginState(preset));

        if (!requestedPresetId.empty() && scrubbedHostedState)
        {
            if (auto storedPreset = TryLoadStoredPresetById(requestedPresetId))
            {
                AppendSessionLog("Hosted plugin load rehydrated presetId=" + requestedPresetId +
                                 " from authoritative source: " + SummarizeHostedPluginState(*storedPreset));
                preset = std::move(*storedPreset);
            }
            else
            {
                AppendSessionLog("Hosted plugin load could not rehydrate presetId=" + requestedPresetId);
            }
        }

        NormalizePresetScenes(preset);

        const std::string requestedSceneId = payload.value("sceneId", "");

        if (!SetPresetActiveScene(preset, requestedSceneId, &mActiveSceneId))
        {
            mActiveSceneId = GetDefaultPresetSceneId(preset);
        }

        ApplyBlendDefinitions(preset);

        AppendSessionLog(
            "Hosted plugin load applying presetId=" + (requestedPresetId.empty() ? preset.id : requestedPresetId) +
            ", final=" + SummarizeHostedPluginState(preset));

        mActivePresetId = requestedPresetId.empty() ? preset.id : requestedPresetId;

        // If this preset is already one of several active mixer slots (e.g. the user is
        // switching scenes on a preset that's part of a multi-preset mix), rebuild just
        // that slot in place. ApplyPreset()'s PreparePresetSwap()/CommitPresetSwap() swap
        // the *entire* mixer down to a single instance, which would silently drop every
        // other active mixer preset.
        const auto activeMixerIds = mPresetMixer.GetActivePresetIds();
        const bool isActiveMixerSlot =
            activeMixerIds.size() > 1 &&
            std::find(activeMixerIds.begin(), activeMixerIds.end(), mActivePresetId) != activeMixerIds.end();

        if (isActiveMixerSlot && ReplaceActiveMixerPresetInPlace(preset, mActivePresetId, preset.name))
        {
            mActivePreset = preset;
            mActivePresetJson = PresetStorage::SerializeToJson(preset);
        }
        else
        {
            ApplyPreset(preset); // SetGlobalChainConfig is called inside ApplyPreset under mDSPMutex
        }

        mPendingPresetStateBroadcast = true;

        // Send explicit "presetLoaded" confirmation to the UI
        {
            nlohmann::json loaded;
            loaded["type"] = "presetLoaded";
            loaded["preset"] = SerializePresetForUi(*mActivePreset);
            nlohmann::json activeIds = nlohmann::json::array();

            for (const auto& id : mPresetMixer.GetActivePresetIds())
            {
                activeIds.push_back(id);
            }

            loaded["activePresetIds"] = activeIds;
            loaded["sceneId"] = GetResolvedActiveSceneId();
            SendMessageToUI(loaded.dump());
        }

        if (mHost.IsStandalone() && !IsPresetArchiveSessionActive())
        {
            mAppSettings["lastPresetId"] = mActivePresetId;
            SaveAppSettings();
        }
    }
    catch (const std::exception& e)
    {
        ReportErrorToUI("Failed to load preset", e.what());
    }
}

std::optional<Preset> PluginController::TryLoadStoredPresetById(const std::string& presetId)
{
    if (presetId.empty())
    {
        return std::nullopt;
    }

    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : presetId;

    if (mActivePreset)
    {
        const bool matchesActivePreset =
            mActivePreset->id == resolvedPresetId ||
            (!mActivePresetId.empty() && (mActivePresetId == presetId || mActivePresetId == resolvedPresetId));

        if (matchesActivePreset)
        {
            Preset preset = *mActivePreset;
            CaptureRuntimePluginStates(preset, mActivePresetId.empty() ? resolvedPresetId : mActivePresetId);
            AppendSessionLog("Hosted plugin rehydrate source=active presetId=" + resolvedPresetId +
                             ", state=" + SummarizeHostedPluginState(preset));
            return preset;
        }
    }

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
    {
        return std::nullopt;
    }

    if (auto presetOpt = LoadUserPreset(resolvedPresetId))
    {
        AppendSessionLog("Hosted plugin rehydrate source=user-store presetId=" + resolvedPresetId +
                         ", state=" + SummarizeHostedPluginState(*presetOpt));
        return presetOpt;
    }

    if (IsPresetArchiveSessionActive())
    {
        return std::nullopt;
    }

    const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");

    if (std::filesystem::exists(factoryPath))
    {
        if (auto presetOpt = PresetStorage::LoadFromFile(factoryPath))
        {
            AppendSessionLog("Hosted plugin rehydrate source=factory-file presetId=" + resolvedPresetId + ", path=" +
                             factoryPath.generic_string() + ", state=" + SummarizeHostedPluginState(*presetOpt));
            return presetOpt;
        }
    }

    const auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);

    if (archiveIt != mFactoryArchivePresets.end())
    {
        AppendSessionLog("Hosted plugin rehydrate source=factory-archive presetId=" + resolvedPresetId +
                         ", state=" + SummarizeHostedPluginState(archiveIt->second));
        return archiveIt->second;
    }

    return std::nullopt;
}

void PluginController::HandleSavePresetRequest(const nlohmann::json& payload)
{
    const std::string presetName = payload.value("name", "");
    const std::string presetCategory = payload.value("category", "User");
    const std::string presetDescription = payload.value("description", "");
    const std::string presetIdOverride = payload.value("presetId", "");
    const std::string saveMode = payload.value("saveMode", "");
    std::string sourcePresetId = payload.value("sourcePresetId", "");
    const bool requireNewPresetId = payload.value("requireNewPresetId", saveMode == "save-as");
    const bool includeGlobalSignalChain =
        payload.value("includeGlobalSignalChain", payload.contains("globalSignalChain"));

    std::optional<Preset> payloadPreset;

    if (payload.contains("preset") && payload["preset"].is_object())
    {
        payloadPreset = PresetStorage::DeserializeFromJson(payload["preset"].dump());
    }

    if (presetName.empty())
    {
        ReportErrorToUI("Cannot save preset", "Preset name is required");
        return;
    }

    if (!payloadPreset)
    {
        EnsureBasicGraph();

        if (!mActivePreset)
        {
            ReportErrorToUI("Cannot save preset", "No active preset to save");
            return;
        }
    }

    try
    {
        Preset newPreset = payloadPreset ? *payloadPreset : *mActivePreset;
        NormalizePresetScenes(newPreset);
        EnsurePresetBoundaryGainNodes(newPreset);

        if (sourcePresetId.empty())
        {
            sourcePresetId = mActivePresetId;
        }

        std::string resolvedPresetId = presetIdOverride.empty() ? GenerateUserPresetId() : presetIdOverride;

        if (requireNewPresetId && !sourcePresetId.empty() && resolvedPresetId == sourcePresetId)
        {
            resolvedPresetId = GenerateUserPresetId();
        }

        newPreset.id = std::move(resolvedPresetId);
        newPreset.name = presetName;
        newPreset.category = presetCategory;
        newPreset.description = presetDescription;
        newPreset.version = 2;

        auto currentChain = mPresetMixer.GetGlobalChainConfig();
        currentChain.autoLevelInput = false;
        currentChain.autoLevelOutput = false;

        newPreset.global.inputTrim = currentChain.inputGain;
        newPreset.global.outputTrim = currentChain.outputGain;
        newPreset.global.transpose = GetGlobalTransposeFromChainConfig(currentChain);
        newPreset.global.autoLevelInput = false;
        newPreset.global.autoLevelOutput = false;

        if (includeGlobalSignalChain)
        {
            if (payload.contains("globalSignalChain") && payload["globalSignalChain"].is_object())
            {
                newPreset.globalSignalChain = payload["globalSignalChain"].get<GlobalSignalChainConfig>();
            }
            else if (newPreset.globalSignalChain.has_value())
            {
                newPreset.globalSignalChain = newPreset.globalSignalChain.value();
            }
            else
            {
                newPreset.globalSignalChain = currentChain;
            }

            if (newPreset.globalSignalChain.has_value())
            {
                newPreset.globalSignalChain->inputGain = currentChain.inputGain;
                newPreset.globalSignalChain->outputGain = currentChain.outputGain;
                newPreset.globalSignalChain->autoLevelInput = false;
                newPreset.globalSignalChain->autoLevelOutput = false;
            }
        }
        else
        {
            newPreset.globalSignalChain.reset();
        }

        const std::string requestedSceneId = payload.value("sceneId", mActiveSceneId);

        if (!SetPresetActiveScene(newPreset, requestedSceneId, &mActiveSceneId))
        {
            mActiveSceneId = GetDefaultPresetSceneId(newPreset);
        }

        AppendSessionLog("Hosted plugin preset save begin presetId=" + newPreset.id +
                         ", sourcePresetId=" + (sourcePresetId.empty() ? std::string{"<none>"} : sourcePresetId) +
                         ", beforeCapture=" + SummarizeHostedPluginState(newPreset));

        CaptureRuntimePluginStates(newPreset, sourcePresetId.empty() ? mActivePresetId : sourcePresetId);

        AppendSessionLog("Hosted plugin preset save captured presetId=" + newPreset.id +
                         ", afterCapture=" + SummarizeHostedPluginState(newPreset));

        if (!SaveUserPreset(newPreset))
        {
            ReportErrorToUI("Failed to save preset", "Could not write the preset to the library");
            return;
        }

        AppendSessionLog("Hosted plugin preset save wrote presetId=" + newPreset.id +
                         ", state=" + SummarizeHostedPluginState(newPreset));

        // "Save as" mints a new preset id, but the running mixer slot is still keyed by the
        // old one and nothing re-applies the preset here. Left alone, the two drift apart:
        // runtime capture callbacks arrive tagged with the slot id and get dropped for not
        // matching mActivePresetId, while manual capture looks up the new id and finds no
        // instance at all. Re-key the slot instead of rebuilding it — a rebuild would tear
        // down every hosted plugin (and close its editor) as a side effect of saving.
        const std::string previousSlotId = mActivePresetId;

        if (!previousSlotId.empty() && previousSlotId != newPreset.id)
        {
            bool renamed = false;
            {
                std::lock_guard<std::mutex> lock(mDSPMutex);
                renamed = mPresetMixer.RenameActivePreset(previousSlotId, newPreset.id, newPreset.name);

                if (renamed)
                {
                    // The callbacks captured the old slot id by value, so they have to be
                    // re-bound or every capture would keep reporting the retired id.
                    AttachRuntimeConfigCallbacks(newPreset.id, newPreset);
                }
            }

            if (renamed)
            {
                mMixerPresetJsonCache.erase(previousSlotId);
                AppendSessionLog("Mixer slot re-keyed after save from " + previousSlotId + " to " + newPreset.id);
            }
            else
            {
                AppendSessionLog("Mixer slot re-key skipped after save from " + previousSlotId + " to " + newPreset.id +
                                 " (slot not active or id already in use)");
            }
        }

        mActivePreset = newPreset;
        mActivePresetId = newPreset.id;
        mActivePresetJson = PresetStorage::SerializeToJson(newPreset);
        mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
        mPendingStateBroadcast = true;

        if (!IsPresetArchiveSessionActive())
        {
            SaveAppSettings();
        }

        TouchSharedSyncState({"presetLibrary"});
        InvalidateResourceUsageIndex();

        nlohmann::json reply;
        reply["type"] = "presetSaved";
        reply["preset"] = SerializePresetForUi(newPreset);
        reply["sceneId"] = GetResolvedActiveSceneId();
        SendMessageToUI(reply.dump());
    }
    catch (const std::exception& e)
    {
        ReportErrorToUI("Failed to save preset", e.what());
    }
}

void PluginController::HandleDeletePresetRequest(const nlohmann::json& payload)
{
    const std::string presetId = payload.value("presetId", "");

    if (presetId.empty())
    {
        return;
    }

    if (!UserPresetExists(presetId))
    {
        ReportErrorToUI("Preset not found", presetId);
        return;
    }

    if (!DeleteUserPreset(presetId))
    {
        ReportErrorToUI("Failed to delete preset", presetId);
        return;
    }

    InvalidateResourceUsageIndex();
    TouchSharedSyncState({"presetLibrary"});
}

void PluginController::HandleGetPresetByIdRequest(const nlohmann::json& payload)
{
    const std::string presetId = payload.value("presetId", "");
    const std::string requestId = payload.value("requestId", "");

    if (presetId.empty())
    {
        return;
    }

    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : presetId;

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
    {
        nlohmann::json msg;
        msg["type"] = "error";
        msg["message"] = "Preset unavailable";
        msg["detail"] = "Factory preset archive loading is disabled in Advanced settings";

        if (!requestId.empty())
        {
            msg["requestId"] = requestId;
        }

        msg["presetId"] = presetId;
        SendMessageToUI(msg.dump());
        return;
    }

    const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");

    std::optional<Preset> presetOpt = LoadUserPreset(resolvedPresetId);

    if (!presetOpt && std::filesystem::exists(factoryPath))
    {
        presetOpt = PresetStorage::LoadFromFile(factoryPath);
    }

    if (!presetOpt)
    {
        auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);

        if (archiveIt != mFactoryArchivePresets.end())
        {
            presetOpt = archiveIt->second;
        }
    }

    if (!presetOpt)
    {
        nlohmann::json msg;
        msg["type"] = "error";
        msg["message"] = "Preset not found";
        msg["detail"] = presetId;

        if (!requestId.empty())
        {
            msg["requestId"] = requestId;
        }

        msg["presetId"] = presetId;
        SendMessageToUI(msg.dump());
        return;
    }

    nlohmann::json msg;
    msg["type"] = "presetData";
    msg["preset"] = SerializePresetForUi(*presetOpt);

    if (!requestId.empty())
    {
        msg["requestId"] = requestId;
    }

    msg["requestedPresetId"] = presetId;
    SendMessageToUI(msg.dump());
}

// ── Signal path editing handlers ───────────────────────────────────
// These handlers manipulate the signal graph nodes and edges.
// They will be ported from GuitarFXPlugin.cpp as the next step.

void PluginController::HandleGetPresetFoldersRequest()
{
    const auto payload = LoadUiStorageJson("preset-folders.json", nlohmann::json::object());
    nlohmann::json folders = payload.value("folders", nlohmann::json::array());
    std::string activeFolderId = payload.value("activeFolderId", "__all__");

    if (!IsPresetArchiveSessionActive() && !IsFactoryPresetArchiveLoadingEnabled() && folders.is_array())
    {
        nlohmann::json filtered = nlohmann::json::array();

        for (const auto& folder : folders)
        {
            if (!folder.is_object())
            {
                continue;
            }

            const std::string folderId = folder.value("id", "");

            if (IsFactoryArchiveFolderId(folderId))
            {
                continue;
            }

            filtered.push_back(folder);
        }

        if (IsFactoryArchiveFolderId(activeFolderId))
        {
            activeFolderId = "__all__";
        }

        folders = std::move(filtered);
    }

    nlohmann::json msg;
    msg["type"] = "presetFolders";
    msg["folders"] = std::move(folders);
    msg["activeFolderId"] = activeFolderId;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetPresetFoldersRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["folders"] = payload.value("folders", nlohmann::json::array());
    toStore["activeFolderId"] = payload.value("activeFolderId", "__all__");
    SaveUiStorageJson("preset-folders.json", toStore);
}

void PluginController::HandleGetPresetFavoritesRequest()
{
    const auto payload = LoadUiStorageJson("preset-favorites.json", nlohmann::json::object());
    nlohmann::json msg;
    msg["type"] = "presetFavorites";
    msg["favorites"] = payload.value("favorites", nlohmann::json::array());
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetPresetFavoritesRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["favorites"] = payload.value("favorites", nlohmann::json::array());
    SaveUiStorageJson("preset-favorites.json", toStore);
}

void PluginController::HandleGetPresetRatingsRequest()
{
    const auto payload = LoadUiStorageJson("preset-ratings.json", nlohmann::json::object());
    nlohmann::json msg;
    msg["type"] = "presetRatings";
    msg["ratings"] = payload.value("ratings", nlohmann::json::object());
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetPresetRatingsRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["ratings"] = payload.value("ratings", nlohmann::json::object());
    SaveUiStorageJson("preset-ratings.json", toStore);
}

void PluginController::HandleGetSetlistsRequest()
{
    const auto payload = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    nlohmann::json msg;
    msg["type"] = "setlists";
    msg["setlists"] = payload.value("setlists", nlohmann::json::array());
    msg["activeSetlistId"] = payload.value("activeSetlistId", "");
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetSetlistsRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["setlists"] = payload.value("setlists", nlohmann::json::array());
    toStore["activeSetlistId"] = payload.value("activeSetlistId", "");
    toStore["bankSize"] = payload.value("bankSize", 8);
    toStore["cursorIndex"] = payload.value("cursorIndex", 0);
    mSetlistBankSize = payload.value("bankSize", 8);
    mSetlistCursorIndex = payload.value("cursorIndex", 0);
    SaveUiStorageJson("setlists.json", toStore);
}

// ════════════════════════════════════════════════════════════════════
// Applying a preset to the running engine
// ════════════════════════════════════════════════════════════════════

void PluginController::ApplyPreset(const Preset& preset)
{
    // === Phase 1: Normalize and prepare preset data — no DSP lock needed. ===
    // All work here modifies local copies only; the audio thread is unaffected.
    Preset normalizedPreset = preset;
    NormalizePresetScenes(normalizedPreset);
    std::string resolvedSceneId = mActiveSceneId;

    if (!SetPresetActiveScene(normalizedPreset, resolvedSceneId, &resolvedSceneId))
    {
        resolvedSceneId = GetDefaultPresetSceneId(normalizedPreset);
    }

    for (auto& node : normalizedPreset.graph.nodes)
    {
        RefreshWasmNodeDescriptor(node);
    }

    for (auto& node : normalizedPreset.graph.nodes)
    {
        if (!IsNamEffectType(node.type))
        {
            continue;
        }

        // Strip legacy NAM level params; replaced by single useCalibration toggle.
        node.params.erase("autoLevelInput");
        node.params.erase("autoLevelOutput");
        node.params.erase("useNamInputMetadata");
        node.params.erase("clampAutoGain");
        node.params.erase("useAutoLevel");
        ClearNamCalibrationParams(node);

        if (!node.params.contains("useCalibration"))
        {
            node.params["useCalibration"] = 1.0;
        }
    }

    // Hydrate blend node resources (model refs + blendMode) from the blend library.
    // This must run before the executor is built so that CreateProcessors -> LoadResources
    // can resolve NAM model paths for MultiModelNAMAmpEffect. Without it, blend nodes added
    // or replaced via the signal path UI have empty resources, leaving the effect in
    // passthrough mode (no models loaded -> input/output gain and blend selection have no effect).
    ApplyBlendDefinitions(normalizedPreset);

    TryRemapHostedPluginResources(normalizedPreset);
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    // Global settings (gate, transpose, EQ, doubler, limiter) must never come from presets.
    // They are per-instance state and come from app settings (standalone) or host state (plugin).
    // Preserve current global FX state when loading a preset—ignore any preset-level overrides.
    auto chainConfig = mPresetMixer.GetGlobalChainConfig();
    const double inputGainDb = chainConfig.inputGain;
    const double outputGainDb = chainConfig.outputGain;

    chainConfig.inputGain = inputGainDb;
    chainConfig.outputGain = outputGainDb;
    chainConfig.autoLevelInput = false;
    chainConfig.autoLevelOutput = false;

    if (mHost.IsStandalone())
    {
        constexpr auto kMonoModeKey = "inputChannel.monoMode";
        constexpr auto kInputChanKey = "inputChannel.mono";

        const auto monoIt = mAppSettings.find(kMonoModeKey);
        const auto chanIt = mAppSettings.find(kInputChanKey);

        chainConfig.monoMode = (monoIt != mAppSettings.end() && monoIt->is_boolean()) ? monoIt->get<bool>() : true;
        chainConfig.inputChannel =
            (chanIt != mAppSettings.end() && chanIt->is_number_integer()) ? std::clamp(chanIt->get<int>(), 0, 1) : 0;
    }

    normalizedPreset.global.inputTrim = inputGainDb;
    normalizedPreset.global.outputTrim = outputGainDb;
    normalizedPreset.global.autoLevelInput = false;
    normalizedPreset.global.autoLevelOutput = false;
    normalizedPreset.globalSignalChain = chainConfig;

    const std::string initialSlotId = normalizedPreset.id.empty() ? "p1" : normalizedPreset.id;
    const std::string newPresetJson = PresetStorage::SerializeToJson(normalizedPreset);

    // === Phase 2: Build the new executors off the DSP lock. ===
    // This is the expensive step: effect processors are created, resources loaded
    // (e.g. NAM model weights read from disk), and Prepare() called on each node.
    // The audio thread continues processing the current preset uninterrupted.
    mPresetMixer.PreparePresetSwap(normalizedPreset, initialSlotId, normalizedPreset.name);

    // Global chains are staged the same way. This is almost always a no-op: global FX are
    // per-instance state that never comes from a preset, so chainConfig normally matches the
    // running config exactly and no rebuild is staged at all.
    mPresetMixer.PrepareGlobalChainSwap(chainConfig);

    // === Phase 3: Atomic swap under the DSP lock (fast). ===
    // The lock is held only for lightweight state updates and the instance swap.
    // No allocations or I/O occur inside this block.
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        mActiveSceneId = resolvedSceneId;

        // Install the global chains staged above. Construction already happened off the
        // lock; this is a pointer-level swap plus the scalar input/output settings.
        mPresetMixer.CommitGlobalChainSwap();
        mPresetMixer.SetAutoLevelInput(false);
        mPresetMixer.SetAutoLevelOutput(false);

        mActivePreset = normalizedPreset;
        mActivePresetJson = newPresetJson;

        // Use the real preset ID so the UI can map the mixer tab to the presetCache entry.
        // Fall back to "p1" only for presets without an id (should not happen in practice).
        mMixerPresetJsonCache.clear();
        mPresetMixer.CommitPresetSwap(); // Fast: swap mPendingInstance into mInstances + schedule fade-in
        mMixerPresetJsonCache[initialSlotId] = mActivePresetJson;
        AttachRuntimeConfigCallbacks(initialSlotId, normalizedPreset);

        // Register tuner callback
        mPresetMixer.SetTunerCallback([this](const MultiPresetMixer::TunerResult& result) {
            mTuner->PostReading({
                result.noteName,
                result.octave,
                result.frequency,
                result.centOffset,
                result.confidence,
                result.detected,
            });
        });

        // Apply global interface calibration level to all calibratable NAM
        // effect nodes (overrides preset params; calibrationInputLevel is not
        // stored in preset data). We inject even when a model is not currently
        // resolved so the value is already present once the model loads.
        const bool hasCalibrationValue = std::isfinite(mNamInterfaceCalibrationLevelDbu);
        const double clearValue = std::numeric_limits<double>::quiet_NaN();

        for (const auto& node : normalizedPreset.graph.nodes)
        {
            if (!IsNamCalibratableEffectType(node.type))
            {
                continue;
            }

            const double calibrationToInject = hasCalibrationValue ? mNamInterfaceCalibrationLevelDbu : clearValue;
            mPresetMixer.SetNodeParam(initialSlotId, node.id, "calibrationInputLevel", calibrationToInject);
        }
    }

    mHost.NotifyStateChanged();
}

std::optional<Preset> PluginController::LoadUserPreset(const std::string& presetId) const
{
    // A preset-archive session is a sandbox: its presets live in a throwaway
    // directory and completely shadow the real library for the duration, so they
    // are read from files and never enter the store.
    if (IsPresetArchiveSessionActive())
    {
        return PresetStorage::LoadFromFile(GetEffectiveUserPresetDirectory() / (presetId + ".json"));
    }

    return PresetStorage::LoadFromStore(Store(), presetId);
}

std::vector<Preset> PluginController::LoadAllUserPresets() const
{
    if (IsPresetArchiveSessionActive())
    {
        return PresetStorage::LoadAllFromDirectory(GetEffectiveUserPresetDirectory());
    }

    return PresetStorage::LoadAllFromStore(Store());
}

bool PluginController::SaveUserPreset(const Preset& preset)
{
    if (IsPresetArchiveSessionActive())
    {
        const auto sessionDir = GetEffectiveUserPresetDirectory();
        [[maybe_unused]] const auto ensuredSessionDir = mFileSystem.EnsureDirectory(sessionDir);
        return PresetStorage::SaveToFile(preset, sessionDir / (preset.id + ".json"));
    }

    return PresetStorage::SaveToStore(Store(), preset);
}

bool PluginController::DeleteUserPreset(const std::string& presetId)
{
    if (IsPresetArchiveSessionActive())
    {
        std::error_code ec;
        return std::filesystem::remove(GetEffectiveUserPresetDirectory() / (presetId + ".json"), ec) && !ec;
    }

    return PresetStorage::RemoveFromStore(Store(), presetId);
}

bool PluginController::UserPresetExists(const std::string& presetId) const
{
    if (IsPresetArchiveSessionActive())
    {
        std::error_code ec;
        return std::filesystem::exists(GetEffectiveUserPresetDirectory() / (presetId + ".json"), ec);
    }

    return PresetStorage::ExistsInStore(Store(), presetId);
}

std::optional<Preset> PluginController::LoadPresetById(const std::string& presetId) const
{
    if (presetId.empty())
    {
        return std::nullopt;
    }

    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : presetId;

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
    {
        return std::nullopt;
    }

    std::optional<Preset> presetOpt = LoadUserPreset(resolvedPresetId);

    if (IsPresetArchiveSessionActive())
    {
        return presetOpt;
    }

    if (!presetOpt)
    {
        const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");
        presetOpt = PresetStorage::LoadFromFile(factoryPath);
    }

    if (!presetOpt)
    {
        const auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);

        if (archiveIt != mFactoryArchivePresets.end())
        {
            presetOpt = archiveIt->second;
        }
    }

    return presetOpt;
}

std::optional<std::string> PluginController::FindPresetIdByTitle(const std::string& presetTitle) const
{
    const std::string normalizedTitle = NormalizePresetTitle(presetTitle);

    if (normalizedTitle.empty())
    {
        return std::nullopt;
    }

    const bool factoryArchiveLoadingEnabled = IsFactoryPresetArchiveLoadingEnabled();

    auto matchesTitle = [&](const Preset& preset) -> bool {
        return NormalizePresetTitle(preset.name) == normalizedTitle;
    };

    for (const auto& preset : LoadAllUserPresets())
    {
        if (!factoryArchiveLoadingEnabled && mTrackedFactoryArchivePresetIds.contains(preset.id))
        {
            continue;
        }

        if (matchesTitle(preset))
        {
            return preset.id;
        }
    }

    if (IsPresetArchiveSessionActive())
    {
        return std::nullopt;
    }

    const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot);

    if (std::filesystem::exists(factoryPath))
    {
        for (const auto& entry : std::filesystem::directory_iterator(factoryPath))
        {
            if (entry.path().extension() != ".json")
            {
                continue;
            }

            const auto presetOpt = PresetStorage::LoadFromFile(entry.path());

            if (presetOpt && matchesTitle(*presetOpt))
            {
                return presetOpt->id;
            }
        }
    }

    if (factoryArchiveLoadingEnabled)
    {
        for (const auto& [presetId, preset] : mFactoryArchivePresets)
        {
            if (matchesTitle(preset))
            {
                return presetId;
            }
        }
    }

    return std::nullopt;
}

bool PluginController::TryLoadConfiguredDefaultPreset()
{
    const std::string configuredTitle = guitarfx::config::kDefaultStartupPresetTitle;

    if (NormalizePresetTitle(configuredTitle).empty())
    {
        return false;
    }

    const auto presetId = FindPresetIdByTitle(configuredTitle);

    if (!presetId)
    {
        std::cerr << "[Plugin] Configured default preset title not found: " << configuredTitle << std::endl;
        return false;
    }

    const auto presetOpt = LoadPresetById(*presetId);

    if (!presetOpt)
    {
        std::cerr << "[Plugin] Configured default preset could not be loaded: " << configuredTitle << std::endl;
        return false;
    }

    mActivePresetId = *presetId;
    mActivePreset = *presetOpt;
    mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
    ApplyPreset(*presetOpt);
    std::cout << "[Plugin] Loaded configured default preset: " << presetOpt->name << std::endl;
    return true;
}
} // namespace guitarfx
