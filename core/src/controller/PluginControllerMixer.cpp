/**
 * PluginControllerMixer.cpp - The multi-preset mixer: which presets are loaded
 * into slots, and their levels.
 *
 * A mixer slot owns a copy of its preset's graph, so changes made while a slot
 * is focused have to be written back into that copy rather than only into the
 * live DSP.
 */

#include "PluginController.h"

#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "controller/internal/SettingsKeys.h"
#include "presets/PresetStorage.h"

#include <algorithm>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
std::vector<std::string> PluginController::SnapshotActivePresetIds() const
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    return mPresetMixer.GetActivePresetIds();
}

std::vector<MultiPresetMixer::InstanceConfig> PluginController::SnapshotActivePresetConfigs() const
{
    std::vector<MultiPresetMixer::InstanceConfig> configs;
    std::lock_guard<std::mutex> lock(mDSPMutex);

    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        if (auto cfg = mPresetMixer.GetPresetConfig(id))
        {
            configs.push_back(std::move(*cfg));
        }
    }

    return configs;
}

void PluginController::ClearActivePresetMixerState()
{
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        for (const auto& presetId : mPresetMixer.GetActivePresetIds())
        {
            mPresetMixer.RemoveActivePreset(presetId);
        }
    }

    mMixerPresetJsonCache.clear();
}

bool PluginController::AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (mPresetMixer.GetPresetConfig(presetId))
        {
            return false;
        }
    }

    // A stored preset names each blend node's blend but not its models, and a slot built
    // without them plays the node dry.
    Preset slotPreset = preset;
    ApplyBlendDefinitions(slotPreset);

    // Built off the DSP lock and installed under it, as a preset switch is: building a slot
    // creates its processors and loads their models, IRs and plugins, and every slot would
    // be silent for as long as that took.
    mPresetMixer.PreparePresetSwap(slotPreset, presetId, name);
    bool added = false;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        added = mPresetMixer.CommitPresetAddition(presetId);

        if (added)
        {
            AttachRuntimeConfigCallbacks(presetId, slotPreset);
            InjectNamInterfaceCalibrationIntoSlot(presetId, slotPreset);
        }
    }

    if (added)
    {
        try
        {
            mMixerPresetJsonCache[presetId] = PresetStorage::SerializeToJson(preset);
        }
        catch (...)
        {
        }
        UpdateHostLatency();
    }

    return added;
}

bool PluginController::AddActivePresetById(const std::string& presetId)
{
    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : presetId;

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
    {
        ReportErrorToUI("Cannot add preset to mixer",
                        "Factory preset archive loading is disabled in Advanced settings");
        return false;
    }

    // If the active preset matches, use it directly
    if (mActivePreset && mActivePreset->id == resolvedPresetId)
    {
        return AddActivePreset(*mActivePreset, resolvedPresetId, mActivePreset->name);
    }

    // Try the user's presets in the store
    if (auto presetOpt = LoadUserPreset(resolvedPresetId))
    {
        return AddActivePreset(*presetOpt, resolvedPresetId, presetOpt->name);
    }

    // Try loading from factory presets directory
    auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");
    auto presetOpt = PresetStorage::LoadFromFile(factoryPath);

    if (!presetOpt)
    {
        auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);

        if (archiveIt != mFactoryArchivePresets.end())
        {
            presetOpt = archiveIt->second;
        }
    }

    if (presetOpt)
    {
        return AddActivePreset(*presetOpt, resolvedPresetId, presetOpt->name);
    }

    ReportErrorToUI("Cannot add preset to mixer", "Preset '" + presetId + "' not found");
    return false;
}

bool PluginController::ApplyActivePresetById(const std::string& presetId)
{
    if (presetId.empty())
    {
        return false;
    }

    auto presetOpt = LoadPresetById(presetId);

    if (!presetOpt && mActivePreset && (mActivePreset->id == presetId || mActivePresetId == presetId))
    {
        // Unsaved/session-only preset that is already loaded — re-apply what we have.
        presetOpt = *mActivePreset;
    }

    if (!presetOpt)
    {
        ReportErrorToUI("Cannot load preset", "Preset '" + presetId + "' not found");
        return false;
    }

    Preset preset = std::move(*presetOpt);
    NormalizePresetScenes(preset);

    if (!SetPresetActiveScene(preset, std::string{}, &mActiveSceneId))
    {
        mActiveSceneId = GetDefaultPresetSceneId(preset);
    }

    mActivePresetId = presetId;

    // Single-instance swap: ApplyPreset() crossfades the outgoing chain out and the new one
    // in, and sets mActivePreset/mActivePresetJson and the mixer slot cache itself.
    ApplyPreset(preset);

    mPendingPresetStateBroadcast = true;

    if (mActivePreset)
    {
        nlohmann::json loaded;
        loaded["type"] = "presetLoaded";
        loaded["preset"] = SerializePresetForUi(*mActivePreset);
        loaded["activePresetIds"] = SnapshotActivePresetIds();
        loaded["sceneId"] = GetResolvedActiveSceneId();
        SendMessageToUI(loaded.dump());
    }

    // Only the standalone app owns the machine-wide "last preset". A hosted instance
    // still reads it to seed a brand-new instance, but its own preset choice belongs to
    // the DAW project (host state), not to every other instance and the app.
    RememberActivePresetAsLast();

    return true;
}

void PluginController::RemoveActivePreset(const std::string& presetId)
{
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.RemoveActivePreset(presetId);
        mMixerPresetJsonCache.erase(presetId);
    }

    UpdateHostLatency();
}

void PluginController::FocusMixerPreset(const std::string& presetId)
{
    if (presetId.empty() || presetId == mActivePresetId)
    {
        return;
    }

    // Bank the outgoing slot's live plugin state into its cache entry before the editing
    // focus moves; once it moves, this slot's runtime notifications land elsewhere.
    CaptureLiveHostedPluginStateIntoActivePreset();

    // And the rest of its working copy, unsaved edits included: from here that entry is all the
    // rig has. Whatever an edit path left unmirrored (automation folds its parameters without
    // re-serialising, up to 30 times a second) is caught here, where the entry is next read.
    MirrorActivePresetJson();

    const auto it = mMixerPresetJsonCache.find(presetId);

    if (it == mMixerPresetJsonCache.end())
    {
        AppendSessionLog("FocusMixerPreset: no cached preset data for slot=" + presetId);
        return;
    }

    auto presetOpt = PresetStorage::DeserializeFromJson(it->second);

    if (!presetOpt)
    {
        AppendSessionLog("FocusMixerPreset: failed to deserialize cached preset for slot=" + presetId);
        return;
    }

    // Switch the editing/display target only. The DSP mixer instances keep running
    // untouched so audio is unaffected — this just makes graph edits and the
    // broadcast "preset" state target the mixer slot the user is currently viewing.
    NormalizePresetScenes(*presetOpt);
    mActiveSceneId = GetDefaultPresetSceneId(*presetOpt);
    mActivePreset = std::move(presetOpt);
    mActivePresetId = presetId;
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[presetId] = mActivePresetJson;

    mPendingPresetStateBroadcast = true;
}

bool PluginController::ReplaceActiveMixerPresetInPlace(const Preset& preset, const std::string& presetId,
                                                       const std::string& name)
{
    // As AddActivePreset: the blend nodes' models come from the blend library.
    Preset slotPreset = preset;
    ApplyBlendDefinitions(slotPreset);

    if (!ReplaceMixerSlotInstance(slotPreset, presetId, name))
    {
        return false;
    }

    try
    {
        mMixerPresetJsonCache[presetId] = PresetStorage::SerializeToJson(preset);
    }
    catch (...)
    {
    }
    UpdateHostLatency();
    return true;
}

bool PluginController::ReplaceMixerSlotInstance(const Preset& slotPreset, const std::string& presetId,
                                                const std::string& name)
{
    UpdatePresetSwapTailBudget();
    mPresetMixer.PreparePresetSwap(slotPreset, presetId, name);

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (!mPresetMixer.CommitPresetReplacement(presetId))
        {
            return false;
        }

        AttachRuntimeConfigCallbacks(presetId, slotPreset);
        InjectNamInterfaceCalibrationIntoSlot(presetId, slotPreset);
    }

    // The slot's hosted plugins move over from the chain it replaced.
    mPresetMixer.CompleteInstanceHandOvers();
    return true;
}

void PluginController::ApplyActivePresetInItsSlot()
{
    if (!mActivePreset)
    {
        return;
    }

    const auto activeMixerIds = SnapshotActivePresetIds();
    const bool sharesTheMix = activeMixerIds.size() > 1 &&
                              std::find(activeMixerIds.begin(), activeMixerIds.end(), mActivePresetId) != activeMixerIds.end();

    if (!sharesTheMix)
    {
        ApplyPreset(*mActivePreset);
        // As below. A scene switch reports only "presetLoaded", never a state broadcast, so the
        // host would otherwise hear of a pitch shifter's latency coming or going some time later.
        UpdateHostLatency();
        return;
    }

    // Readied as ApplyPreset() readies it, against the global chain already running, which a
    // rebuild of one slot leaves alone. That hydrates the blends too, so the slot is built from
    // this copy as it is, and one serialisation serves the working copy and the slot's entry: a
    // preset carrying hosted-plugin state can run to megabytes.
    Preset slotPreset = *mActivePreset;
    const std::string sceneId = PreparePresetForEngine(slotPreset, mActiveSceneId, mPresetMixer.GetGlobalChainConfig());
    std::string slotJson = PresetStorage::SerializeToJson(slotPreset);

    if (!ReplaceMixerSlotInstance(slotPreset, mActivePresetId, slotPreset.name))
    {
        // The slot left the mix since the snapshot.
        ApplyPreset(*mActivePreset);
        return;
    }

    mMixerPresetJsonCache[mActivePresetId] = slotJson;
    mActiveSceneId = sceneId;
    mActivePreset = std::move(slotPreset);
    mActivePresetJson = std::move(slotJson);
    UpdateHostLatency();
    NotifyHostStateChanged();
}

void PluginController::MirrorActivePresetJson()
{
    if (!mActivePreset)
    {
        return;
    }

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);

    // Only an entry the slot already has: the working copy can outlive its slot (a rig removed
    // from the mix while focused), and every entry here is read as a slot's.
    if (const auto it = mMixerPresetJsonCache.find(mActivePresetId); it != mMixerPresetJsonCache.end())
    {
        it->second = mActivePresetJson;
    }
}

// The slot lookups behind these walk instances the audio thread erases, and Process() reads
// what they write.
void PluginController::SetActivePresetMix(const std::string& presetId, double value)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetPresetMix(presetId, value);
}

void PluginController::SetActivePresetPan(const std::string& presetId, double pan)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetPresetPan(presetId, pan);
}

void PluginController::SetActivePresetMute(const std::string& presetId, bool mute)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetPresetMute(presetId, mute);
}

void PluginController::SetActivePresetSolo(const std::string& presetId, bool solo)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetPresetSolo(presetId, solo);
}

void PluginController::SetMasterGain(double value)
{
    mPresetMixer.SetMasterGain(value);
}

void PluginController::SetMixGainDb(double dB)
{
    mPresetMixer.SetMixGainDb(dB);
}

void PluginController::ApplyMultiCoreSettingFromAppSettings()
{
    const bool standalone = mHost.IsStandalone();
    const auto it = mAppSettings.find(standalone ? kMultiCoreStandaloneSettingKey : kMultiCorePluginSettingKey);
    const bool enabled = it != mAppSettings.end() && it->is_boolean()
                             ? it->get<bool>()
                             : (standalone ? kMultiCoreStandaloneDefault : kMultiCorePluginDefault);

    // No DSP lock: the flag is atomic, and stopping the helpers joins threads, which must never
    // happen under the lock the audio thread try_locks. A block already fanned out finishes on
    // the audio thread (RealtimeTaskPool::Stop).
    mPresetMixer.SetMultiThreadedProcessingEnabled(enabled);
}

void PluginController::ApplyMonoOutputUnityFromAppSettings()
{
    // A plugin instance takes the setting only while it is new. Once it has its own value, from
    // here or from its project, a later change to the setting or a reload of the shared store
    // leaves it alone: the setting is for instances added from now on, and a saved project keeps
    // the level it was made with.
    if (!mHost.IsStandalone() && mMonoOutputUnityOwned)
    {
        return;
    }

    const auto it = mAppSettings.find(kMonoOutputUnitySettingKey);
    const bool enabled = it != mAppSettings.end() && it->is_boolean() && it->get<bool>();

    // No DSP lock: the flag is atomic and read once per block.
    mPresetMixer.SetMonoOutputUnity(enabled);
    mMonoOutputUnityOwned = !mHost.IsStandalone();
}

} // namespace guitarfx
