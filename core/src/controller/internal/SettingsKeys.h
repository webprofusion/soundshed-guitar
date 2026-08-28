#pragma once

/**
 * SettingsKeys.h — Canonical names for entries in the app settings document.
 *
 * A settings key is a contract with a file already on the user's disk: the
 * writer and every reader must spell it identically, and a renamed key
 * silently loses the stored value. Declaring them once here is what keeps a
 * key from being typed twice and diverging.
 *
 * Keys owned by a single feature live with that feature instead — see
 * MetronomeSupport.h, RiffSupport.h and PresetArchiveSupport.h.
 */

namespace guitarfx::controller_detail
{

// ── Jam panel ───────────────────────────────────────────────────────
inline constexpr const char* kJamYouTubeApiKeySettingKey = "jam.youtubeApiKey";
inline constexpr const char* kBundledJamYouTubeApiKey = "";

// ── DSP levels ──────────────────────────────────────────────────────
inline constexpr const char* kNominalOperatingLevelSettingKey = "audio.dsp.nominalOperatingLevelDbfs";
inline constexpr const char* kOutputProtectionCeilingSettingKey = "audio.dsp.outputProtectionCeilingDbfs";
inline constexpr const char* kGlobalFxSettingsKey = "globalFx.settings";

// ── NAM quality ─────────────────────────────────────────────────────
// Each setting has a matching per-node config key: the app-wide setting is
// pushed down onto every NAM node, and a node may then carry its own value.
inline constexpr const char* kNamSlimmableSizeSettingKey = "audio.nam.slimmableSize";
inline constexpr const char* kNamSlimmableNodeConfigKey = "slimmableSize";
inline constexpr const char* kNamOversamplingSettingKey = "audio.nam.oversampling";
inline constexpr const char* kNamOversamplingNodeConfigKey = "oversampling";
inline constexpr const char* kNamAntiAliasPhaseSettingKey = "audio.nam.antiAliasPhase";
inline constexpr const char* kNamAntiAliasPhaseNodeConfigKey = "antiAliasPhase";

// ── Input calibration ───────────────────────────────────────────────
inline constexpr const char* kNamInterfaceCalibrationLevelDbuSettingKey = "audio.nam.interfaceCalibrationLevelDbu";
inline constexpr double kNamInterfaceCalibrationLevelDbuDefault = 12.0;
inline constexpr double kNamInterfaceCalibrationLevelDbuMin = 0.0;
inline constexpr double kNamInterfaceCalibrationLevelDbuMax = 24.0;
inline constexpr const char* kNamAutoInputCalibrationSettingKey = "audio.nam.autoInputCalibration";
inline constexpr const char* kUserInputCalibrationProfilesSettingKey = "audio.userInputCalibration.profiles";
inline constexpr const char* kUserInputCalibrationActiveProfileIdSettingKey =
    "audio.userInputCalibration.activeProfileId";

// Superseded by the profile keys above; still read when migrating a settings file.
inline constexpr const char* kLegacyInterfaceCalibrationEnabledSettingKey = "audio.interfaceCalibration.enabled";
inline constexpr const char* kLegacyInterfaceCalibrationReferenceDbuSettingKey =
    "audio.interfaceCalibration.referenceDbu";

} // namespace guitarfx::controller_detail
