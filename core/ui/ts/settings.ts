/**
 * Settings panel — the entry point and the full-panel refresh.
 *
 * Everything the panel can do lives in ./settings/, one module per section of
 * the UI. What stays here is the two passes that cross all of them: wiring the
 * panel up once, and re-reading every control from the current app settings.
 */

import { updateAppSetting } from "./appSettingsStore.js";
import { initDensitySelect, initThemeSelect, initZoomControls } from "./settings/appearance.js";
import { initAudioDeviceSettings, syncAudioDeviceSettingsAvailability } from "./settings/audioDevice.js";
import { apiKeyInput, channelModeControlToggle, clearButton, dspNominalLevelInput,dspOutputLimiterToggle, dspProtectionCeilingInput, factoryArchiveExportEffectPresetsToggle, factoryArchiveLoadingToggle, namAntiAliasPhaseSelect, namAutoInputCalibrationToggle, namInterfaceCalibrationLevelInput, namOversamplingSelect, namSlimmableSizeInput, presetSwitchTailSelect, saveButton, themeSelect, tone3000UseSoundshedApiToggle, updateCheckToggle } from "./settings/dom.js";
import { initChannelModeControlToggle, initDiagnosticsToggle, initDspLevelTargetControls,initFactoryArchiveExportEffectPresetsToggle, initFactoryArchiveLoadingToggle, initPresetSwitchControls, initUpdateCheckToggle } from "./settings/dspSettings.js";
import { initFeatureToggles, refreshFeatureToggleStates, syncFeatureVisibility } from "./settings/features.js";
import { initUserInputCalibrationControls, refreshUserInputCalibrationView } from "./settings/inputCalibration.js";
import { API_KEY_SETTING, CHANNEL_MODE_CONTROL_SETTING, DSP_NOMINAL_LEVEL_DEFAULT,DSP_NOMINAL_LEVEL_MAX, DSP_NOMINAL_LEVEL_MIN, DSP_NOMINAL_LEVEL_SETTING, DSP_OUTPUT_LIMITER_SETTING, DSP_PROTECTION_CEILING_DEFAULT, DSP_PROTECTION_CEILING_MAX, DSP_PROTECTION_CEILING_MIN, DSP_PROTECTION_CEILING_SETTING, FACTORY_ARCHIVE_EXPORT_EFFECT_PRESETS_SETTING, FACTORY_ARCHIVE_LOADING_SETTING, NAM_ANTI_ALIAS_PHASE_DEFAULT, NAM_ANTI_ALIAS_PHASE_MAX, NAM_ANTI_ALIAS_PHASE_MIN, NAM_ANTI_ALIAS_PHASE_SETTING, NAM_AUTO_INPUT_CALIBRATION_SETTING, NAM_INTERFACE_CALIBRATION_LEVEL_DEFAULT, NAM_INTERFACE_CALIBRATION_LEVEL_MAX, NAM_INTERFACE_CALIBRATION_LEVEL_MIN, NAM_INTERFACE_CALIBRATION_LEVEL_SETTING, NAM_OVERSAMPLING_DEFAULT, NAM_OVERSAMPLING_MAX, NAM_OVERSAMPLING_MIN, NAM_OVERSAMPLING_SETTING, NAM_SLIMMABLE_SIZE_DEFAULT, NAM_SLIMMABLE_SIZE_MAX, NAM_SLIMMABLE_SIZE_MIN, NAM_SLIMMABLE_SIZE_SETTING, PRESET_SWITCH_TAIL_DEFAULT, PRESET_SWITCH_TAIL_MAX, PRESET_SWITCH_TAIL_MIN, PRESET_SWITCH_TAIL_SETTING, TONE3000_USE_SOUNDSHED_API_SETTING, UPDATE_CHECK_ENABLED_SETTING } from "./settings/keys.js";
import { initLibraryCleanup } from "./settings/libraryCleanup.js";
import { initLibraryExport } from "./settings/libraryExport.js";
import { initLibraryFilters, renderLibraryView } from "./settings/libraryView.js";
import { initEquipmentTabs, initLibraryTabs } from "./settings/tabs.js";
import { applyTone3000ModeVisibility, clearApiKey, initTone3000ProxyHealthCheck, initTone3000UseSoundshedApiToggle, refreshTone3000ApiKeyField, saveApiKey, updateTone3000ApiModeStatus, updateTone3000ProxyHealthStatus } from "./settings/tone3000Settings.js";
import { refreshSettingsUpdateBanner, updateCurrentVersionDisplay } from "./settings/updateBanner.js";
import { getSettingValue, restoreIndexSelectSetting, sanitizeNumericSetting } from "./settings/values.js";
import { uiState } from "./state.js";
import { themeSwitcher } from "./theme-switcher.js";
import { handleAppSettingUpdate } from "./tone3000.js";
import { getTone3000ApiClientConfig } from "./tone3000Api.js";
import { initTone3000Browser } from "./tone3000Browser.js";
import { updateSignalDiagnosticsView } from "./views.js";

export { initDensitySelect, initThemeSelect, initZoomControls } from "./settings/appearance.js";
export { initDiagnosticsToggle } from "./settings/dspSettings.js";
export { handleUserInputCalibrationDiagnosticsUpdate, initUserInputCalibrationControls } from "./settings/inputCalibration.js";
export { activateAdvancedSubTab, activateEquipmentTab, activateLibraryTab } from "./settings/tabs.js";
export { refreshSettingsUpdateBanner } from "./settings/updateBanner.js";
export { refreshTone3000ApiKeyField } from "./settings/tone3000Settings.js";
export { setSettingsViewStateSuppressed } from "./settings/viewState.js";

let settingsInitialized = false;

export function initSettingsPanel(): void {
  if (settingsInitialized) {
    return;
  }
  settingsInitialized = true;
  saveButton?.addEventListener("click", () => void saveApiKey());
  clearButton?.addEventListener("click", () => void clearApiKey());
  initAudioDeviceSettings();
  initDiagnosticsToggle();
  initUserInputCalibrationControls();
  initFeatureToggles();
  initDspLevelTargetControls();
  initPresetSwitchControls();
  initFactoryArchiveLoadingToggle();
  initFactoryArchiveExportEffectPresetsToggle();
  initTone3000UseSoundshedApiToggle();
  initTone3000ProxyHealthCheck();
  initUpdateCheckToggle();
  initChannelModeControlToggle();
  initEquipmentTabs();
  initLibraryFilters();
  initLibraryCleanup();
  initThemeSelect();
  initZoomControls();
  initDensitySelect();
  initLibraryTabs();
  initLibraryExport();

  refreshSettingsView();
  initTone3000Browser();
}

export function refreshSettingsView(): void {
  const stored = getSettingValue(API_KEY_SETTING);
  const storedApiKey = typeof stored === "string" ? stored.trim() : "";
  const storedProxyMode = getSettingValue(TONE3000_USE_SOUNDSHED_API_SETTING);
  if (storedProxyMode === null && !storedApiKey) {
    updateAppSetting(TONE3000_USE_SOUNDSHED_API_SETTING, true);
    void handleAppSettingUpdate(TONE3000_USE_SOUNDSHED_API_SETTING, true);
  }

  if (apiKeyInput) {
    apiKeyInput.value = "";
  }
  refreshTone3000ApiKeyField();
  refreshUserInputCalibrationView();
  if (themeSelect) {
    themeSelect.value = themeSwitcher.getCurrentTheme();
  }
  refreshFeatureToggleStates();
  if (factoryArchiveLoadingToggle) {
    const factoryArchiveLoadingEnabled = getSettingValue(FACTORY_ARCHIVE_LOADING_SETTING);
    factoryArchiveLoadingToggle.checked = factoryArchiveLoadingEnabled === null ? true : Boolean(factoryArchiveLoadingEnabled);
  }
  if (factoryArchiveExportEffectPresetsToggle) {
    factoryArchiveExportEffectPresetsToggle.checked = getSettingValue(FACTORY_ARCHIVE_EXPORT_EFFECT_PRESETS_SETTING) === true;
  }
  if (dspNominalLevelInput) {
    const nominalLevel = sanitizeNumericSetting(
      Number(getSettingValue(DSP_NOMINAL_LEVEL_SETTING)),
      DSP_NOMINAL_LEVEL_MIN,
      DSP_NOMINAL_LEVEL_MAX,
      DSP_NOMINAL_LEVEL_DEFAULT,
    );
    dspNominalLevelInput.value = nominalLevel.toFixed(1);
  }
  if (dspProtectionCeilingInput) {
    const protectionCeiling = sanitizeNumericSetting(
      Number(getSettingValue(DSP_PROTECTION_CEILING_SETTING)),
      DSP_PROTECTION_CEILING_MIN,
      DSP_PROTECTION_CEILING_MAX,
      DSP_PROTECTION_CEILING_DEFAULT,
    );
    dspProtectionCeilingInput.value = protectionCeiling.toFixed(1);
  }
  if (dspOutputLimiterToggle) {
    dspOutputLimiterToggle.checked = Boolean(getSettingValue(DSP_OUTPUT_LIMITER_SETTING));
  }
  if (namSlimmableSizeInput) {
    const slimmableSize = sanitizeNumericSetting(
      Number(getSettingValue(NAM_SLIMMABLE_SIZE_SETTING)),
      NAM_SLIMMABLE_SIZE_MIN,
      NAM_SLIMMABLE_SIZE_MAX,
      NAM_SLIMMABLE_SIZE_DEFAULT,
    );
    namSlimmableSizeInput.value = slimmableSize.toFixed(2);
  }
  restoreIndexSelectSetting(
    presetSwitchTailSelect,
    PRESET_SWITCH_TAIL_SETTING,
    PRESET_SWITCH_TAIL_MIN,
    PRESET_SWITCH_TAIL_MAX,
    PRESET_SWITCH_TAIL_DEFAULT,
  );
  restoreIndexSelectSetting(
    namOversamplingSelect,
    NAM_OVERSAMPLING_SETTING,
    NAM_OVERSAMPLING_MIN,
    NAM_OVERSAMPLING_MAX,
    NAM_OVERSAMPLING_DEFAULT,
  );
  restoreIndexSelectSetting(
    namAntiAliasPhaseSelect,
    NAM_ANTI_ALIAS_PHASE_SETTING,
    NAM_ANTI_ALIAS_PHASE_MIN,
    NAM_ANTI_ALIAS_PHASE_MAX,
    NAM_ANTI_ALIAS_PHASE_DEFAULT,
  );
  if (namInterfaceCalibrationLevelInput) {
    const storedCal = getSettingValue(NAM_INTERFACE_CALIBRATION_LEVEL_SETTING);
    const calLevel = storedCal !== null
      ? sanitizeNumericSetting(
          Number(storedCal),
          NAM_INTERFACE_CALIBRATION_LEVEL_MIN,
          NAM_INTERFACE_CALIBRATION_LEVEL_MAX,
          NAM_INTERFACE_CALIBRATION_LEVEL_DEFAULT,
        )
      : NAM_INTERFACE_CALIBRATION_LEVEL_DEFAULT;
    namInterfaceCalibrationLevelInput.value = calLevel.toFixed(1);
  }
  if (namAutoInputCalibrationToggle) {
    const stored = getSettingValue(NAM_AUTO_INPUT_CALIBRATION_SETTING);
    namAutoInputCalibrationToggle.checked = stored === null ? true : Boolean(stored);
  }
  if (updateCheckToggle) {
    const updateCheckEnabled = getSettingValue(UPDATE_CHECK_ENABLED_SETTING);
    updateCheckToggle.checked = updateCheckEnabled === null ? true : Boolean(updateCheckEnabled);
  }
  if (channelModeControlToggle) {
    channelModeControlToggle.checked = Boolean(getSettingValue(CHANNEL_MODE_CONTROL_SETTING));
  }
  if (tone3000UseSoundshedApiToggle) {
    const useSoundshedApi = getSettingValue(TONE3000_USE_SOUNDSHED_API_SETTING);
    tone3000UseSoundshedApiToggle.checked = Boolean(useSoundshedApi);
    applyTone3000ModeVisibility(Boolean(useSoundshedApi));
  }
  updateTone3000ApiModeStatus();
  if (getTone3000ApiClientConfig().usingProxy) {
    updateTone3000ProxyHealthStatus("API health: not checked.");
  } else {
    updateTone3000ProxyHealthStatus("API health: disabled in direct mode.");
  }
  syncAudioDeviceSettingsAvailability(Boolean(uiState.environment?.audioDeviceSettings));
  syncFeatureVisibility();
  updateSignalDiagnosticsView();
  updateCurrentVersionDisplay();
  renderLibraryView();
  refreshSettingsUpdateBanner();
}
