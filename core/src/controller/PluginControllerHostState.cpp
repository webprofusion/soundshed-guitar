/**
 * PluginControllerHostState.cpp - The state blob a DAW saves with its project.
 *
 * SerializeState/DeserializeState are the plugin's whole conversation with the
 * host about persistence: the focused preset with its hosted-plugin state
 * folded in, every mixer slot's preset data, the settings that belong to the
 * project rather than the machine, and this instance's editor window size.
 *
 * The asymmetry is deliberate. The standalone app restores from app settings
 * and preset files, so DeserializeState takes only the hosted-plugin path
 * there; and a hosted restore runs inside a scope that blocks writes to the
 * shared settings store, so reopening an old project cannot republish its
 * snapshot over settings the user has changed since.
 *
 * SerializeState answers on any thread, but builds only on the message thread,
 * which owns nearly everything the blob is made of; HostStateRelay hands it
 * requests from elsewhere. DeserializeState, which replaces most of it, goes the
 * same way, and so does a host's program change (ApplySetlistPresetByIndex).
 */

#include "PluginController.h"

#include "controller/HostStateRelay.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/ResourceSources.h"
#include "controller/internal/SettingsKeys.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#include "presets/PresetStorage.h"

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
// Sizes an editor window can plausibly have been left at. Anything outside this is a
// layout artefact rather than a size a user dragged to.
bool IsPlausibleEditorWindowSize(int width, int height)
{
    constexpr int kMinRememberedEditorDimension = 200;
    constexpr int kMaxRememberedEditorDimension = 16384;

    return width >= kMinRememberedEditorDimension && height >= kMinRememberedEditorDimension &&
           width <= kMaxRememberedEditorDimension && height <= kMaxRememberedEditorDimension;
}
} // namespace

void PluginController::SetEditorWindowSize(int width, int height)
{
    // Staged, not committed. A range check alone is not enough to tell a real resize from
    // a layout artefact: hosts resize the editor on the way to closing its window, and a
    // degenerate rect comes back through the editor's constrainer as its *minimum* size,
    // which looks entirely plausible but is not a size anyone chose. Committing that is
    // how a remembered size turns into a tiny window on the next open.
    //
    // So a reported size only becomes the remembered one once it is still in effect an
    // idle tick later (see OnIdle). The editor drives the idle callback and stops driving
    // it when it is destroyed, so a size that exists only while the window is being torn
    // down is never committed.
    if (!IsPlausibleEditorWindowSize(width, height))
    {
        return;
    }

    if (mEditorWindowSize.width == width && mEditorWindowSize.height == height)
    {
        mPendingEditorWindowSize.reset();
        return;
    }

    // Repeated reports of the same pending size must not keep restarting the settle, or a
    // host that re-reports its size every frame would never let anything commit.
    if (mPendingEditorWindowSize.has_value() && mPendingEditorWindowSize->width == width &&
        mPendingEditorWindowSize->height == height)
    {
        return;
    }

    mPendingEditorWindowSize = EditorWindowSize{width, height};
    mEditorWindowSizeChangedSinceIdle = true;
}

void PluginController::OnEditorClosed()
{
    // Nobody is left to confirm or cancel a preview once its window is gone, and someone who
    // closes the editor on a model they are hearing means to keep it.
    if (mPreviewState.active && KeepActiveResourcePreview())
    {
        NotifyHostStateChanged();
    }
}

void PluginController::RepairRestoredResourceRefs(Preset& preset) const
{
    for (const auto& repaired : RepairResourceRefs(preset, mResourceLibrary))
    {
        AppendSessionLog("Restore: " + repaired.type + " \"" + repaired.fromId + "\" is not in the library; using \"" +
                         repaired.toId + "\", the same model by its saved source");
    }

    for (const auto& missing : FindMissingResourceRefs(preset, mResourceLibrary))
    {
        const auto name = missing.source.find("name");
        AppendSessionLog("Restore: " + missing.type + " \"" + missing.id + "\" is missing" +
                         (name != missing.source.end() ? " (" + name->second + ")" : std::string{}) + " in preset " +
                         preset.name);
    }
}

void PluginController::NotifyHostStateChanged()
{
    // Even during a restore: the blob that answers off-thread requests has changed either way.
    mHostStateRelay->MarkStale();

    // A restore replays the state the host has just handed over, and everything it applies
    // on the way (the preset, the NAM quality tier) would otherwise report itself as a change.
    // Hosts take that at its word: the project reads as modified the moment it opens, and a
    // host that snapshots plugin state for undo can record the restore, which may be its own
    // undo, as a new step and discard its redo.
    if (mRestoringHostState)
    {
        return;
    }

    if (mHost.IsMessageThread() && (mHostCallsHeld || mHostCallsHeldForHostCall))
    {
        mHeldStateChange = true;
        return;
    }

    mHost.NotifyStateChanged();
}

bool PluginController::RunHostChangeOnMessageThread(std::function<void()> change, bool isRestore,
                                                    std::optional<std::string> pendingState)
{
    return mHostStateRelay->ApplyOnMessageThread(
        [this, change = std::move(change)] {
            mHostCallsHeld = true;
            change();
        },
        [this, isRestore](bool callerWaited) {
            ReleaseHeldHostCalls();

            // A caller that was still waiting tells the host itself when it returns.
            if (isRestore && !callerWaited)
            {
                mHost.NotifyDeferredStateRestored();
            }
        },
        std::move(pendingState));
}

void PluginController::ReleaseHeldHostCalls()
{
    mHostCallsHeld = false;

    // The host's own call into the plugin has not returned yet; the task posted for that makes them.
    if (mHostCallsHeldForHostCall)
    {
        return;
    }

    if (std::exchange(mHeldLatencyReport, false))
    {
        UpdateHostLatency();
    }

    if (std::exchange(mHeldStateChange, false))
    {
        NotifyHostStateChanged();
    }
}

std::string PluginController::SerializeState() const
{
    // Not every host asks for state on the message thread, and nearly everything the blob is
    // built from belongs to it: the working copy and the slot cache change there without a
    // lock, and a hosted plugin found under the DSP lock stays alive after it is released
    // only there. So a call from any other thread is handed to the message thread, and
    // answered with the last blob built there if it cannot start in time (see HostStateRelay).
    if (!mHost.IsMessageThread())
    {
        return mHostStateRelay->BuildOnMessageThread([this] { return SerializeState(); });
    }

    // A restore the host made from another thread before asking for this is part of the
    // answer, even when the message thread has not got to it yet.
    mHostStateRelay->ApplyQueued();

    auto state = BuildHostState(HostedPluginStateSource::Live);
    mHostStateRelay->Remember(state);
    return state;
}

void PluginController::RememberHostStateFromWorkingCopy() const
{
    try
    {
        mHostStateRelay->Remember(BuildHostState(HostedPluginStateSource::WorkingCopy));
    }
    catch (const std::exception&)
    {
        // Leave the previous blob in place. A save on the message thread builds its own
        // either way.
    }
}

std::string PluginController::BuildHostState(HostedPluginStateSource source) const
{
    const bool captureLive = source == HostedPluginStateSource::Live;
    nlohmann::json state = nlohmann::json::object();
    state["version"] = 1;

    if (mActivePreset)
    {
        Preset presetWithRuntimeState = *mActivePreset;

        if (captureLive)
        {
            CaptureRuntimePluginStates(presetWithRuntimeState, mActivePresetId);
        }

        // A Tone3000 model still being previewed plays from a temp file that is not the
        // project's to keep: the slot is saved with what it held before the preview. Closing
        // the editor keeps the preview in the library (KeepActiveResourcePreview), and the
        // project then saves that.
        if (mPreviewState.active && mPreviewState.originalResourceRef && !mPreviewState.tempFilePath.empty())
        {
            const auto putBack = [this](SignalGraph& graph) {
                for (auto& node : graph.nodes)
                {
                    for (auto& ref : node.resources)
                    {
                        if (!ref.filePath.empty() && ref.filePath == mPreviewState.tempFilePath)
                        {
                            ref = *mPreviewState.originalResourceRef;
                        }
                    }
                }
            };

            putBack(presetWithRuntimeState.graph);

            for (auto& scene : presetWithRuntimeState.scenes)
            {
                putBack(scene.graph);
            }
        }

        StampResourceSources(presetWithRuntimeState, mResourceLibrary);
        state["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(presetWithRuntimeState));
    }

    state["presetId"] = mActivePresetId;
    state["activeSceneId"] = GetResolvedActiveSceneId();

    // Account settings stay out of the project: they are the user's, not the project's,
    // and a project file gets shared.
    nlohmann::json appSettings = nlohmann::json::object();

    if (mAppSettings.is_object())
    {
        for (const auto& [key, value] : mAppSettings.items())
        {
            if (!IsAccountSettingKey(key))
            {
                appSettings[key] = value;
            }
        }
    }

    state["appSettings"] = std::move(appSettings);
    state["uiSettings"] = mUiSettings;
    state["uiViewState"] = mUiViewState;
    state["globalSignalChain"] = mPresetMixer.GetGlobalChainConfig();

    // The editor window size the user last left this instance at. Only emitted once the
    // editor has actually reported a size, so a project saved with the editor never
    // opened does not pin a default over whatever the wrapper would pick.
    if (mEditorWindowSize.IsValid())
    {
        state["editorWindow"] = {{"width", mEditorWindowSize.width}, {"height", mEditorWindowSize.height}};
    }

    // NAM quality is per instance, so it rides in host state rather than app.json.
    // Emitted as its own block (not just inside appSettings) so it stays legible and
    // is restored explicitly, after the appSettings merge, on the way back in.
    state["namQuality"] = {{"slimmableSize", mNamQuality.slimmableSize},
                           {"oversampling", mNamQuality.oversamplingIndex},
                           {"antiAliasPhase", mNamQuality.antiAliasPhaseIndex}};

    // No "parameters" block: global FX live in globalSignalChain, which is the single
    // source of truth. States written before that consolidation still carry the key and
    // are simply ignored on the way back in.

    nlohmann::json mixer = nlohmann::json::object();
    mixer["masterGain"] = mPresetMixer.GetMasterGain();
    mixer["mixGainDb"] = mPresetMixer.GetMixGainDb();
    mixer["limiterEnabled"] = mPresetMixer.IsLimiterEnabled();
    mixer["monoOutputUnity"] = mPresetMixer.IsMonoOutputUnity();

    nlohmann::json activePresetIds = nlohmann::json::array();
    nlohmann::json presetConfigs = nlohmann::json::object();
    // Full preset data for the non-focused slots. Restoring them by id alone (the original
    // behaviour, still the fallback) reloads them from the machine's preset library, which
    // throws away every project-local edit — hosted plugin state included — and fails
    // outright when the project is opened on a machine that does not have the preset.
    nlohmann::json presetData = nlohmann::json::object();

    for (const auto& cfg : SnapshotActivePresetConfigs())
    {
        const auto& id = cfg.id;
        activePresetIds.push_back(id);
        presetConfigs[id] = {
            {"name", cfg.name}, {"mix", cfg.mix}, {"pan", cfg.pan}, {"mute", cfg.mute}, {"solo", cfg.solo}};

        // The focused slot already rides in state["preset"] with its runtime state folded in.
        if (mActivePreset && id == mActivePresetId)
        {
            continue;
        }

        const auto cachedIt = mMixerPresetJsonCache.find(id);

        if (cachedIt == mMixerPresetJsonCache.end())
        {
            continue;
        }

        if (auto slotPreset = PresetStorage::DeserializeFromJson(cachedIt->second))
        {
            if (captureLive)
            {
                CaptureMixerSlotHostedPluginState(*slotPreset, id);
            }

            StampResourceSources(*slotPreset, mResourceLibrary);

            try
            {
                presetData[id] = nlohmann::json::parse(PresetStorage::SerializeToJson(*slotPreset));
            }
            catch (const std::exception&)
            {
                // A slot that will not round-trip is skipped rather than poisoning the whole
                // state blob; it falls back to restore-by-id on the way back in.
            }
        }
    }

    mixer["activePresetIds"] = std::move(activePresetIds);
    mixer["presets"] = std::move(presetConfigs);
    mixer["presetData"] = std::move(presetData);
    state["mixer"] = std::move(mixer);

    state["automation"] = mAutomationSlots.SaveToJson();
    state["automationValues"] = mAutomationSlots.SaveValuesToJson();

    return state.dump();
}

bool PluginController::DeserializeState(const std::string& json)
{
    // The mirror of SerializeState. A restore replaces what the message thread owns (the working
    // copy, settings, automation, the mixer's slots) and loads presets and plugins, which belong
    // to it too, so a call from any other thread is handed to it. JUCE's AU, AAX and LV2
    // wrappers restore on whatever thread the host used.
    //
    // Unlike a save there is nothing to answer with instead, so a restore the message thread
    // does not start in time is left queued rather than dropped; it may be busy on the host's
    // behalf, or blocked on this very thread. Until it lands, a save is answered with the state
    // being restored, which is what the host will expect to get back.
    if (!mHost.IsMessageThread())
    {
        std::optional<std::string> pendingState;

        if (!mHost.IsStandalone() && nlohmann::json::parse(json, nullptr, false).is_object())
        {
            pendingState = json;
        }

        return RunHostChangeOnMessageThread([this, json] { RestoreHostState(json); }, true, std::move(pendingState));
    }

    // The host is still inside its setState here, and hearing from the plugin there can trip it
    // up. A latency change becomes VST3's restartComponent, made synchronously on this thread,
    // and a host may answer it by deactivating and re-preparing the plugin halfway through the
    // restore. FL Studio reports any fault inside the call as the plugin crashing while loading
    // its settings. So what the restore, and anything queued ahead of it, has to tell the host
    // waits until the call has returned.
    HoldHostCallsUntilHostReturns();

    // One the host made earlier from another thread goes first, or it would land on top of this.
    mHostStateRelay->ApplyQueued();
    RestoreHostState(json);
    return true;
}

void PluginController::HoldHostCallsUntilHostReturns()
{
    // Already held by an earlier restore whose task has not run yet: that task makes them for both.
    if (std::exchange(mHostCallsHeldForHostCall, true))
    {
        return;
    }

    mHost.RunOnMainThread([this, alive = std::weak_ptr<const bool>(mAliveForPostedTasks)] {
        if (alive.expired())
        {
            return;
        }

        mHostCallsHeldForHostCall = false;

        // A relayed change holding them still has its own release to come, which makes them.
        if (!mHostCallsHeld)
        {
            ReleaseHeldHostCalls();
        }
    });
}

void PluginController::RestoreHostState(const std::string& json)
{
    if (mHost.IsStandalone())
    {
        // Standalone startup restores from app settings + preset files
        // (LoadLastSessionState), not from host-serialized transient state — a stale
        // snapshot must never republish machine-wide settings or revive an old graph.
        //
        // Hosted plugin state is the one exception, and it is handled separately below.
        RestoreStandaloneHostedPluginState(json);
        return;
    }

    // Everything restored below belongs to the DAW project, not to the machine-wide
    // store. Without this, reopening an old project republishes its whole settings
    // snapshot over settings the user has changed since — the merge lands in
    // mAppSettings while mAppSettingsBaseline still describes the store, so the next
    // save of anything at all diffs the project's values as this instance's edits.
    //
    // The scope blocks writes for the duration and rebases the baseline on the way
    // out, including on the exception path, so a partial restore cannot leave project
    // values queued for publication either. It also stops anything applied during the
    // restore from reporting itself to the host as a change (see NotifyHostStateChanged).
    struct HostStateRestoreScope
    {
        PluginController& controller;

        explicit HostStateRestoreScope(PluginController& c) : controller(c)
        {
            controller.mRestoringHostState = true;
        }

        ~HostStateRestoreScope()
        {
            controller.mRestoringHostState = false;
            controller.AdoptAppSettingsAsBaseline();
        }
    };

    const HostStateRestoreScope restoreScope{*this};

    try
    {
        auto state = nlohmann::json::parse(json);
        const nlohmann::json* incomingSettings = nullptr;

        if (state.contains("appSettings") && state["appSettings"].is_object())
        {
            incomingSettings = &state["appSettings"];
        }
        else if (state.contains("settings") && state["settings"].is_object())
        {
            incomingSettings = &state["settings"];
        }

        if (incomingSettings != nullptr)
        {
            if (!mAppSettings.is_object())
            {
                mAppSettings = nlohmann::json::object();
            }

            for (auto it = incomingSettings->begin(); it != incomingSettings->end(); ++it)
            {
                // Projects saved before account settings were left out still carry them.
                // Taking one would put this instance on a key, sign-in or Tone3000 mode
                // from whenever the project was saved, instead of the user's current one.
                // The Custom Effects switches decide whether WASM code runs at all, so a
                // project someone sends cannot turn them on either.
                const bool customEffectsSwitch =
                    it.key() == kCustomEffectsFeatureSettingKey || it.key() == kLegacyAdvancedOptionsSettingKey;

                if (!IsAccountSettingKey(it.key()) && !customEffectsSwitch)
                {
                    mAppSettings[it.key()] = it.value();
                }
            }

            // Merging is not applying. These values have to reach the DSP, or the
            // instance runs on whatever Initialize() read from the shared store while
            // the UI reports the project's values back — the two silently disagree.
            // The return value is discarded on purpose: nothing restored from host state
            // is this instance's to publish (see HostStateRestoreScope above).
            (void)ApplySettingsToRuntime(SettingsApplyMode::kApplyAll);
        }

        // Applied after the appSettings merge so the instance's own saved tier wins over
        // whatever app.json seeded at Initialize(). Older states without this block fall
        // back to the appSettings values handled above.
        if (state.contains("namQuality") && state["namQuality"].is_object())
        {
            const auto& quality = state["namQuality"];
            const auto readNumber = [&quality](const char* field, double fallback) {
                const auto it = quality.find(field);
                return (it != quality.end() && it->is_number()) ? it->get<double>() : fallback;
            };

            mAppSettings[kNamSlimmableSizeSettingKey] =
                SanitizeNamSlimmableSize(readNumber("slimmableSize", kNamSlimmableSizeDefault));
            mAppSettings[kNamOversamplingSettingKey] =
                SanitizeNamOversamplingIndex(readNumber("oversampling", kNamOversamplingIndexDefault));
            mAppSettings[kNamAntiAliasPhaseSettingKey] =
                SanitizeNamAntiAliasPhaseIndex(readNumber("antiAliasPhase", kNamAntiAliasPhaseIndexDefault));
            ApplyNamQualitySettings();
        }

        if (state.contains("uiSettings") && state["uiSettings"].is_object())
        {
            mUiSettings = state["uiSettings"];
        }
        else
        {
            ApplyUiSettingsFromAppSettings();
        }

        if (state.contains("uiViewState") && state["uiViewState"].is_object())
        {
            mUiViewState = state["uiViewState"];
        }

        if (state.contains("editorWindow") && state["editorWindow"].is_object())
        {
            const auto& editorWindow = state["editorWindow"];
            const auto readDimension = [&editorWindow](const char* field) {
                const auto it = editorWindow.find(field);
                return (it != editorWindow.end() && it->is_number()) ? it->get<int>() : 0;
            };

            // Committed straight away rather than staged: this is the project's own value,
            // not something an editor is currently reporting, and the editor has to be
            // able to read it back the moment it is created. Same sanity check as a live
            // resize, so a hand-edited or corrupt project cannot pin an absurd size.
            const auto width = readDimension("width");
            const auto height = readDimension("height");

            if (IsPlausibleEditorWindowSize(width, height))
            {
                mEditorWindowSize = EditorWindowSize{width, height};
                mPendingEditorWindowSize.reset();
            }
        }

        if (state.contains("globalSignalChain") && state["globalSignalChain"].is_object())
        {
            // Build off the lock, install under it — see PrepareGlobalChainSwap().
            mPresetMixer.PrepareGlobalChainSwap(state["globalSignalChain"].get<GlobalSignalChainConfig>());
            std::lock_guard<std::mutex> dspLock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
        }

        if (state.contains("preset"))
        {
            auto presetOpt = PresetStorage::DeserializeFromJson(state["preset"].dump());

            if (presetOpt)
            {
                RepairRestoredResourceRefs(*presetOpt);
                mActivePresetId = state.value("presetId", presetOpt->id);
                mActiveSceneId = state.contains("activeSceneId") && state["activeSceneId"].is_string()
                                     ? state["activeSceneId"].get<std::string>()
                                     : std::string{};
                mActivePreset = *presetOpt;
                mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
                ApplyPreset(*presetOpt);
            }
        }

        // A "parameters" array from an older state is deliberately not replayed. It was a
        // flat mirror of the global FX values, and only ever tracked a few of them — the
        // rest read back as 0, which switched those effects off over the chain restored
        // just above. globalSignalChain carries all of it.

        // The mono-output makeup is the project's, never the setting's: the setting is for
        // instances added from now on. A project saved before it existed has no value, and was
        // made without the makeup, so it stays off.
        {
            const auto mixerIt = state.find("mixer");
            const bool unity = mixerIt != state.end() && mixerIt->is_object() && mixerIt->contains("monoOutputUnity") &&
                               (*mixerIt)["monoOutputUnity"].is_boolean() && (*mixerIt)["monoOutputUnity"].get<bool>();
            mPresetMixer.SetMonoOutputUnity(unity);
            mMonoOutputUnityOwned = true;
        }

        if (state.contains("mixer") && state["mixer"].is_object())
        {
            const auto& mixer = state["mixer"];

            if (mixer.contains("masterGain") && mixer["masterGain"].is_number())
            {
                mPresetMixer.SetMasterGain(mixer["masterGain"].get<double>());
            }

            if (mixer.contains("limiterEnabled") && mixer["limiterEnabled"].is_boolean())
            {
                mPresetMixer.SetLimiterEnabled(mixer["limiterEnabled"].get<bool>());
            }

            if (mixer.contains("mixGainDb") && mixer["mixGainDb"].is_number())
            {
                mPresetMixer.SetMixGainDb(mixer["mixGainDb"].get<double>());
            }

            // Reset active presets before restoring mixer state
            {
                std::lock_guard<std::mutex> lock(mDSPMutex);

                for (const auto& id : mPresetMixer.GetActivePresetIds())
                {
                    mPresetMixer.RemoveActivePreset(id);
                }
            }

            std::vector<std::string> activeIds;

            if (mixer.contains("activePresetIds") && mixer["activePresetIds"].is_array())
            {
                for (const auto& entry : mixer["activePresetIds"])
                {
                    if (entry.is_string())
                    {
                        activeIds.push_back(entry.get<std::string>());
                    }
                }
            }

            const auto presets = mixer.contains("presets") ? mixer["presets"] : nlohmann::json::object();
            // Written since full slot data was added to host state; absent in older projects,
            // which fall through to the restore-by-id path below exactly as before.
            const auto presetData = mixer.contains("presetData") && mixer["presetData"].is_object()
                                        ? mixer["presetData"]
                                        : nlohmann::json::object();

            if (activeIds.empty() && presets.is_object())
            {
                for (const auto& [id, _] : presets.items())
                {
                    activeIds.push_back(id);
                }
            }

            for (const auto& id : activeIds)
            {
                const auto presetEntry =
                    presets.is_object() && presets.contains(id) ? presets[id] : nlohmann::json::object();
                const std::string name = presetEntry.value("name", id);

                bool added = false;

                // Through the controller's AddActivePreset, which builds each slot off the DSP
                // lock and installs it under it, and attaches the runtime callbacks.
                if (mActivePreset && (id == "p1" || id == mActivePresetId))
                {
                    added = AddActivePreset(*mActivePreset, id, name);
                }

                // The project's own copy of this slot wins over the machine's preset library:
                // it is the one carrying the project's edits and its hosted plugin state.
                if (!added && presetData.contains(id))
                {
                    if (auto slotPreset = PresetStorage::DeserializeFromJson(presetData[id].dump()))
                    {
                        RepairRestoredResourceRefs(*slotPreset);
                        added = AddActivePreset(*slotPreset, id, name);

                        if (added)
                        {
                            AppendSessionLog("Mixer slot restored from host state id=" + id +
                                             ", state=" + SummarizeHostedPluginState(*slotPreset));
                        }
                    }
                }

                if (!added)
                {
                    added = AddActivePresetById(id);
                }

                if (!added && mActivePreset)
                {
                    added = AddActivePreset(*mActivePreset, id, name);
                }

                if (presetEntry.is_object())
                {
                    if (presetEntry.contains("mix") && presetEntry["mix"].is_number())
                    {
                        SetActivePresetMix(id, presetEntry["mix"].get<double>());
                    }

                    if (presetEntry.contains("pan") && presetEntry["pan"].is_number())
                    {
                        SetActivePresetPan(id, presetEntry["pan"].get<double>());
                    }

                    if (presetEntry.contains("mute") && presetEntry["mute"].is_boolean())
                    {
                        SetActivePresetMute(id, presetEntry["mute"].get<bool>());
                    }

                    if (presetEntry.contains("solo") && presetEntry["solo"].is_boolean())
                    {
                        SetActivePresetSolo(id, presetEntry["solo"].get<bool>());
                    }
                }
            }
        }

        const auto valuesIt = state.find("automationValues");
        const nlohmann::json* values = valuesIt != state.end() && valuesIt->is_object() ? &*valuesIt : nullptr;

        if (state.contains("automation") && state["automation"].is_object())
        {
            // The values go into the rebuilt slots, which can include custom slots they belong
            // to, before those go live: nothing sees them at 0 in between.
            ReplaceAutomationSlots(state["automation"], values);
        }
        else if (values != nullptr)
        {
            mAutomationSlots.LoadValuesFromJson(*values);
        }

        // States written before values were saved have no key, and leave values alone.
    }
    catch (const std::exception&)
    {
        // Ignore malformed state
    }

    mPendingStateBroadcast = true;

    // The project just restored is the answer for a host that asks from another thread before
    // anything changes, whether or not an editor is open to drive the idle refresh. Taken from
    // the working copy: hosted plugins may not have applied their restored state yet.
    RememberHostStateFromWorkingCopy();
}
} // namespace guitarfx
