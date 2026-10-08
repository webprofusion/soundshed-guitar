/**
 * The engine settings: level targets, NAM model handling, preset-switch tails,
 * and the toggles that sit alongside them.
 */

import { postMessage } from "../bridge.js";
import { updateAppSetting } from "../appSettingsStore.js";
import { showNotification } from "../notifications.js";
import { clonePreset, uiState } from "../state.js";
import { cachePreset } from "../presetLibraryStore.js";
import { triggerUpdateCheck } from "../updateCheck.js";
import type { Preset } from "../types.js";
import { requestNodeParamsRefresh } from "../signalPath/render.js";
import { channelModeControlToggle, dspNominalLevelInput,dspOutputLimiterToggle, dspProtectionCeilingInput, factoryArchiveExportEffectPresetsToggle, factoryArchiveLoadingToggle, namAntiAliasPhaseSelect, namAutoInputCalibrationToggle, namInterfaceCalibrationLevelInput, namOversamplingSelect, namSlimmableSizeInput, presetSwitchTailSelect, updateCheckToggle } from "./dom.js";
import { CHANNEL_MODE_CONTROL_SETTING, DSP_NOMINAL_LEVEL_DEFAULT,DSP_NOMINAL_LEVEL_MAX, DSP_NOMINAL_LEVEL_MIN, DSP_NOMINAL_LEVEL_SETTING, DSP_OUTPUT_LIMITER_SETTING, DSP_PROTECTION_CEILING_DEFAULT, DSP_PROTECTION_CEILING_MAX, DSP_PROTECTION_CEILING_MIN, DSP_PROTECTION_CEILING_SETTING, FACTORY_ARCHIVE_EXPORT_EFFECT_PRESETS_SETTING, FACTORY_ARCHIVE_LOADING_SETTING, NAM_ANTI_ALIAS_PHASE_DEFAULT, NAM_ANTI_ALIAS_PHASE_MAX, NAM_ANTI_ALIAS_PHASE_MIN, NAM_ANTI_ALIAS_PHASE_SETTING, NAM_AUTO_INPUT_CALIBRATION_SETTING, NAM_INTERFACE_CALIBRATION_LEVEL_DEFAULT, NAM_INTERFACE_CALIBRATION_LEVEL_MAX, NAM_INTERFACE_CALIBRATION_LEVEL_MIN, NAM_INTERFACE_CALIBRATION_LEVEL_SETTING, NAM_OVERSAMPLING_DEFAULT, NAM_OVERSAMPLING_MAX, NAM_OVERSAMPLING_MIN, NAM_OVERSAMPLING_SETTING, NAM_SLIMMABLE_SIZE_DEFAULT, NAM_SLIMMABLE_SIZE_MAX, NAM_SLIMMABLE_SIZE_MIN, NAM_SLIMMABLE_SIZE_SETTING, PRESET_SWITCH_TAIL_DEFAULT, PRESET_SWITCH_TAIL_MAX, PRESET_SWITCH_TAIL_MIN, PRESET_SWITCH_TAIL_SETTING, UPDATE_CHECK_ENABLED_SETTING } from "./keys.js";
import { bindImmediateNumericSetting, bindIndexSelectSetting } from "./values.js";

let dspLevelTargetsInitialized = false;

export function initUpdateCheckToggle(): void {
  // Bound to a local so the null check narrows inside the listener; a
  // narrowing on an imported binding does not survive into a closure.
  const toggle = updateCheckToggle;
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.addEventListener("change", () => {
    const enabled = Boolean(toggle.checked);
    updateAppSetting(UPDATE_CHECK_ENABLED_SETTING, enabled);
    if (enabled) {
      triggerUpdateCheck();
    }
  });
}

export function initChannelModeControlToggle(): void {
  const toggle = channelModeControlToggle;
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.addEventListener("change", () => {
    updateAppSetting(CHANNEL_MODE_CONTROL_SETTING, Boolean(toggle.checked));
    // Redraw the open effect panel so its header gains or loses the Channels choice.
    requestNodeParamsRefresh();
  });
}

export function initFactoryArchiveLoadingToggle(): void {
  const toggle = factoryArchiveLoadingToggle;
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.addEventListener("change", () => {
    const enabled = Boolean(toggle.checked);
    updateAppSetting(FACTORY_ARCHIVE_LOADING_SETTING, enabled);
  });
}

export function initFactoryArchiveExportEffectPresetsToggle(): void {
  const toggle = factoryArchiveExportEffectPresetsToggle;
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.addEventListener("change", () => {
    updateAppSetting(FACTORY_ARCHIVE_EXPORT_EFFECT_PRESETS_SETTING, Boolean(toggle.checked));
  });
}

export function initPresetSwitchControls(): void {
  bindIndexSelectSetting(
    presetSwitchTailSelect,
    PRESET_SWITCH_TAIL_SETTING,
    PRESET_SWITCH_TAIL_MIN,
    PRESET_SWITCH_TAIL_MAX,
    PRESET_SWITCH_TAIL_DEFAULT,
  );
}

export function initDspLevelTargetControls(): void {
  if (dspLevelTargetsInitialized) {
    return;
  }
  dspLevelTargetsInitialized = true;

  bindImmediateNumericSetting(
    dspNominalLevelInput,
    DSP_NOMINAL_LEVEL_SETTING,
    DSP_NOMINAL_LEVEL_MIN,
    DSP_NOMINAL_LEVEL_MAX,
    DSP_NOMINAL_LEVEL_DEFAULT,
  );

  bindImmediateNumericSetting(
    dspProtectionCeilingInput,
    DSP_PROTECTION_CEILING_SETTING,
    DSP_PROTECTION_CEILING_MIN,
    DSP_PROTECTION_CEILING_MAX,
    DSP_PROTECTION_CEILING_DEFAULT,
  );

  bindImmediateNumericSetting(
    namSlimmableSizeInput,
    NAM_SLIMMABLE_SIZE_SETTING,
    NAM_SLIMMABLE_SIZE_MIN,
    NAM_SLIMMABLE_SIZE_MAX,
    NAM_SLIMMABLE_SIZE_DEFAULT,
  );

  bindIndexSelectSetting(
    namOversamplingSelect,
    NAM_OVERSAMPLING_SETTING,
    NAM_OVERSAMPLING_MIN,
    NAM_OVERSAMPLING_MAX,
    NAM_OVERSAMPLING_DEFAULT,
  );

  bindIndexSelectSetting(
    namAntiAliasPhaseSelect,
    NAM_ANTI_ALIAS_PHASE_SETTING,
    NAM_ANTI_ALIAS_PHASE_MIN,
    NAM_ANTI_ALIAS_PHASE_MAX,
    NAM_ANTI_ALIAS_PHASE_DEFAULT,
  );

  bindImmediateNumericSetting(
    namInterfaceCalibrationLevelInput,
    NAM_INTERFACE_CALIBRATION_LEVEL_SETTING,
    NAM_INTERFACE_CALIBRATION_LEVEL_MIN,
    NAM_INTERFACE_CALIBRATION_LEVEL_MAX,
    NAM_INTERFACE_CALIBRATION_LEVEL_DEFAULT,
  );

  const autoCalibrationToggle = namAutoInputCalibrationToggle;
  if (autoCalibrationToggle && autoCalibrationToggle.dataset.bound !== "true") {
    autoCalibrationToggle.dataset.bound = "true";
    autoCalibrationToggle.addEventListener("change", () => {
      const enabled = Boolean(autoCalibrationToggle.checked);
      updateAppSetting(NAM_AUTO_INPUT_CALIBRATION_SETTING, enabled);
    });
  }

  const limiterToggle = dspOutputLimiterToggle;
  if (limiterToggle && limiterToggle.dataset.bound !== "true") {
    limiterToggle.dataset.bound = "true";
    limiterToggle.addEventListener("change", () => {
      const enabled = Boolean(limiterToggle.checked);
      updateAppSetting(DSP_OUTPUT_LIMITER_SETTING, enabled);
    });
  }

}

export function initDiagnosticsToggle(): void {

  const applyBtn = document.getElementById("apply-designed-peak-btn") as HTMLButtonElement | null;
  if (applyBtn && applyBtn.dataset.bound !== "true") {
    applyBtn.dataset.bound = "true";
    applyBtn.addEventListener("click", () => {
      const peakDbfs = (applyBtn as HTMLButtonElement & { _peakDbfs?: number })._peakDbfs;
      if (peakDbfs == null || !isFinite(peakDbfs)) {
        showNotification("No peak value available yet — play audio first");
        return;
      }
      const activeId = uiState.activePresetId;
      if (!activeId) {
        showNotification("No active preset");
        return;
      }
      const preset = clonePreset(
        uiState.presetCache.get(activeId) ??
        uiState.presets.find((p) => p.id === activeId) ??
        ({} as Preset)
      );
      preset.designedPeakInputDbfs = Math.round(peakDbfs * 10) / 10;
      delete (preset as Record<string, unknown>).globalSignalChain;
      cachePreset(preset, activeId);
      postMessage({
        type: "savePreset",
        presetId: preset.id,
        name: preset.name ?? "",
        category: preset.category ?? "",
        description: preset.description ?? "",
        includeGlobalSignalChain: false,
        preset,
      });
      showNotification(`Designed peak input set to ${preset.designedPeakInputDbfs.toFixed(1)} dBFS`);
    });
  }
}
