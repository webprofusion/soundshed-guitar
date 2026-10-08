/**
 * App-setting keys, and the bounds every numeric setting is clamped to.
 *
 * The bounds live next to the keys rather than next to the control that shows
 * them, because refreshing the panel and binding the control both have to agree
 * on them.
 */

export const API_KEY_SETTING = "tone3000.apiKey";

export const TONE3000_USE_SOUNDSHED_API_SETTING = "tone3000.useSoundshedToneSearchApi";

export const USER_INPUT_CALIBRATION_PROFILES_SETTING = "audio.userInputCalibration.profiles";

export const USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING = "audio.userInputCalibration.activeProfileId";

export const USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS = -12.0;

export const USER_INPUT_CALIBRATION_NONE_VALUE = "__none__";

export const FACTORY_ARCHIVE_LOADING_SETTING = "factoryPresets.archiveLoadingEnabled";

/** Preset collection exports carry the user's effect presets: how factory ones are authored. */
export const FACTORY_ARCHIVE_EXPORT_EFFECT_PRESETS_SETTING = "factoryPresets.exportIncludesEffectPresets";

export const DSP_NOMINAL_LEVEL_SETTING = "audio.dsp.nominalOperatingLevelDbfs";

export const DSP_PROTECTION_CEILING_SETTING = "audio.dsp.outputProtectionCeilingDbfs";

export const DSP_OUTPUT_LIMITER_SETTING = "audio.dsp.outputLimiterEnabled";

export const PRESET_SWITCH_TAIL_SETTING = "audio.presetSwitch.tailBars";

export const NAM_SLIMMABLE_SIZE_SETTING = "audio.nam.slimmableSize";

export const NAM_OVERSAMPLING_SETTING = "audio.nam.oversampling";

export const NAM_ANTI_ALIAS_PHASE_SETTING = "audio.nam.antiAliasPhase";

export const NAM_INTERFACE_CALIBRATION_LEVEL_SETTING = "audio.nam.interfaceCalibrationLevelDbu";

export const NAM_AUTO_INPUT_CALIBRATION_SETTING = "audio.nam.autoInputCalibration";

export const DSP_NOMINAL_LEVEL_DEFAULT = -18.0;

export const DSP_NOMINAL_LEVEL_MIN = -30.0;

export const DSP_NOMINAL_LEVEL_MAX = -6.0;

export const DSP_PROTECTION_CEILING_DEFAULT = -1.0;

export const DSP_PROTECTION_CEILING_MIN = -6.0;

export const DSP_PROTECTION_CEILING_MAX = 0.0;

// Bars of delay/reverb tail carried over a preset switch; 0 is off. The option values are
// the bar count itself, and the engine clamps to the same range.
export const PRESET_SWITCH_TAIL_DEFAULT = 2;

export const PRESET_SWITCH_TAIL_MIN = 0;

export const PRESET_SWITCH_TAIL_MAX = 4;

export const NAM_SLIMMABLE_SIZE_DEFAULT = 1.0;

export const NAM_SLIMMABLE_SIZE_MIN = 0.0;

export const NAM_SLIMMABLE_SIZE_MAX = 1.0;

// Index into the Off/2x/4x/8x/16x/32x and Minimum Phase/Linear Short/Linear Long
// option lists; the DSP side clamps to the same ranges.
export const NAM_OVERSAMPLING_DEFAULT = 0;

export const NAM_OVERSAMPLING_MIN = 0;

export const NAM_OVERSAMPLING_MAX = 5;

export const NAM_ANTI_ALIAS_PHASE_DEFAULT = 0;

export const NAM_ANTI_ALIAS_PHASE_MIN = 0;

export const NAM_ANTI_ALIAS_PHASE_MAX = 2;

export const NAM_INTERFACE_CALIBRATION_LEVEL_DEFAULT = 12.0;

export const NAM_INTERFACE_CALIBRATION_LEVEL_MIN = 0.0;

export const NAM_INTERFACE_CALIBRATION_LEVEL_MAX = 24.0;

export const ZOOM_MIN = 0.5;

export const ZOOM_MAX = 2.0;

export const ZOOM_DEFAULT = 1.0;

export const ZOOM_LEVEL_OPTIONS = [
  0.75,
  0.9,
  1.0,
  1.1,
  1.25,
  1.5,
  1.75,
  2.0,
];

export const UPDATE_CHECK_ENABLED_SETTING = "app.updateCheckEnabled";

/**
 * Shows the Channels (Follow/Mono) choice in effect panel headers. Off by default, as an advanced
 * control; it only hides the choice, and a node's channel mode still applies while it is off.
 */
export const CHANNEL_MODE_CONTROL_SETTING = "audio.signalPath.channelModeControl";
