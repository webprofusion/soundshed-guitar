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
 */

#include "PluginController.h"

#include "controller/internal/HostedPluginSupport.h"
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

std::string PluginController::SerializeState() const
{
    nlohmann::json state = nlohmann::json::object();
    state["version"] = 1;

    if (mActivePreset)
    {
        Preset presetWithRuntimeState = *mActivePreset;
        CaptureRuntimePluginStates(presetWithRuntimeState, mActivePresetId);
        state["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(presetWithRuntimeState));
    }

    state["presetId"] = mActivePresetId;
    state["activeSceneId"] = GetResolvedActiveSceneId();
    state["appSettings"] = mAppSettings;
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

    nlohmann::json activePresetIds = nlohmann::json::array();
    nlohmann::json presetConfigs = nlohmann::json::object();
    // Full preset data for the non-focused slots. Restoring them by id alone (the original
    // behaviour, still the fallback) reloads them from the machine's preset library, which
    // throws away every project-local edit — hosted plugin state included — and fails
    // outright when the project is opened on a machine that does not have the preset.
    nlohmann::json presetData = nlohmann::json::object();

    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        activePresetIds.push_back(id);

        if (const auto cfg = mPresetMixer.GetPresetConfig(id))
        {
            presetConfigs[id] = {
                {"name", cfg->name}, {"mix", cfg->mix}, {"pan", cfg->pan}, {"mute", cfg->mute}, {"solo", cfg->solo}};
        }

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
            CaptureMixerSlotHostedPluginState(*slotPreset, id);
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

    return state.dump();
}

void PluginController::DeserializeState(const std::string& json)
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
    // values queued for publication either.
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
                mAppSettings[it.key()] = it.value();
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
            for (const auto& id : mPresetMixer.GetActivePresetIds())
            {
                mPresetMixer.RemoveActivePreset(id);
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

                if (mActivePreset && (id == "p1" || id == mActivePresetId))
                {
                    added = mPresetMixer.AddActivePreset(*mActivePreset, id, name);

                    if (added)
                    {
                        AttachRuntimeConfigCallbacks(id, *mActivePreset);
                        mMixerPresetJsonCache[id] = PresetStorage::SerializeToJson(*mActivePreset);
                    }
                }

                // The project's own copy of this slot wins over the machine's preset library:
                // it is the one carrying the project's edits and its hosted plugin state.
                if (!added && presetData.contains(id))
                {
                    if (auto slotPreset = PresetStorage::DeserializeFromJson(presetData[id].dump()))
                    {
                        added = mPresetMixer.AddActivePreset(*slotPreset, id, name);

                        if (added)
                        {
                            AttachRuntimeConfigCallbacks(id, *slotPreset);
                            mMixerPresetJsonCache[id] = PresetStorage::SerializeToJson(*slotPreset);
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
                    added = mPresetMixer.AddActivePreset(*mActivePreset, id, name);

                    if (added)
                    {
                        AttachRuntimeConfigCallbacks(id, *mActivePreset);
                        mMixerPresetJsonCache[id] = PresetStorage::SerializeToJson(*mActivePreset);
                    }
                }

                if (presetEntry.is_object())
                {
                    if (presetEntry.contains("mix") && presetEntry["mix"].is_number())
                    {
                        mPresetMixer.SetPresetMix(id, presetEntry["mix"].get<double>());
                    }

                    if (presetEntry.contains("pan") && presetEntry["pan"].is_number())
                    {
                        mPresetMixer.SetPresetPan(id, presetEntry["pan"].get<double>());
                    }

                    if (presetEntry.contains("mute") && presetEntry["mute"].is_boolean())
                    {
                        mPresetMixer.SetPresetMute(id, presetEntry["mute"].get<bool>());
                    }

                    if (presetEntry.contains("solo") && presetEntry["solo"].is_boolean())
                    {
                        mPresetMixer.SetPresetSolo(id, presetEntry["solo"].get<bool>());
                    }
                }
            }
        }

        if (state.contains("automation") && state["automation"].is_object())
        {
            mAutomationSlots.LoadFromJson(state["automation"]);
        }
    }
    catch (const std::exception&)
    {
        // Ignore malformed state
    }

    mPendingStateBroadcast = true;
}
} // namespace guitarfx
