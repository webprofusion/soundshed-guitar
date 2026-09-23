/**
 * The big periodic state message, and the host-level settings that arrive
 * alongside it.
 */

import { applyAutomationState } from "../automationPanel.js";
import { renderBlendList } from "../blendManager.js";
import { recordAppSetting, replaceAppSettings } from "../appSettingsStore.js";
import { requestGlobalChainState } from "../bridge.js";
import { applyDensityAppSettings } from "../compactMode.js";
import { renderCompositeList } from "../compositeEditor.js";
import { handleCompositeLibrary } from "../compositeEffects.js";
import type { CompositeEffectDefinition } from "../compositeTypes.js";
import { applyOutputMuted, applyStoredInputChannel, handleAmpCabStateChanged, handleInputModeChanged, syncControlsFromState } from "../controls.js";
import { handleCustomEffectLibrary } from "../customEffects.js";
import { applyDemoClips, applyStoredDemoAudioSelection, refreshDemoAudioSelectors } from "../demoAudio.js";
import { refreshFxSelector } from "../fxSelector.js";
import { applyJamAppSettings } from "../jam.js";
import { appendLog } from "../logging.js";
import { applyEnvironmentState, applyMetronomeState } from "../metronome.js";
import { reconcileActiveCompositePreset } from "../multiPresetMixer.js";
import { applyUiViewState } from "../navigation.js";
import { replaceNavigationViewState } from "../navigationState.js";
import { showNotification } from "../notifications.js";
import { applyPerformancePadAppSettings, refreshPerformancePads } from "../performancePads.js";
import { applyPresetArchiveSessionState, applyPresetRecentsFromAppSettings, populatePresetDropdown, rejectPendingPresetRequest, renderActivePreset, updatePresetActionButtons, updatePresetDropdownSelection } from "../presets.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { migratePresetNodeTypes } from "../presetV2.js";
import { applyRiffLibraryState } from "../riffLibrary.js";
import { refreshSettingsView } from "../settings.js";
import { applyEnginePresetDirty, clonePreset, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, uiState } from "../state.js";
import { replaceMixerState } from "../mixerStore.js";
import { cachePreset, putLibraryPresetFirst, setActivePresetId, setActivePresetSceneId, setPresetLoadingId, showAllLibraryPresets } from "../presetLibraryStore.js";
import { themeSwitcher } from "../theme-switcher.js";
import { applyToneSharingAppSettings } from "../toneSharingPanel.js";
import type { AppSettings, AppSettingValue, AutomationSlot, BlendLibrary, CustomEffectLibrary, GlobalSignalChainConfig, MixerPresetState, MixerState, Preset, PresetArchiveSessionState, ResourceLibrary, RiffLibrary, UiSettings, UiViewState } from "../types.js";
import { triggerUpdateCheck } from "../updateCheck.js";
import { applyUiSettings } from "../windowSettings.js";
import { shouldIgnoreStatePreset } from "./echoGuard.js";
import { normalizeGlobalSignalChain, normalizePresetResources } from "./normalize.js";
import type { IncomingPayload } from "./types.js";

export function onState(payload: IncomingPayload): void {
  setActivePresetId((payload as { activePresetId?: string }).activePresetId ?? null);
  setActivePresetSceneId((payload as { activeSceneId?: string }).activeSceneId ?? uiState.activePresetSceneId ?? null);
  const parameters = (payload as { parameters?: Record<string, unknown> }).parameters;
  if (parameters) {
    uiState.parameters = {
      values: Array.isArray((parameters as { parameters?: unknown }).parameters)
        ? ((parameters as { parameters: [] }).parameters as [])
        : [],
    };
  }
  // Process resource library
  const resourceLibrary = (payload as { resourceLibrary?: Record<string, unknown[]> }).resourceLibrary;
  if (resourceLibrary) {
    uiState.resourceLibrary = resourceLibrary as ResourceLibrary;
  }
  const missingNodeResources = (payload as { missingNodeResources?: Array<{ nodeId?: string; resourceType?: string; resourceId?: string; filePath?: string }> }).missingNodeResources;
  if (Array.isArray(missingNodeResources)) {
    uiState.missingNodeResources = missingNodeResources
      .filter((entry) => entry && typeof entry.nodeId === "string")
      .map((entry) => ({
        nodeId: entry.nodeId ?? "",
        resourceType: typeof entry.resourceType === "string" ? entry.resourceType : undefined,
        resourceId: typeof entry.resourceId === "string" ? entry.resourceId : undefined,
        filePath: typeof entry.filePath === "string" ? entry.filePath : undefined,
      }));
  } else {
    uiState.missingNodeResources = [];
  }
  const blendLibrary = (payload as { blendLibrary?: unknown[] }).blendLibrary;
  if (Array.isArray(blendLibrary)) {
    uiState.blendLibrary = blendLibrary as BlendLibrary;
    refreshFxSelector();
    renderBlendList();
  }
  const customEffectLibrary = (payload as { customEffectLibrary?: unknown[] }).customEffectLibrary;
  if (Array.isArray(customEffectLibrary)) {
    handleCustomEffectLibrary(customEffectLibrary as CustomEffectLibrary);
    refreshFxSelector();
  }
  const compositeLibrary = (payload as { compositeLibrary?: CompositeEffectDefinition[] }).compositeLibrary;
  if (Array.isArray(compositeLibrary)) {
    handleCompositeLibrary(compositeLibrary);
    refreshFxSelector();
    renderCompositeList();
  }
  const appSettings = (payload as { appSettings?: Record<string, unknown> }).appSettings;
  if (appSettings) {
    replaceAppSettings(appSettings as AppSettings);
    applyDensityAppSettings(appSettings);
    applyStoredDemoAudioSelection();
    applyToneSharingAppSettings(appSettings);
    applyJamAppSettings();
    applyPresetRecentsFromAppSettings();
    applyPerformancePadAppSettings(appSettings as AppSettings);
    triggerUpdateCheck();
  }
  applyPresetArchiveSessionState((payload as { presetArchiveSession?: PresetArchiveSessionState | null }).presetArchiveSession ?? null);
  const globalSignalChain = (payload as { globalSignalChain?: GlobalSignalChainConfig }).globalSignalChain;
  if (globalSignalChain) {
    uiState.globalSignalChain = normalizeGlobalSignalChain(globalSignalChain) ?? uiState.globalSignalChain;
  } else {
    requestGlobalChainState();
  }
  const uiSettings = (payload as { uiSettings?: UiSettings }).uiSettings;
  if (uiSettings) {
    uiState.uiSettings = uiSettings;
    applyUiSettings(uiSettings);
  }
  const uiViewState = (payload as { uiViewState?: UiViewState }).uiViewState;
  if (uiViewState) {
    replaceNavigationViewState(uiViewState);
    applyUiViewState(uiViewState);
  }
  const environment = (payload as { environment?: { standalone?: boolean; audioDeviceSettings?: boolean; version?: string; os?: string; cpu?: string } }).environment;
  if (environment) {
    applyEnvironmentState({
      standalone: Boolean(environment.standalone),
      audioDeviceSettings: environment.audioDeviceSettings === true,
      version: environment.version ?? uiState.environment?.version,
      os: environment.os ?? uiState.environment?.os,
      cpu: environment.cpu ?? uiState.environment?.cpu
    });
    refreshSettingsView();
  }
  // Apply stored input channel AFTER environment so isStandaloneUi() is correct.
  if (appSettings) {
    applyStoredInputChannel();
  }
  const metronome = (payload as { metronome?: Record<string, unknown> }).metronome;
  if (metronome) {
    applyMetronomeState(metronome);
  }
  const riffLibrary = (payload as { riffLibrary?: RiffLibrary }).riffLibrary;
  if (riffLibrary) {
    applyRiffLibraryState(riffLibrary);
    refreshDemoAudioSelectors();
  }
  // The demo clips the engine reads, plays and renders itself (ui/demo/clips.json).
  applyDemoClips((payload as { demoClips?: unknown }).demoClips);
  const outputMuted = (payload as { outputMuted?: unknown }).outputMuted;
  if (typeof outputMuted === "boolean") {
    applyOutputMuted(outputMuted);
  }
  const automation = (payload as { automation?: AutomationSlot[] }).automation;
  if (automation) {
    applyAutomationState({ slots: automation });
  }
  const mixer = (payload as { mixer?: MixerState }).mixer;
  if (mixer) {
    const activePresetIds = Array.isArray(mixer.activePresetIds) ? mixer.activePresetIds.slice() : [];
    const presets = mixer.presets ?? {};
    const resolvedPresets: Record<string, MixerPresetState> = {};

    const ensurePreset = (id: string) => {
      const entry = presets[id] as (MixerPresetState & { name?: string }) | undefined;
      resolvedPresets[id] = {
        id,
        name: typeof entry?.name === "string" ? entry.name : undefined,
        mix: typeof entry?.mix === "number" ? entry.mix : 1.0,
        pan: typeof entry?.pan === "number" ? entry.pan : 0.0,
        mute: Boolean(entry?.mute),
        solo: Boolean(entry?.solo),
      };
    };

    activePresetIds.forEach((id) => ensurePreset(id));
    Object.keys(presets).forEach((id) => {
      if (!resolvedPresets[id]) ensurePreset(id);
    });

    replaceMixerState({
      activePresetIds,
      presets: resolvedPresets,
      masterGain: typeof mixer.masterGain === "number" ? mixer.masterGain : uiState.mixer?.masterGain ?? 1.0,
      mixGainDb: typeof mixer.mixGainDb === "number" ? mixer.mixGainDb : uiState.mixer?.mixGainDb ?? 0,
    });

    // Populate presetCache with full graph data for each mixer slot.
    // The C++ includes these so the UI can display signal chains even for
    // slots that the user has never explicitly loaded as the active preset.
    const presetGraphs = (mixer as { presetGraphs?: Record<string, unknown> }).presetGraphs;
    if (presetGraphs && typeof presetGraphs === "object") {
      for (const [slotId, presetData] of Object.entries(presetGraphs)) {
        if (presetData && typeof presetData === "object") {
          const existing = uiState.presetCache.get(slotId);
          // Only overwrite stubs (entries without graph nodes)
          if (!existing?.graph?.nodes?.length) {
            const p = presetData as Preset;
            migratePresetNodeTypes(p);
            normalizePresetResources(p);
            normalizePresetScenes(p);
            cachePreset(p, slotId);
          }
        }
      }
    }
    reconcileActiveCompositePreset();
  }
  uiState.signalTest = null;
  const preset = (payload as { preset?: Preset }).preset;
  if (preset) {
    if (!shouldIgnoreStatePreset(preset)) {
      normalizePresetResources(preset);
      setActivePresetSceneId(normalizePresetScenes(preset, uiState.activePresetSceneId ?? undefined));
      const preserveNewDraft = Boolean(uiState.activePresetIsNew && uiState.activePresetId === preset.id);
      setActivePresetIsNew(preserveNewDraft);
      const snapshot = uiState.activePresetSnapshot;
      const isNewPreset = !snapshot || snapshot.id !== preset.id;
      // The engine owns the unsaved-changes flag (activePresetDirty).
      applyEnginePresetDirty((payload as { activePresetDirty?: unknown }).activePresetDirty, isNewPreset);
      if (isNewPreset) {
        setActivePresetSnapshot(preset);
        cachePreset(clonePreset(preset));
        if (!uiState.presets.some((p) => p.id === preset.id)) {
          putLibraryPresetFirst(clonePreset(preset));
          showAllLibraryPresets();
          populatePresetDropdown();
        }
      }
      setActivePresetDraft(preset);
    } else {
      appendLog(`state preset ignored ← ${preset.name ?? preset.id ?? "unknown"} (stale post-save state)`);
    }
  }
  renderActivePreset();
  refreshPerformancePads();
  syncControlsFromState();
  updatePresetActionButtons();
  updatePresetDropdownSelection();
  refreshSettingsView();
}

export function onError(payload: IncomingPayload): void {
  console.error("Plugin error", payload);
  const requestId = (payload as { requestId?: string }).requestId;
  if (requestId) {
    rejectPendingPresetRequest(requestId, (payload as { message?: string }).message ?? "An error occurred", (payload as { detail?: string }).detail);
  }
  showNotification((payload as { message?: string }).message ?? "An error occurred", (payload as { detail?: string }).detail ?? "");
  if (uiState.presetLoadingId) {
    // A backend-driven load (e.g. a setlist step onto a missing preset) failed, so the
    // "presetLoaded" that would clear the loading state is never coming.
    setPresetLoadingId(null);
    renderActivePreset();
  }
}

/** The output mute changed: the engine's answer to "setOutputMuted". */
export function onOutputMutedChanged(payload: IncomingPayload): void {
  applyOutputMuted((payload as { muted?: unknown }).muted === true);
}

/**
 * The engine changed one app setting itself, as the answer to an edit command
 * ("setResourceFavorite"). It is recorded as it stands, and not sent back.
 */
export function onAppSettingChanged(payload: IncomingPayload): void {
  const change = payload as { key?: unknown; value?: AppSettingValue };
  if (typeof change.key === "string" && change.key) {
    recordAppSetting(change.key, change.value ?? null);
  }
}

export function onAppInfo(payload: IncomingPayload): void {
  const infoPayload = payload as { version?: string; os?: string; cpu?: string };
  applyEnvironmentState({
    standalone: uiState.environment?.standalone ?? false,
    audioDeviceSettings: uiState.environment?.audioDeviceSettings ?? false,
    version: infoPayload.version ?? uiState.environment?.version,
    os: infoPayload.os ?? uiState.environment?.os,
    cpu: infoPayload.cpu ?? uiState.environment?.cpu,
  });
  refreshSettingsView();
}

export function onTheme(payload: IncomingPayload): void {
  const themePayload = payload as { theme?: string };
  const theme = themePayload.theme === "light" || themePayload.theme === "classic" ? themePayload.theme : "dark";
  themeSwitcher.applyTheme(theme);
}

export function onInputModeChanged(payload: IncomingPayload): void {
  const modePayload = payload as { monoMode?: boolean; inputChannel?: number };
  handleInputModeChanged(
    modePayload.monoMode ?? true,
    modePayload.inputChannel ?? 1
  );
  appendLog(`Input mode changed: ${modePayload.monoMode ? "Mono" : "Stereo"}, Channel: ${(modePayload.inputChannel ?? 1) + 1}`);
}

export function onAmpCabStateChanged(payload: IncomingPayload): void {
  const statePayload = payload as { ampEnabled?: boolean; cabEnabled?: boolean };
  handleAmpCabStateChanged(
    statePayload.ampEnabled ?? true,
    statePayload.cabEnabled ?? true
  );
  appendLog(`Amp: ${statePayload.ampEnabled ? "ON" : "OFF"}, Cab: ${statePayload.cabEnabled ? "ON" : "OFF"}`);
}

