/**
 * Theme, zoom and density — settings that change how the rest of the UI looks
 * rather than how it sounds.
 */

import type { DensityPreference } from "../compactMode.js";
import { getDensityPreference, setDensityPreference } from "../compactMode.js";
import { uiState } from "../state.js";
import { themeSwitcher } from "../theme-switcher.js";
import type { ThemeName } from "../theme-switcher.js";
import { updateAppSetting } from "../appSettingsStore.js";
import { WHEEL_ADJUST_SETTING, isWheelAdjustEnabled } from "../wheelAdjust.js";
import { SIGNAL_FLOW_ANIMATION_SETTING, isSignalFlowAnimationEnabled, updateSignalPathFlow } from "../signalPath/signalFlow.js";
import { updateUiSettings } from "../windowSettings.js";
import { densityLevelSelect, footerDensitySelect, signalFlowAnimationToggle, themeSelect, wheelAdjustToggle, zoomLevelSelect } from "./dom.js";
import { ZOOM_DEFAULT, ZOOM_LEVEL_OPTIONS, ZOOM_MAX, ZOOM_MIN } from "./keys.js";

let themeSelectInitialized = false;

let zoomControlsInitialized = false;

let densitySelectInitialized = false;

/**
 * Layout density. Auto follows the window, which is what almost everyone wants;
 * the two pinned values are for the cases it cannot see — a player who wants the
 * dense stage layout on a large display, and someone who would rather scroll a
 * full-size panel than learn a second arrangement of it.
 */
export function initDensitySelect(): void {
  // The settings panel's select, and the same choice in the compact footer's OPTIONS sheet.
  const selects = [densityLevelSelect, footerDensitySelect].filter((select): select is HTMLSelectElement => Boolean(select));
  if (!selects.length || densitySelectInitialized) {
    return;
  }
  densitySelectInitialized = true;

  const options: Array<{ value: DensityPreference; label: string }> = [
    { value: "auto", label: "Auto layout" },
    { value: "compact", label: "Compact" },
    { value: "full", label: "Full" },
  ];

  for (const select of selects) {
    select.innerHTML = options
      .map((option) => `<option value="${option.value}">${option.label}</option>`)
      .join("");

    select.value = getDensityPreference();

    select.addEventListener("change", () => {
      setDensityPreference(select.value as DensityPreference);
    });
  }

  // The stored preference arrives with app settings, well after this binds.
  window.addEventListener("densityPreferenceChanged", () => {
    for (const select of selects) {
      select.value = getDensityPreference();
    }
  });
}

export function initWheelAdjustToggle(): void {
  // Bound to a local so the null check narrows inside the listener.
  const toggle = wheelAdjustToggle;
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.checked = isWheelAdjustEnabled();
  toggle.addEventListener("change", () => {
    updateAppSetting(WHEEL_ADJUST_SETTING, Boolean(toggle.checked));
  });
}

/** Shows the stored value; app settings arrive after the panel binds. */
export function refreshWheelAdjustToggle(): void {
  if (wheelAdjustToggle) {
    wheelAdjustToggle.checked = isWheelAdjustEnabled();
  }
}

export function initSignalFlowAnimationToggle(): void {
  const toggle = signalFlowAnimationToggle;
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.checked = isSignalFlowAnimationEnabled();
  toggle.addEventListener("change", () => {
    updateAppSetting(SIGNAL_FLOW_ANIMATION_SETTING, Boolean(toggle.checked));
    // Light or clear the chain now rather than on the next diagnostics frame.
    updateSignalPathFlow();
  });
}

/** Shows the stored value; app settings arrive after the panel binds. */
export function refreshSignalFlowAnimationToggle(): void {
  if (signalFlowAnimationToggle) {
    signalFlowAnimationToggle.checked = isSignalFlowAnimationEnabled();
  }
}

export function initThemeSelect(): void {
  // Bound to a local so the null check still narrows inside the listeners:
  // TypeScript will not carry a narrowing on an imported binding into a
  // closure, because another module could reassign it between the two.
  const select = themeSelect;
  if (!select || themeSelectInitialized) {
    return;
  }
  themeSelectInitialized = true;

  const themes: Array<{ value: ThemeName; label: string }> = [
    { value: "light", label: "Light" },
    { value: "dark", label: "Dark" },
    { value: "classic", label: "Vintage" },
  ];

  select.innerHTML = themes
    .map((theme) => `<option value="${theme.value}">${theme.label}</option>`)
    .join("");

  select.value = themeSwitcher.getCurrentTheme();

  select.addEventListener("change", () => {
    const value = select.value as ThemeName;
    themeSwitcher.setTheme(value);
  });

  window.addEventListener("themeChanged", ((event: CustomEvent) => {
    select.value = event.detail.theme as ThemeName;
  }) as EventListener);
}

export function initZoomControls(): void {
  if (!zoomLevelSelect || zoomControlsInitialized) {
    return;
  }
  zoomControlsInitialized = true;

  const select = zoomLevelSelect;

  function getCurrentZoom(): number {
    const stateZoom = uiState.uiSettings?.zoom;
    if (typeof stateZoom === "number" && Number.isFinite(stateZoom)) {
      return stateZoom;
    }
    const inlineZoom = Number.parseFloat(document.body.style.zoom || "");
    if (Number.isFinite(inlineZoom)) {
      return inlineZoom;
    }
    return ZOOM_DEFAULT;
  }

  function findClosestZoomOption(zoomLevel: number): number {
    let closest = ZOOM_LEVEL_OPTIONS[0];
    let minDistance = Number.POSITIVE_INFINITY;
    for (const option of ZOOM_LEVEL_OPTIONS) {
      const distance = Math.abs(option - zoomLevel);
      if (distance < minDistance) {
        minDistance = distance;
        closest = option;
      }
    }
    return closest;
  }

  function populateZoomSelect(): void {
    if (!select) {
      return;
    }
    select.innerHTML = ZOOM_LEVEL_OPTIONS
      .map((option) => {
        const percentage = Math.round(option * 100);
        return `<option value="${option.toFixed(2)}">${percentage}%</option>`;
      })
      .join("");
  }

  function syncZoomSelect(zoomLevel: number): void {
    const closestOption = findClosestZoomOption(zoomLevel);
    select.value = closestOption.toFixed(2);
  }

  function setZoomLevel(zoomLevel: number): void {
    const clampedZoom = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, zoomLevel));
    uiState.uiSettings = {
      ...(uiState.uiSettings ?? { zoom: clampedZoom }),
      zoom: clampedZoom,
    };
    syncZoomSelect(clampedZoom);
    updateUiSettings({ zoom: clampedZoom });
  }

  populateZoomSelect();
  select.addEventListener("change", () => {
    const selected = Number.parseFloat(select.value);
    if (Number.isFinite(selected)) {
      setZoomLevel(selected);
    }
  });

  window.addEventListener("uiSettingsApplied", () => {
    const current = getCurrentZoom();
    syncZoomSelect(current);
  });

  const currentZoom = getCurrentZoom();
  syncZoomSelect(currentZoom);
}
