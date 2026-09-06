/**
 * PluginControllerSettings.cpp - Loading, applying and persisting app settings.
 *
 * ApplySettingsToRuntime is the one definition of "apply the settings", shared
 * by startup, host-state restore and shared-settings reload. Keeping three
 * hand-maintained lists in step is what previously let restored values be
 * merged but never applied.
 */

#include "PluginController.h"

#include "controller/MetronomeService.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/MetronomeSupport.h"
#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "controller/internal/SettingsKeys.h"
#include "dsp/EffectGuids.h"
#include "dsp/LevelTargets.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#include "presets/PresetStorage.h"

#include <algorithm>
#include <cmath>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
bool PluginController::ApplyDspLevelTargetSettingsFromAppSettings()
{
    bool settingsChanged = false;

    const auto readNumericSetting = [this, &settingsChanged](const char* key, double defaultValue) -> double {
        const auto it = mAppSettings.find(key);

        if (it == mAppSettings.end())
        {
            return defaultValue;
        }

        if (it->is_number())
        {
            return it->get<double>();
        }

        if (!it->is_null())
        {
            settingsChanged = true;
        }

        return defaultValue;
    };

    const double nominalLevelDbfs = SanitizeNominalOperatingLevelDbfs(
        readNumericSetting(kNominalOperatingLevelSettingKey, kDefaultNominalOperatingLevelDbfs));
    const double protectionCeilingDbfs = SanitizeOutputProtectionCeilingDbfs(
        readNumericSetting(kOutputProtectionCeilingSettingKey, kDefaultOutputProtectionCeilingDbfs));

    SetNominalOperatingLevelDbfs(nominalLevelDbfs);
    SetOutputProtectionCeilingDbfs(protectionCeilingDbfs);

    const auto updateStoredSetting = [this, &settingsChanged](const char* key, double value) {
        const auto it = mAppSettings.find(key);

        if (it == mAppSettings.end() || !it->is_number() || it->get<double>() != value)
        {
            mAppSettings[key] = value;
            settingsChanged = true;
        }
    };

    updateStoredSetting(kNominalOperatingLevelSettingKey, nominalLevelDbfs);
    updateStoredSetting(kOutputProtectionCeilingSettingKey, protectionCeilingDbfs);

    // The limiter that aims at the ceiling just applied. Off unless the stored value says
    // otherwise, and a missing or malformed entry is written back the same way the numeric
    // targets above are.
    const auto limiterIt = mAppSettings.find(kOutputLimiterEnabledSettingKey);
    const bool limiterEnabled = limiterIt != mAppSettings.end() && limiterIt->is_boolean() && limiterIt->get<bool>();

    mPresetMixer.SetLimiterEnabled(limiterEnabled);

    if (limiterIt == mAppSettings.end() || !limiterIt->is_boolean())
    {
        mAppSettings[kOutputLimiterEnabledSettingKey] = limiterEnabled;
        settingsChanged = true;
    }

    return settingsChanged;
}

void PluginController::ApplyInputModeSettingsFromAppSettings()
{
    // Only applies in standalone mode; in plugin mode the DAW owns the input config.
    if (!mHost.IsStandalone())
    {
        return;
    }

    // Key names must match the UI constants in controls.ts:
    //   INPUT_CHANNEL_SETTING  = "inputChannel.mono"
    //   MONO_MODE_SETTING      = "inputChannel.monoMode"
    constexpr auto kMonoModeKey = "inputChannel.monoMode";
    constexpr auto kInputChanKey = "inputChannel.mono";

    const auto monoIt = mAppSettings.find(kMonoModeKey);
    const auto chanIt = mAppSettings.find(kInputChanKey);

    const bool storedMonoMode = (monoIt != mAppSettings.end() && monoIt->is_boolean())
                                    ? monoIt->get<bool>()
                                    : true; // Default to mono mode so the guitar comes through on startup

    const int storedChannel =
        (chanIt != mAppSettings.end() && chanIt->is_number_integer()) ? std::clamp(chanIt->get<int>(), 0, 1) : 0;

    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetMonoMode(storedMonoMode);
    mPresetMixer.SetInputChannel(storedChannel);
}

void PluginController::ApplyGlobalFxSettingsFromAppSettings()
{
    if (!mHost.IsStandalone())
    {
        return;
    }

    const auto it = mAppSettings.find(kGlobalFxSettingsKey);

    if (it == mAppSettings.end() || !it->is_object())
    {
        return;
    }

    try
    {
        auto config = it->get<GlobalSignalChainConfig>();
        config.autoLevelInput = false;
        config.autoLevelOutput = false;
        // The blob does not carry the limiter, and committing the swap re-applies every
        // scalar from the config — so carry the live value across rather than reading the
        // stored default back over the app setting applied just above.
        config.limiterEnabled = mPresetMixer.IsLimiterEnabled();

        // Build off the lock, install under it — rebuilding the global executors while the
        // audio thread is blocked on mDSPMutex is an audible dropout.
        mPresetMixer.PrepareGlobalChainSwap(config);

        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
        }
        UpdateHostLatency();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] Failed to apply global FX settings: " << e.what() << std::endl;
    }
}

void PluginController::PersistGlobalFxSettingsToAppSettings()
{
    mHost.NotifyStateChanged();

    if (!mHost.IsStandalone())
    {
        return;
    }

    mAppSettings[kGlobalFxSettingsKey] = SerializeGlobalFxSettings(mPresetMixer.GetGlobalChainConfig());
    SaveAppSettings();
}

bool PluginController::IsNamQualitySettingKey(const std::string& key)
{
    return key == kNamSlimmableSizeSettingKey || key == kNamOversamplingSettingKey ||
           key == kNamAntiAliasPhaseSettingKey;
}

bool PluginController::IsUiLayoutSettingKey(const std::string& key)
{
    // Zoom, window size and the signal-path split all live inside the uiSettings blob;
    // uiZoom/uiBounds are its legacy flattened aliases.
    return key == "uiSettings" || key == "uiZoom" || key == "uiBounds";
}

bool PluginController::IsInstanceOwnedSettingKey(const std::string& key) const
{
    // Hosted as a plugin, layout belongs to the editor window the DAW sized and placed,
    // so it rides in host state with the project rather than following the standalone
    // app around. The shared store still seeds a brand-new instance, exactly as it does
    // for NAM quality — after that this instance owns it.
    return (IsNamQualitySettingKey(key) || IsUiLayoutSettingKey(key)) && !mHost.IsStandalone();
}

bool PluginController::ApplyNamQualitySettings()
{
    // NAM quality (slimmable size, oversampling, anti-alias phase) is per plugin
    // instance, not process-wide: a DAW loads every instance into one process, so
    // shared state would force a single quality tier on the whole project. The values
    // are pushed down as node-type config defaults so nodes built later — including
    // nodes nested inside composites — inherit this instance's tier.
    //
    // In plugin mode these keys are instance-owned: app.json only seeds a brand-new
    // instance, and this never writes back to it. Standalone keeps app.json ownership.
    bool settingsChanged = false;

    const auto readSetting = [&](const char* settingKey, auto sanitize, auto fallback) {
        const auto it = mAppSettings.find(settingKey);
        const auto sanitized = (it != mAppSettings.end() && it->is_number()) ? sanitize(it->get<double>()) : fallback;

        if (it == mAppSettings.end() || !it->is_number() || it->get<double>() != sanitized)
        {
            mAppSettings[settingKey] = sanitized;
            settingsChanged = true;
        }

        return sanitized;
    };

    mNamQuality.slimmableSize = readSetting(
        kNamSlimmableSizeSettingKey, [](double raw) { return SanitizeNamSlimmableSize(raw); },
        kNamSlimmableSizeDefault);
    mNamQuality.oversamplingIndex = readSetting(
        kNamOversamplingSettingKey, [](double raw) { return SanitizeNamOversamplingIndex(raw); },
        kNamOversamplingIndexDefault);
    mNamQuality.antiAliasPhaseIndex = readSetting(
        kNamAntiAliasPhaseSettingKey, [](double raw) { return SanitizeNamAntiAliasPhaseIndex(raw); },
        kNamAntiAliasPhaseIndexDefault);

    PushNamQualityToDsp();

    // Oversampling and the AA filter both change the resampler's reported latency.
    UpdateHostLatency();

    // Only the standalone app owns these on disk. A plugin instance persists them in its
    // host state (SerializeState) instead; SaveAppSettings() filters the keys out there,
    // so reporting the change up is safe in both modes.
    if (!mHost.IsStandalone())
    {
        // Tell the host its saved state is stale so the project keeps this tier.
        mHost.NotifyStateChanged();
    }

    return settingsChanged;
}

PluginController::NamQualityConfig PluginController::ApplyOfflineRenderBoost(NamQualityConfig quality)
{
    // An offline bounce has no realtime deadline, so spend the CPU on quality: full
    // slimmable size, and at least 2x oversampling. Both are floors, not overrides — a
    // user already running higher keeps their choice.
    quality.slimmableSize = kNamSlimmableSizeMax;
    quality.oversamplingIndex = std::max(quality.oversamplingIndex, kOfflineMinimumOversamplingIndex);
    return quality;
}

PluginController::NamQualityConfig PluginController::EffectiveNamQuality() const
{
    return mOfflineRendering ? ApplyOfflineRenderBoost(mNamQuality) : mNamQuality;
}

void PluginController::SetOfflineRendering(bool offline)
{
    if (mOfflineRendering == offline)
    {
        return;
    }

    mOfflineRendering = offline;

    // Re-push at the new effective tier. The host calls this around a bounce, off the
    // audio thread, and follows it with prepareToPlay — but push now so a host that
    // does not re-prepare still renders at the right quality.
    PushNamQualityToDsp();
    UpdateHostLatency();
}

void PluginController::PushNamQualityToDsp()
{
    const NamQualityConfig effective = EffectiveNamQuality();
    const std::string slimmableValue = std::to_string(effective.slimmableSize);
    const std::string oversamplingValue = std::to_string(effective.oversamplingIndex);
    const std::string antiAliasPhaseValue = std::to_string(effective.antiAliasPhaseIndex);

    std::lock_guard<std::mutex> lock(mDSPMutex);

    for (const char* nodeType :
         {EffectGuids::kAmpNam, EffectGuids::kAmpNamOptimized, EffectGuids::kAmpNamBlend, EffectGuids::kFxNam})
    {
        mPresetMixer.SetNodeTypeConfigDefault(nodeType, kNamSlimmableNodeConfigKey, slimmableValue);
        mPresetMixer.SetNodeTypeConfigDefault(nodeType, kNamOversamplingNodeConfigKey, oversamplingValue);
        mPresetMixer.SetNodeTypeConfigDefault(nodeType, kNamAntiAliasPhaseNodeConfigKey, antiAliasPhaseValue);
    }
}

/// Re-assert this instance's NAM quality over whatever a shared-settings reload wrote
/// into mAppSettings. Plugin instances own these keys; the shared app.json does not.
void PluginController::RestoreInstanceOwnedSettings()
{
    if (mHost.IsStandalone())
    {
        return;
    }

    // The live values survive LoadAppSettings() — it only replaces mAppSettings — so this
    // reads them straight back out of the members rather than needing the caller to snapshot
    // anything. It is the counterpart to the Apply* helpers, which would otherwise pull the
    // shared store's values over this instance's.
    mAppSettings[kNamSlimmableSizeSettingKey] = mNamQuality.slimmableSize;
    mAppSettings[kNamOversamplingSettingKey] = mNamQuality.oversamplingIndex;
    mAppSettings[kNamAntiAliasPhaseSettingKey] = mNamQuality.antiAliasPhaseIndex;
    PushNamQualityToDsp();
    UpdateHostLatency();

    mAppSettings["uiSettings"] = mUiSettings;
}

bool PluginController::ApplySettingsToRuntime(SettingsApplyMode mode)
{
    // The single place that decides what "apply the settings" means. Startup, a host-state
    // restore and a shared-settings reload all go through here: when they each kept their
    // own hand-maintained list, they drifted, and a setting missing from one of them was
    // merged but never pushed to the DSP.
    //
    // The standalone-only helpers self-guard, so a hosted instance runs the same list and
    // simply no-ops through them. The one real axis is whether this instance's own values
    // are re-derived from the store or re-asserted over it.
    const bool preserveInstanceOwned = mode == SettingsApplyMode::kPreserveInstanceOwned && !mHost.IsStandalone();

    bool settingsChanged = false;

    mMetronome->ApplySettingsFromAppSettings();
    settingsChanged |= ApplyDspLevelTargetSettingsFromAppSettings();
    ApplyInputModeSettingsFromAppSettings();
    ApplyGlobalFxSettingsFromAppSettings();

    if (preserveInstanceOwned)
    {
        RestoreInstanceOwnedSettings();
    }
    else
    {
        settingsChanged |= ApplyNamQualitySettings();
    }

    ApplyNamInterfaceCalibrationFromAppSettings();
    settingsChanged |= ApplyUserInputCalibrationSettingsFromAppSettings();

    if (!preserveInstanceOwned)
    {
        ApplyUiSettingsFromAppSettings();
    }

    return settingsChanged;
}

void PluginController::ApplyNamInterfaceCalibrationFromAppSettings()
{
    // Respect the global auto-input-calibration toggle (default: enabled).
    bool autoCalibrationEnabled = true;
    const auto enableIt = mAppSettings.find(kNamAutoInputCalibrationSettingKey);

    if (enableIt != mAppSettings.end() && enableIt->is_boolean())
    {
        autoCalibrationEnabled = enableIt->get<bool>();
    }

    const auto it = mAppSettings.find(kNamInterfaceCalibrationLevelDbuSettingKey);
    double calLevel = std::numeric_limits<double>::quiet_NaN();

    if (autoCalibrationEnabled)
    {
        if (it != mAppSettings.end() && it->is_number())
        {
            const double raw = it->get<double>();

            if (raw >= kNamInterfaceCalibrationLevelDbuMin && raw <= kNamInterfaceCalibrationLevelDbuMax)
            {
                calLevel = raw;
            }
            else
            {
                calLevel = kNamInterfaceCalibrationLevelDbuDefault;
            }
        }
        else
        {
            // Setting absent — use the default, mirroring the UI's fallback behaviour.
            // This ensures calibration is active from the first run even before the user
            // has visited Settings and explicitly saved the value.
            calLevel = kNamInterfaceCalibrationLevelDbuDefault;
        }
    }

    mNamInterfaceCalibrationLevelDbu = calLevel;

    if (!mActivePresetId.empty() && mActivePreset)
    {
        const auto& graph = mActivePreset->graph;
        const bool hasCalibrationValue = std::isfinite(calLevel);
        const double clearValue = std::numeric_limits<double>::quiet_NaN();
        std::lock_guard<std::mutex> lock(mDSPMutex);

        for (const auto& node : graph.nodes)
        {
            if (!IsNamCalibratableEffectType(node.type))
            {
                continue;
            }

            const double calibrationToInject = hasCalibrationValue ? calLevel : clearValue;
            mPresetMixer.SetNodeParam(mActivePresetId, node.id, "calibrationInputLevel", calibrationToInject);
        }
    }
}

bool PluginController::ApplyUserInputCalibrationSettingsFromAppSettings()
{
    bool settingsChanged = false;

    if (mAppSettings.erase(kLegacyInterfaceCalibrationEnabledSettingKey) > 0)
    {
        settingsChanged = true;
    }

    if (mAppSettings.erase(kLegacyInterfaceCalibrationReferenceDbuSettingKey) > 0)
    {
        settingsChanged = true;
    }

    std::string activeProfileId;
    const auto activeIt = mAppSettings.find(kUserInputCalibrationActiveProfileIdSettingKey);

    if (activeIt != mAppSettings.end())
    {
        if (activeIt->is_string())
        {
            activeProfileId = activeIt->get<std::string>();
        }
        else if (!activeIt->is_null())
        {
            mAppSettings[kUserInputCalibrationActiveProfileIdSettingKey] = nullptr;
            settingsChanged = true;
        }
    }

    double gainDb = 0.0;
    bool foundActiveProfile = activeProfileId.empty();
    const auto profilesIt = mAppSettings.find(kUserInputCalibrationProfilesSettingKey);

    if (profilesIt != mAppSettings.end())
    {
        if (profilesIt->is_array())
        {
            for (const auto& profile : *profilesIt)
            {
                if (!profile.is_object())
                {
                    continue;
                }

                const auto idIt = profile.find("id");

                if (idIt == profile.end() || !idIt->is_string() || idIt->get<std::string>() != activeProfileId)
                {
                    continue;
                }

                foundActiveProfile = true;

                const auto gainIt = profile.find("gainDb");

                if (gainIt != profile.end() && gainIt->is_number())
                {
                    gainDb = gainIt->get<double>();
                }
                else
                {
                    const auto capturedIt = profile.find("capturedPeakDbfs");
                    const auto targetIt = profile.find("targetPeakDbfs");

                    if (capturedIt != profile.end() && capturedIt->is_number() && targetIt != profile.end() &&
                        targetIt->is_number())
                    {
                        gainDb = targetIt->get<double>() - capturedIt->get<double>();
                    }
                }

                break;
            }
        }
        else if (!profilesIt->is_null())
        {
            mAppSettings[kUserInputCalibrationProfilesSettingKey] = nlohmann::json::array();
            settingsChanged = true;
        }
    }

    if (!foundActiveProfile)
    {
        mAppSettings[kUserInputCalibrationActiveProfileIdSettingKey] = nullptr;
        gainDb = 0.0;
        settingsChanged = true;
    }

    if (mUserInputCalibrationTrainingActive)
    {
        gainDb = 0.0;
    }

    mPresetMixer.SetUserInputCalibrationGainDb(gainDb);

    return settingsChanged;
}

void PluginController::ApplyUiSettingsFromAppSettings()
{
    mUiSettings = nlohmann::json::object();

    const auto it = mAppSettings.find("uiSettings");

    if (it != mAppSettings.end() && it->is_object())
    {
        mUiSettings = *it;
        return;
    }

    // Fall back to the legacy flattened aliases for stores written before uiSettings existed.
    nlohmann::json legacy = nlohmann::json::object();

    if (const auto zoomIt = mAppSettings.find("uiZoom"); zoomIt != mAppSettings.end())
    {
        legacy["zoom"] = *zoomIt;
    }

    if (const auto boundsIt = mAppSettings.find("uiBounds"); boundsIt != mAppSettings.end())
    {
        legacy["bounds"] = *boundsIt;
    }

    if (!legacy.empty())
    {
        mUiSettings = std::move(legacy);
    }
}

void PluginController::SaveAppSettings() const
{
    if (!mAppSettings.is_object())
    {
        return;
    }

    // Restoring host state must not write to the shared store — see HostStateRestoreScope
    // in DeserializeState().
    //
    // No path reachable from a restore still persists: the Apply* helpers now only report
    // that sanitising changed something, and the handlers that do save are standalone-only.
    // This stays as the invariant rather than the mechanism, because that "no path still
    // persists" claim is an audit over four separate guards, and an audit going stale is
    // exactly how a stale project came to republish its settings in the first place.
    if (mRestoringHostState)
    {
        return;
    }

    // Write only what *this* instance changed, against the snapshot it last
    // loaded or saved.
    //
    // Rewriting every key (which is what a ReplaceAll here would do) puts back
    // the exact clobber the per-key schema exists to avoid: mAppSettings is one
    // instance's view of the settings, so pushing all of it would overwrite a
    // key another instance changed since we loaded, with our stale copy. The
    // 2s shared-sync poll would eventually pull the loser's value back, but the
    // user sees their setting revert in the meantime.
    //
    // Diffing against the baseline also tells deletion apart from absence: a key
    // that is gone from mAppSettings but present in the baseline was removed
    // here and must be removed from the store; a key in neither was never ours
    // and is left alone.
    //
    // Instance-owned keys are skipped in both directions. They live in mAppSettings
    // because the rest of the controller reads its settings from there, but this
    // instance has no authority over them in the shared store: suppressing the save
    // at the call site is not enough, because the very next save of an unrelated key
    // would diff them as changed and push them out anyway.
    if (!mAppSettingsBaseline.is_object())
    {
        mAppSettingsBaseline = nlohmann::json::object();
    }

    const nlohmann::json& baseline = mAppSettingsBaseline;

    std::vector<std::pair<std::string, std::string>> upserts;
    std::vector<std::string> removals;

    for (const auto& [key, value] : mAppSettings.items())
    {
        if (key.empty() || IsInstanceOwnedSettingKey(key))
        {
            continue;
        }

        const auto previous = baseline.find(key);

        if (previous == baseline.end() || *previous != value)
        {
            upserts.emplace_back(key, value.dump());
        }
    }

    for (const auto& [key, value] : baseline.items())
    {
        if (!key.empty() && !IsInstanceOwnedSettingKey(key) && !mAppSettings.contains(key))
        {
            removals.push_back(key);
        }
    }

    if (upserts.empty() && removals.empty())
    {
        return;
    }

    storage::JsonStore& store = Store();
    const bool wrote = store.Transact([&]() {
        for (const auto& [key, json] : upserts)
        {
            if (!store.PutRaw(storage::ItemType::kSetting, key, json))
            {
                return false;
            }
        }

        for (const auto& key : removals)
        {
            if (!store.Remove(storage::ItemType::kSetting, key))
            {
                return false;
            }
        }

        return true;
    });

    if (!wrote)
    {
        std::cerr << "[Plugin] SaveAppSettings failed" << std::endl;
        return;
    }

    AdoptAppSettingsAsBaseline();
    TouchSharedSyncState({"appSettings"});
}

void PluginController::AdoptAppSettingsAsBaseline() const
{
    // Declare the current live view to be "already published", so nothing in it is
    // pending a write. Instance-owned keys keep whatever the store last reported
    // instead, because this instance never writes them and must not start claiming
    // its own value is what the store holds.
    if (!mAppSettings.is_object())
    {
        return;
    }

    nlohmann::json next = nlohmann::json::object();

    for (const auto& [key, value] : mAppSettings.items())
    {
        if (!key.empty() && !IsInstanceOwnedSettingKey(key))
        {
            next[key] = value;
        }
    }

    if (mAppSettingsBaseline.is_object())
    {
        for (const auto& [key, value] : mAppSettingsBaseline.items())
        {
            if (!key.empty() && IsInstanceOwnedSettingKey(key))
            {
                next[key] = value;
            }
        }
    }

    mAppSettingsBaseline = std::move(next);
}

bool PluginController::CleanupLegacyAppSettingsOnLoad()
{
    if (!mAppSettings.is_object())
    {
        return false;
    }

    bool settingsChanged = false;
    const auto eraseKeyIfPresent = [&](const char* key) {
        if (mAppSettings.erase(key) > 0)
        {
            settingsChanged = true;
        }
    };

    // Legacy/dead keys no longer read by startup/runtime paths.
    eraseKeyIfPresent("appSettings");
    eraseKeyIfPresent("audioSettings");
    eraseKeyIfPresent("lastPresetJson");
    eraseKeyIfPresent("parameters");
    eraseKeyIfPresent("metronomeEnabled");
    eraseKeyIfPresent("performancePads.open");
    eraseKeyIfPresent("toneSharing.apiBase");
    eraseKeyIfPresent("ui.experimentalFeaturesEnabled");
    eraseKeyIfPresent("audio.processing.namMonoOnly");
    // Multi-threaded preset processing is unconditional now; the preference is retired.
    eraseKeyIfPresent("audio.processing.multiThreaded");
    // Signal diagnostics are always on, so the stored flag was only ever `true`.
    eraseKeyIfPresent("diagnostics.signalLevelsEnabled");
    // Mixer-wide auto-levelling is retired; these were only ever written as `false`.
    eraseKeyIfPresent("autoLevelInput");
    eraseKeyIfPresent("autoLevelOutput");
    eraseKeyIfPresent("app.lastUpdateCheck");

    // Legacy global chain app setting was superseded by globalFx.settings.
    if (mAppSettings.contains(kGlobalFxSettingsKey))
    {
        eraseKeyIfPresent("globalSignalChain");
    }

    // Prune legacy metronome aliases after canonical keys are present.
    if (mAppSettings.contains(kMetronomeBpmSettingKey))
    {
        eraseKeyIfPresent(kMetronomeLegacyBpmKey);
    }

    if (mAppSettings.contains(kMetronomeVolumeDbSettingKey))
    {
        eraseKeyIfPresent(kMetronomeLegacyVolumeDbKey);
    }

    if (mAppSettings.contains(kMetronomePanSettingKey))
    {
        eraseKeyIfPresent(kMetronomeLegacyPanKey);
    }

    if (mAppSettings.contains(kMetronomeClickTypeSettingKey))
    {
        eraseKeyIfPresent(kMetronomeLegacyClickTypeKey);
    }

    return settingsChanged;
}

void PluginController::LoadAppSettings()
{
    const auto applyBundledDefaults = [this]() {
        if (!mAppSettings.is_object())
        {
            mAppSettings = nlohmann::json::object();
        }

        if (std::strlen(kBundledJamYouTubeApiKey) > 0)
        {
            mAppSettings[kJamYouTubeApiKeySettingKey] = std::string{kBundledJamYouTubeApiKey};
        }
    };

    mAppSettings = nlohmann::json::object();

    for (const auto& item : Store().List(storage::ItemType::kSetting))
    {
        if (auto parsed = item.Parse())
        {
            mAppSettings[item.id] = std::move(*parsed);
        }
    }

    // The baseline is what the store held, before the bundled defaults are
    // layered on. A default that matches what is already stored must not be
    // rewritten, and a default for a key nobody has ever set should be written
    // once — both fall out of diffing against the store's own contents.
    mAppSettingsBaseline = mAppSettings;

    applyBundledDefaults();

    if (CleanupLegacyAppSettingsOnLoad())
    {
        SaveAppSettings();
    }
}

void PluginController::LoadLastSessionState()
{
    // Restore last-used preset from settings if available
    std::string lastPresetId;

    if (mAppSettings.contains("lastPresetId") && mAppSettings["lastPresetId"].is_string())
    {
        lastPresetId = mAppSettings["lastPresetId"].get<std::string>();
    }

    if (!lastPresetId.empty())
    {
        if (lastPresetId.rfind("preset-archive-session__", 0) == 0)
        {
            lastPresetId.clear();
        }
    }

    if (!lastPresetId.empty())
    {
        if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(lastPresetId))
        {
            std::cout << "[Plugin] Skipping last factory archive preset restore because archive loading is disabled"
                      << std::endl;
            mPendingStateBroadcast = true;
            std::cout << "[Plugin] Last session state restored" << std::endl;
            return;
        }

        std::cout << "[Plugin] Restoring last preset: " << lastPresetId << std::endl;
        try
        {
            const auto aliasIt = mFactoryArchivePresetAliases.find(lastPresetId);
            const std::string resolvedPresetId =
                aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : lastPresetId;

            // Try user presets first, then factory
            std::optional<Preset> presetOpt = LoadUserPreset(resolvedPresetId);

            if (!presetOpt)
            {
                auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");
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

            if (presetOpt)
            {
                mActivePresetId = resolvedPresetId;
                mActivePreset = *presetOpt;
                mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
                ApplyPreset(*presetOpt);
                std::cout << "[Plugin] Restored preset: " << presetOpt->name << std::endl;
            }
            else
            {
                std::cerr << "[Plugin] Last preset not found on disk: " << lastPresetId << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Plugin] Failed to restore last preset: " << e.what() << std::endl;
        }
    }

    if (lastPresetId.empty())
    {
        TryLoadConfiguredDefaultPreset();
    }

    mPendingStateBroadcast = true;
    std::cout << "[Plugin] Last session state restored" << std::endl;
}
} // namespace guitarfx
