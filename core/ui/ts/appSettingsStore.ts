/**
 * The engine's persisted app settings as the UI holds them, and the one place
 * that changes them.
 *
 * Twenty-three modules used to write `uiState.appSettings[key]` and then send
 * the same value with `setAppSetting`, each having to remember both halves —
 * and some sent a value without recording it, so the UI's copy drifted from
 * the engine's until the next full state push. A change now goes through
 * updateAppSetting, which does both. `scripts/check-state-writes.js` fails the
 * build if another module starts writing `uiState.appSettings` directly.
 *
 * Reads can still use `uiState.appSettings` — the gate is about who writes.
 */

import { setAppSetting } from "./bridge.js";
import { uiState } from "./state.js";
import type { AppSettings, AppSettingValue } from "./types.js";

/** One setting's stored value, or null when it has never been set. */
export function getAppSetting(key: string): AppSettingValue {
  return uiState.appSettings?.[key] ?? null;
}

/** Changes a setting: records it here and persists it through the engine. */
export function updateAppSetting(key: string, value: AppSettingValue): void {
  recordAppSetting(key, value);
  setAppSetting(key, value);
}

/**
 * Records a setting's value without sending it: one the engine changed itself and reported
 * with "appSettingChanged", or one a command the caller has just sent will change there
 * (`setResourceFavorite`), drawn before the engine's answer arrives.
 */
export function recordAppSetting(key: string, value: AppSettingValue): void {
  if (!uiState.appSettings) {
    uiState.appSettings = {};
  }
  uiState.appSettings[key] = value;
}

/** Changes several settings at once, in order. */
export function updateAppSettings(values: Readonly<Record<string, AppSettingValue>>): void {
  for (const [key, value] of Object.entries(values)) {
    updateAppSetting(key, value);
  }
}

/**
 * Takes the full set the engine just sent — the state push at startup, or
 * another instance's change arriving through shared sync. Nothing is sent back.
 */
export function replaceAppSettings(settings: AppSettings): void {
  uiState.appSettings = settings;
}
