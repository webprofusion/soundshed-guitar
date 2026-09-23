/**
 * Cross-instance sync: another window changed the shared store, so this one
 * re-reads it.
 */

import { renderBlendList } from "../blendManager.js";
import { replaceAppSettings } from "../appSettingsStore.js";
import { postMessage } from "../bridge.js";
import { applyDensityAppSettings } from "../compactMode.js";
import { applyStoredInputChannel } from "../controls.js";
import { handleCustomEffectLibrary } from "../customEffects.js";
import { applyStoredDemoAudioSelection } from "../demoAudio.js";
import { refreshFxSelector } from "../fxSelector.js";
import { applyJamAppSettings } from "../jam.js";
import { applyPerformancePadAppSettings, refreshPerformancePads } from "../performancePads.js";
import { applyPresetArchiveSessionState, applyPresetRecentsFromAppSettings, applyPresetRecentsFromBackend } from "../presets.js";
import { refreshSettingsView } from "../settings.js";
import { uiState } from "../state.js";
import { applyToneSharingAppSettings } from "../toneSharingPanel.js";
import type { AppSettings, BlendLibrary, CustomEffectLibrary, PresetArchiveSessionState, ResourceLibrary, UiSettings } from "../types.js";
import { triggerUpdateCheck } from "../updateCheck.js";
import type { IncomingPayload } from "./types.js";

let sharedSyncRefreshTimer: number | null = null;

let pendingSharedPresetHydration = false;

/**
 * Another instance changed the shared store, so the preset list that follows
 * has to re-read every preset this window had already cached.
 */
export function requestSharedPresetHydration(): void {
  pendingSharedPresetHydration = true;
}

/** True once, for the first preset list to arrive after such a change. */
export function takePendingSharedPresetHydration(): boolean {
  const pending = pendingSharedPresetHydration;
  pendingSharedPresetHydration = false;
  return pending;
}

export function scheduleSharedSyncRefresh(): void {
  if (sharedSyncRefreshTimer !== null) {
    return;
  }

  sharedSyncRefreshTimer = window.setTimeout(() => {
    sharedSyncRefreshTimer = null;
    postMessage({ type: "getSharedSyncState" });
  }, 1000);
}

export function onSharedSyncUpdated(): void {
  requestSharedPresetHydration();
  scheduleSharedSyncRefresh();
}

export function onSharedSyncState(payload: IncomingPayload): void {
  requestSharedPresetHydration();
  const sharedPayload = payload as {
    appSettings?: Record<string, unknown>;
    uiSettings?: UiSettings;
    resourceLibrary?: Record<string, unknown[]>;
    blendLibrary?: unknown[];
    customEffectLibrary?: unknown[];
    presetArchiveSession?: PresetArchiveSessionState | null;
  };

  if (sharedPayload.appSettings) {
    replaceAppSettings(sharedPayload.appSettings as AppSettings);
    applyDensityAppSettings(sharedPayload.appSettings);
    applyStoredDemoAudioSelection();
    applyToneSharingAppSettings(sharedPayload.appSettings);
    applyJamAppSettings();
    applyPresetRecentsFromAppSettings();
    applyPerformancePadAppSettings(sharedPayload.appSettings as AppSettings);
    triggerUpdateCheck();
    applyStoredInputChannel();
  }
  applyPresetArchiveSessionState(sharedPayload.presetArchiveSession ?? null);

  if (sharedPayload.uiSettings) {
    uiState.uiSettings = sharedPayload.uiSettings;
    // The engine keeps the recents there; the blob this UI sends back must carry them.
    applyPresetRecentsFromBackend(sharedPayload.uiSettings.presetRecents);
  }

  if (sharedPayload.resourceLibrary) {
    uiState.resourceLibrary = sharedPayload.resourceLibrary as ResourceLibrary;
  }

  if (Array.isArray(sharedPayload.blendLibrary)) {
    uiState.blendLibrary = sharedPayload.blendLibrary as BlendLibrary;
    renderBlendList();
  }

  if (Array.isArray(sharedPayload.customEffectLibrary)) {
    handleCustomEffectLibrary(sharedPayload.customEffectLibrary as CustomEffectLibrary);
  }

  refreshFxSelector();
  refreshPerformancePads();
  refreshSettingsView();
}
