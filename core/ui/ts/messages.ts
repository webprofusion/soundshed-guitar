import { uiState, clonePreset, getActivePresetForRender, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty } from "./state.js";
import { renderActivePreset, applyPresetFromLibrary, populatePresetDropdown, updatePresetDropdownSelection, cachePresetInMemory, updatePresetActionButtons, applyPresetFoldersFromBackend, applyPresetFavoritesFromBackend, applyPresetRecentsFromAppSettings, applyPresetRatingsFromBackend, applySetlistsFromBackend, handlePresetDataMessage, recordRecentPreset, refreshSavePresetModalPeakInfoIfOpen } from "./presets.js";
import { syncControlsFromState, handleInputModeChanged, handleAmpCabStateChanged, syncAutoLevelControlsFromState, applyStoredInputChannel } from "./controls.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { applyStoredDemoAudioSelection, previewSelectedDemoAudio, onDemoAudioStarted, onDemoAudioStopped, refreshDemoAudioSelectors, syncDemoAudioSelectionFromPreview } from "./demoAudio.js";
import { handleTunerUpdate, handleTunerStarted, handleTunerStopped, handleTunerReferenceChanged, handleTunerLiveModeChanged } from "./tuner.js";
import { applyUiSettings } from "./windowSettings.js";
import { updateDSPPerformancePlot, updateSignalDiagnosticsView } from "./views.js";
import { refreshSettingsView, handleUserInputCalibrationDiagnosticsUpdate } from "./settings.js";
import { applyRiffCaptureProgress, applyRiffCaptureState, applyRiffLibraryState, handleCapturedPreviewComplete, handleRiffPreviewPlayback, handleSavedRiffPreviewComplete, renderRiffLibraryPanel } from "./riffLibrary.js";
import { getRiffLibrary, postMessage } from "./bridge.js";
import { refreshSelectedNodeParams, renderSignalPathBar, updateSelectedNodePeakMeter } from "./signalPath.js";
import { refreshFxSelector } from "./fxSelector.js";
import { applyEnvironmentState, applyMetronomeState } from "./metronome.js";
import { applyToneSharingAppSettings, registerInstalledToneSharingPackFromImport } from "./toneSharingPanel.js";
import { applyJamAppSettings } from "./jam.js";
import type { GlobalSignalChainConfig, Preset, PresetFolder, ResourceRef, Setlist, UiSettings, CompositePreset } from "./types.js";
import { EffectGuids } from "./effectGuids.js";
import { migratePresetNodeTypes } from "./presetV2.js";
import { handleResourceDataMessage } from "./archiveUtils.js";
import { layoutDesigner } from "./layoutDesigner.js";
import type { LayoutLibrary, EffectLayout } from "./layoutTypes.js";
import { layoutLookupKey } from "./layoutTypes.js";
import { handleCompositeLibrary, handleCompositeDefinitionAdded, handleCompositeDefinitionRemoved } from "./compositeEffects.js";
import type { CompositeEffectDefinition } from "./compositeTypes.js";
import { renderCompositeList, handleCompositeEditModeExited, handleCompositeEditStateUpdate } from "./compositeEditor.js";
import { handleCustomEffectLibrary } from "./customEffects.js";
import { renderLayoutList } from "./layoutManager.js";
import { renderBlendList } from "./blendManager.js";
import { handleCompositePresetList, handleCompositePresetSaved, handleCompositePresetLoaded } from "./multiPresetMixer.js";
import { enterCompositeEditState, updateCompositeEditState, exitCompositeEditState } from "./state.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { themeSwitcher } from "./theme-switcher.js";
import { applyUiViewState } from "./navigation.js";
import { triggerUpdateCheck } from "./updateCheck.js";
import { getPresetSceneGraphs, normalizePresetScenes } from "./presetScenes.js";

function normalizeResourceRef(ref?: ResourceRef | null): void {
  if (!ref) return;
  const resourceType = ref.resourceType ?? "";
  const resourceId = ref.resourceId ?? "";
  if (!ref.type && resourceType) {
    ref.type = resourceType;
  }

  if (!ref.id && resourceId) {
    ref.id = resourceId;
  }
  if (ref.type && !ref.resourceType) {
    ref.resourceType = ref.type;
  }
  if (ref.id && !ref.resourceId) {
    ref.resourceId = ref.id;
  }
}

function normalizePresetResources(preset?: Preset | null): void {
  for (const graph of getPresetSceneGraphs(preset)) {
    graph.nodes.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((ref) => normalizeResourceRef(ref));
      }
    });
  }
}

const DEBUG_SNAPSHOT_SKIP_TYPES = new Set(["dspPerformance", "signalLevelDiagnostics", "captureDebugSnapshot", "debugSnapshotWritten"]);
let debugSnapshotTimer: number | null = null;

function isSensitiveDebugKey(key: string): boolean {
  const normalizedKey = key.toLowerCase();
  return normalizedKey.includes("token")
    || normalizedKey.includes("api_key")
    || normalizedKey.includes("apikey")
    || normalizedKey.includes("secret")
    || normalizedKey.includes("password")
    || normalizedKey.includes("authorization")
    || normalizedKey.includes("cookie")
    || normalizedKey.includes("credential");
}

function sanitizeDebugValue(value: unknown, seen = new WeakSet<object>(), currentKey = ""): unknown {
  if (isSensitiveDebugKey(currentKey)) {
    return "<redacted>";
  }
  if (value == null || typeof value === "string" || typeof value === "number" || typeof value === "boolean") {
    return value;
  }
  if (value instanceof Date) {
    return value.toISOString();
  }
  if (Array.isArray(value)) {
    return value.map((entry) => sanitizeDebugValue(entry, seen));
  }
  if (value instanceof Map) {
    const mapped: Record<string, unknown> = {};
    value.forEach((entryValue, entryKey) => {
      const key = String(entryKey);
      mapped[key] = sanitizeDebugValue(entryValue, seen, key);
    });
    return mapped;
  }
  if (value instanceof Set) {
    return Array.from(value.values(), (entry) => sanitizeDebugValue(entry, seen));
  }
  if (typeof value === "object") {
    if (seen.has(value as object)) {
      return "[Circular]";
    }
    seen.add(value as object);
    const sanitized: Record<string, unknown> = {};
    Object.entries(value as Record<string, unknown>).forEach(([key, entryValue]) => {
      sanitized[key] = sanitizeDebugValue(entryValue, seen, key);
    });
    seen.delete(value as object);
    return sanitized;
  }
  return String(value);
}

function describeElement(element: Element | null): Record<string, unknown> | null {
  if (!(element instanceof HTMLElement)) {
    return null;
  }
  return {
    tagName: element.tagName.toLowerCase(),
    id: element.id || null,
    className: element.className || null,
    ariaLabel: element.getAttribute("aria-label"),
    text: element.textContent?.trim().slice(0, 120) || null,
  };
}

function buildUiDebugSnapshot(source: string): Record<string, unknown> {
  const activePresetForRender = getActivePresetForRender();
  return {
    capturedAt: new Date().toISOString(),
    source,
    uiState: sanitizeDebugValue(uiState),
    activePresetForRender: sanitizeDebugValue(activePresetForRender),
    document: {
      title: document.title,
      readyState: document.readyState,
      visibilityState: document.visibilityState,
      locationHref: window.location.href,
      viewport: {
        width: window.innerWidth,
        height: window.innerHeight,
      },
      activeElement: describeElement(document.activeElement),
      bodyClassName: document.body.className,
    },
  };
}

function postUiDebugSnapshot(source: string): Record<string, unknown> {
  const snapshot = buildUiDebugSnapshot(source);
  postMessage({
    type: "debugReportUiState",
    source,
    snapshot,
  });
  return snapshot;
}

function scheduleUiDebugSnapshot(source: string): void {
  if (debugSnapshotTimer !== null) {
    window.clearTimeout(debugSnapshotTimer);
  }
  debugSnapshotTimer = window.setTimeout(() => {
    debugSnapshotTimer = null;
    postUiDebugSnapshot(source);
  }, 200);
}

window.SoundshedDebug = {
  captureSnapshot(reason = "manual"): Record<string, unknown> {
    return postUiDebugSnapshot(reason);
  },
  getUiSnapshot(reason = "manual"): Record<string, unknown> {
    return buildUiDebugSnapshot(reason);
  },
};

function applySignalPathNodeConfigUpdate(nodeId: string, key: string, value: string | undefined, valueLength?: number): void {
  const preset = getActivePresetForRender();
  if (!preset) {
    return;
  }

  const updateGraph = (graph: Preset["graph"] | undefined): boolean => {
    const node = graph?.nodes?.find((candidate) => candidate.id === nodeId);
    if (!node) {
      return false;
    }
    node.config = { ...(node.config ?? {}) };
    if (typeof value === "string") {
      node.config[key] = value;
    }
    if (key === "pluginStateBase64" && typeof valueLength === "number") {
      node.config.pluginStateBase64Length = `${valueLength}`;
    }
    return true;
  };

  let updated = updateGraph(preset.graph);
  for (const scene of preset.scenes ?? []) {
    updated = updateGraph(scene.graph) || updated;
  }

  if (!updated) {
    return;
  }

  setActivePresetDraft(preset);
  setPresetDirty(true);
  refreshSelectedNodeParams();
  renderSignalPathBar();
}

function presetSignature(preset?: Preset | null): string {
  if (!preset) return "";

  const normalize = (value: unknown): unknown => {
    if (Array.isArray(value)) {
      return value.map(normalize);
    }
    if (value && typeof value === "object") {
      const obj = value as Record<string, unknown>;
      const cleaned: Record<string, unknown> = { ...obj };
      if (typeof cleaned.resourceType === "string" || typeof cleaned.type === "string") {
        cleaned.resourceType = typeof cleaned.resourceType === "string" ? cleaned.resourceType : cleaned.type;
        delete cleaned.type;
      }
      if (typeof cleaned.resourceId === "string" || typeof cleaned.id === "string") {
        cleaned.resourceId = typeof cleaned.resourceId === "string" ? cleaned.resourceId : cleaned.id;
        delete cleaned.id;
      }
      if (cleaned.filePath === "") {
        delete cleaned.filePath;
      }
      if (cleaned.embeddedId === "") {
        delete cleaned.embeddedId;
      }
      if (cleaned.parameterId === "") {
        delete cleaned.parameterId;
      }
      if (cleaned.parameters && typeof cleaned.parameters === "object" && cleaned.parameters !== null) {
        if (Object.keys(cleaned.parameters as Record<string, unknown>).length === 0) {
          delete cleaned.parameters;
        }
      }
      if (cleaned.params && typeof cleaned.params === "object" && cleaned.params !== null) {
        const params = cleaned.params as Record<string, unknown>;
        const cleanedParams: Record<string, unknown> = { ...params };
        delete cleanedParams.calibrationInputLevel;
        delete cleanedParams.calibrationOutputLevel;
        cleaned.params = cleanedParams;
      }
      const sorted: Record<string, unknown> = {};
      Object.keys(cleaned).sort().forEach((key) => {
        sorted[key] = normalize(cleaned[key]);
      });
      return sorted;
    }
    return value;
  };

  return JSON.stringify(normalize(preset));
}

function normalizeGlobalSignalChain(chain?: GlobalSignalChainConfig | null): GlobalSignalChainConfig | null {
  if (!chain) {
    return null;
  }
  const normalizeGraph = (graph?: { nodes?: Array<Record<string, unknown>> } | null) => {
    if (!graph?.nodes) {
      return;
    }
    graph.nodes.forEach((node) => {
      const anyNode = node as { enabled?: boolean; bypassed?: boolean };
      if (typeof anyNode.bypassed !== "boolean") {
        if (typeof anyNode.enabled === "boolean") {
          anyNode.bypassed = !anyNode.enabled;
        } else {
          anyNode.bypassed = false;
        }
      }
      if (typeof anyNode.enabled !== "boolean") {
        anyNode.enabled = !anyNode.bypassed;
      }
    });
  };
  normalizeGraph(chain.preChainGraph as unknown as { nodes?: Array<Record<string, unknown>> });
  normalizeGraph(chain.postChainGraph as unknown as { nodes?: Array<Record<string, unknown>> });
  return chain;
}

export function handleIncomingMessage(message: string): void {
  const payload = JSON.parse(message) as Record<string, unknown>;
  const type = typeof payload.type === "string" ? payload.type : "";
  // Frequent diagnostics messages; avoid spamming console.
  if (type !== "dspPerformance" && type !== "signalLevelDiagnostics") {
    console.log("[JS] handleIncomingMessage received:", message.substring(0, 200));
    console.log("[JS] Parsed message type:", type);
  }

  /*if (payload.type === "dspPerformance") {
    console.log("[JS] DSP Performance message received:", payload);
  }*/
  switch (type) {
    case "state": {
      uiState.activePresetId = (payload as { activePresetId?: string }).activePresetId ?? null;
      uiState.activePresetSceneId = (payload as { activeSceneId?: string }).activeSceneId ?? uiState.activePresetSceneId ?? null;
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
        uiState.resourceLibrary = resourceLibrary as import("./types.js").ResourceLibrary;
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
        uiState.blendLibrary = blendLibrary as import("./types.js").BlendLibrary;
        refreshFxSelector();
        renderBlendList();
      }
      const customEffectLibrary = (payload as { customEffectLibrary?: unknown[] }).customEffectLibrary;
      if (Array.isArray(customEffectLibrary)) {
        handleCustomEffectLibrary(customEffectLibrary as import("./types.js").CustomEffectLibrary);
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
        uiState.appSettings = appSettings as import("./types.js").AppSettings;
        applyStoredDemoAudioSelection();
        applyStoredInputChannel();
        applyToneSharingAppSettings(appSettings);
        applyJamAppSettings();
        applyPresetRecentsFromAppSettings();
        triggerUpdateCheck();
      }
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
      const uiViewState = (payload as { uiViewState?: import("./types.js").UiViewState }).uiViewState;
      if (uiViewState) {
        uiState.uiViewState = uiViewState;
        applyUiViewState(uiViewState);
      }
      const environment = (payload as { environment?: { standalone?: boolean; version?: string; os?: string; cpu?: string } }).environment;
      if (environment) {
        applyEnvironmentState({ 
          standalone: Boolean(environment.standalone),
          version: environment.version ?? uiState.environment?.version,
          os: environment.os ?? uiState.environment?.os,
          cpu: environment.cpu ?? uiState.environment?.cpu
        });
        refreshSettingsView();
      }
      const metronome = (payload as { metronome?: { bpm?: number; enabled?: boolean; editable?: boolean; source?: string; volumeDb?: number; pan?: number; clickType?: string; beatPattern?: string; clickTypes?: Array<{ id?: string; label?: string }> } }).metronome;
      if (metronome) {
        applyMetronomeState({
          bpm: typeof metronome.bpm === "number" ? metronome.bpm : uiState.metronome?.bpm ?? 120,
          enabled: Boolean(metronome.enabled),
          editable: metronome.editable !== undefined ? Boolean(metronome.editable) : true,
          source: metronome.source === "host" ? "host" : "app",
          volumeDb: typeof metronome.volumeDb === "number" ? metronome.volumeDb : uiState.metronome?.volumeDb ?? -12,
          pan: typeof metronome.pan === "number" ? metronome.pan : uiState.metronome?.pan ?? 0,
          clickType: typeof metronome.clickType === "string" ? metronome.clickType : uiState.metronome?.clickType ?? "click",
          beatPattern: typeof metronome.beatPattern === "string" ? metronome.beatPattern : uiState.metronome?.beatPattern,
          clickTypes: Array.isArray(metronome.clickTypes)
            ? metronome.clickTypes
                .filter((entry) => entry && typeof entry.id === "string")
                .map((entry) => ({ id: entry.id ?? "", label: typeof entry.label === "string" ? entry.label : entry.id }))
            : uiState.metronome?.clickTypes,
        });
      }
      const riffLibrary = (payload as { riffLibrary?: import("./types.js").RiffLibrary }).riffLibrary;
      if (riffLibrary) {
        applyRiffLibraryState(riffLibrary);
        refreshDemoAudioSelectors();
      }
      const mixer = (payload as { mixer?: import("./types.js").MixerState }).mixer;
      if (mixer) {
        const activePresetIds = Array.isArray(mixer.activePresetIds) ? mixer.activePresetIds.slice() : [];
        const presets = mixer.presets ?? {};
        const resolvedPresets: Record<string, import("./types.js").MixerPresetState> = {};

        const ensurePreset = (id: string) => {
          const entry = presets[id] as (import("./types.js").MixerPresetState & { name?: string }) | undefined;
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

        uiState.mixer = {
          activePresetIds,
          presets: resolvedPresets,
          masterGain: typeof mixer.masterGain === "number" ? mixer.masterGain : uiState.mixer?.masterGain ?? 1.0,
          limiterEnabled: Boolean(mixer.limiterEnabled),
        };

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
                uiState.presetCache.set(slotId, p);
              }
            }
          }
        }
      }
      uiState.signalTest = null;
      const preset = (payload as { preset?: Preset }).preset;
      if (preset) {
        normalizePresetResources(preset);
        uiState.activePresetSceneId = normalizePresetScenes(preset, uiState.activePresetSceneId ?? undefined);
        const preserveNewDraft = Boolean(uiState.activePresetIsNew && uiState.activePresetId === preset.id);
        setActivePresetIsNew(preserveNewDraft);
        const snapshot = uiState.activePresetSnapshot;
        const isNewPreset = !snapshot || snapshot.id !== preset.id;
        if (isNewPreset) {
          setActivePresetSnapshot(preset);
          setPresetDirty(false);
          uiState.presetCache.set(preset.id, clonePreset(preset));
          if (!uiState.presets.some((p) => p.id === preset.id)) {
            uiState.presets = [clonePreset(preset), ...uiState.presets];
            uiState.filteredPresets = uiState.presets.slice();
            populatePresetDropdown();
          }
        } else {
          if (!uiState.presetDirty) {
            const dirty = presetSignature(snapshot) !== presetSignature(preset);
            setPresetDirty(dirty);
          }
        }
        setActivePresetDraft(preset);
      }
      renderActivePreset();
      syncControlsFromState();
      updatePresetActionButtons();
      updatePresetDropdownSelection();
      showNotification("");
      refreshSettingsView();
      break;
    }
    case "metronomeState": {
      const metroPayload = payload as { bpm?: number; enabled?: boolean; editable?: boolean; source?: string; volumeDb?: number; pan?: number; clickType?: string; clickTypes?: Array<{ id?: string; label?: string }> };
      applyMetronomeState({
        bpm: typeof metroPayload.bpm === "number" ? metroPayload.bpm : uiState.metronome?.bpm ?? 120,
        enabled: Boolean(metroPayload.enabled),
        editable: metroPayload.editable !== undefined ? Boolean(metroPayload.editable) : true,
        source: metroPayload.source === "host" ? "host" : "app",
        volumeDb: typeof metroPayload.volumeDb === "number" ? metroPayload.volumeDb : uiState.metronome?.volumeDb ?? -12,
        pan: typeof metroPayload.pan === "number" ? metroPayload.pan : uiState.metronome?.pan ?? 0,
        clickType: typeof metroPayload.clickType === "string" ? metroPayload.clickType : uiState.metronome?.clickType ?? "click",
        clickTypes: Array.isArray(metroPayload.clickTypes)
          ? metroPayload.clickTypes
              .filter((entry) => entry && typeof entry.id === "string")
              .map((entry) => ({ id: entry.id ?? "", label: typeof entry.label === "string" ? entry.label : entry.id }))
          : uiState.metronome?.clickTypes,
      });
      break;
    }
    case "riffCaptureProgress": {
      applyRiffCaptureProgress(
        (payload as { capturedSamples?: number }).capturedSamples ?? 0,
        Array.isArray((payload as { waveformPeaks?: unknown[] }).waveformPeaks)
          ? ((payload as { waveformPeaks?: unknown[] }).waveformPeaks as unknown[])
              .filter((value): value is number => typeof value === "number")
          : [],
      );
      break;
    }
    case "riffCaptureStarted": {
      appendLog(`riff capture started ← ${(payload as { takeId?: string }).takeId ?? "take"}`);
      applyRiffCaptureState({
        active: true,
        complete: false,
        takeId: (payload as { takeId?: string }).takeId ?? "",
        bars: (payload as { bars?: number }).bars ?? uiState.riffCapture?.bars ?? 1,
        tempoBpm: (payload as { tempoBpm?: number }).tempoBpm ?? uiState.riffCapture?.tempoBpm ?? 120,
        timeSigNum: (payload as { timeSigNum?: number }).timeSigNum ?? uiState.riffCapture?.timeSigNum ?? 4,
        timeSigDen: (payload as { timeSigDen?: number }).timeSigDen ?? uiState.riffCapture?.timeSigDen ?? 4,
        metronomeClickEnabled: typeof (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled === "boolean"
          ? (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled
          : uiState.riffCapture?.metronomeClickEnabled ?? true,
        hasAudio: false,
        waveformPeaks: [],
        barAlignOffsetSamples: typeof (payload as { barAlignOffsetSamples?: number }).barAlignOffsetSamples === "number"
          ? (payload as { barAlignOffsetSamples?: number }).barAlignOffsetSamples
          : 0,
      });
      showNotification("Riff capture started");
      break;
    }
    case "riffCaptureStopped": {
      appendLog(`riff capture stopped ← ${(payload as { takeId?: string }).takeId ?? "take"}`);
      const source = (payload as { source?: string }).source ?? "capture";
      applyRiffCaptureState({
        active: false,
        complete: true,
        takeId: (payload as { takeId?: string }).takeId ?? uiState.riffCapture?.takeId ?? "",
        bars: (payload as { bars?: number }).bars ?? uiState.riffCapture?.bars ?? 1,
        tempoBpm: (payload as { tempoBpm?: number }).tempoBpm ?? uiState.riffCapture?.tempoBpm ?? 120,
        timeSigNum: (payload as { timeSigNum?: number }).timeSigNum ?? uiState.riffCapture?.timeSigNum ?? 4,
        timeSigDen: (payload as { timeSigDen?: number }).timeSigDen ?? uiState.riffCapture?.timeSigDen ?? 4,
        metronomeClickEnabled: typeof (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled === "boolean"
          ? (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled
          : uiState.riffCapture?.metronomeClickEnabled ?? true,
        capturedSamples: (payload as { capturedSamples?: number }).capturedSamples ?? uiState.riffCapture?.capturedSamples ?? 0,
        sampleRate: (payload as { sampleRate?: number }).sampleRate ?? uiState.riffCapture?.sampleRate ?? 0,
        hasAudio: Boolean((payload as { hasAudio?: boolean }).hasAudio),
        waveformPeaks: Array.isArray((payload as { waveformPeaks?: unknown[] }).waveformPeaks)
          ? ((payload as { waveformPeaks?: unknown[] }).waveformPeaks as unknown[])
              .filter((value): value is number => typeof value === "number")
          : [],
      });
      showNotification(
        source === "import"
          ? "Riff WAV imported"
          : source === "editLoad"
            ? "Riff take loaded for edit"
          : source === "trim"
            ? "Riff cropped to markers"
            : "Riff capture complete",
      );
      break;
    }
    case "riffCaptureCanceled": {
      appendLog(`riff capture cancelled ← ${(payload as { takeId?: string }).takeId ?? "take"}`);
      applyRiffCaptureState({ active: false, complete: false, takeId: "", capturedSamples: 0, sampleRate: 0, hasAudio: false, waveformPeaks: [] });
      showNotification("Riff capture canceled");
      break;
    }
    case "riffSaved": {
      appendLog(`riff saved ← ${(payload as { riffId?: string }).riffId ?? "riff"}`);
      const riffLibrary = (payload as { library?: import("./types.js").RiffLibrary }).library;
      if (riffLibrary) {
        applyRiffLibraryState(riffLibrary);
      }
      showNotification("Riff saved", (payload as { path?: string }).path ?? "");
      if (!riffLibrary) {
        getRiffLibrary();
      }
      refreshDemoAudioSelectors();
      break;
    }
    case "resourceCleanupResult": {
      const removed = typeof (payload as { removed?: number }).removed === "number"
        ? (payload as { removed?: number }).removed as number
        : 0;
      const skipped = typeof (payload as { skipped?: number }).skipped === "number"
        ? (payload as { skipped?: number }).skipped as number
        : 0;
      const skippedUsed = typeof (payload as { skippedUsed?: number }).skippedUsed === "number"
        ? (payload as { skippedUsed?: number }).skippedUsed as number
        : 0;
      const message = removed > 0 ? `Removed ${removed} resources.` : "No resources removed.";
      const parts: string[] = [];
      if (skipped > 0) {
        parts.push(`${skipped} skipped`);
      }
      if (skippedUsed > 0) {
        parts.push(`${skippedUsed} in use`);
      }
      const detail = parts.length ? `${parts.join("; ")}.` : "";
      showNotification(message, detail);
      refreshSettingsView();
      break;
    }
    case "presetLoaded": {
      const preset = (payload as { preset?: Preset }).preset;
      if (preset) {
        migratePresetNodeTypes(preset);
        normalizePresetResources(preset);
        const preserveNewDraft = Boolean(uiState.activePresetIsNew && uiState.activePresetId === preset.id);
        uiState.activePresetSceneId = normalizePresetScenes(preset, (payload as { sceneId?: string }).sceneId ?? uiState.activePresetSceneId ?? undefined);
        recordRecentPreset(preset.id);
        uiState.activePresetId = preset.id;
        setActivePresetIsNew(preserveNewDraft);
        uiState.presetCache.set(preset.id, clonePreset(preset));
        setActivePresetSnapshot(preset);
        setActivePresetDraft(preset);
        setPresetDirty(false);
        updatePresetDropdownSelection();
      }
      const activePresetIds = (payload as { activePresetIds?: string[] }).activePresetIds;
      if (Array.isArray(activePresetIds)) {
        uiState.mixer = uiState.mixer ?? { activePresetIds: [], presets: {}, masterGain: 1.0, limiterEnabled: false };
        uiState.mixer.activePresetIds = activePresetIds.slice();
        activePresetIds.forEach((id) => {
          if (!uiState.mixer!.presets[id]) {
            uiState.mixer!.presets[id] = { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
          }
        });
      }
      const parameters = (payload as { parameters?: Record<string, unknown> }).parameters;
      if (parameters) {
        uiState.parameters = {
          values: Array.isArray((parameters as { parameters?: unknown }).parameters)
            ? ((parameters as { parameters: [] }).parameters as [])
            : uiState.parameters.values,
        };
      }
      if (preset) {
        uiState.presetCache.set(preset.id, clonePreset(preset));
        setActivePresetSnapshot(preset);
        setActivePresetDraft(preset);
        setPresetDirty(false);
      }
      renderActivePreset();
      syncControlsFromState();
      updatePresetActionButtons();
      break;
    }
    case "signalPathTestResult": {
      const result = payload as Record<string, unknown>;
      uiState.signalTest = {
        frequency: (result.frequency as number) ?? 0,
        duration: (result.duration as number) ?? 0,
        elapsed: (result.elapsed as number) ?? 0,
        sampleRate: (result.sampleRate as number) ?? 0,
        inputRMS: (result.inputRMS as number) ?? 0,
        outputLeft: Array.isArray(result.outputRMS) ? ((result.outputRMS as number[])[0] ?? 0) : 0,
        outputRight: Array.isArray(result.outputRMS) ? ((result.outputRMS as number[])[1] ?? 0) : 0,
        passed: Boolean(result.passed),
        message: (result.message as string) ?? "",
      };
      renderActivePreset();
      const ratio = uiState.signalTest.elapsed > 0 ? (uiState.signalTest.duration / uiState.signalTest.elapsed).toFixed(2) : "N/A";
      const timingInfo = `Audio: ${uiState.signalTest.duration.toFixed(3)}s, Elapsed: ${uiState.signalTest.elapsed.toFixed(3)}s (${ratio}x realtime)`;
      showNotification(
        uiState.signalTest.passed ? "Signal path test passed" : "Signal path test failed",
        timingInfo + (uiState.signalTest.message ? ` - ${uiState.signalTest.message}` : ""),
      );
      break;
    }
    case "previewStarted": {
      appendLog(`preview started ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
      handleRiffPreviewPlayback("start", (payload as { id?: string }).id ?? "");
      syncDemoAudioSelectionFromPreview((payload as { id?: string }).id ?? null);
      onDemoAudioStarted();
      showNotification("Playing demo audio", (payload as { title?: string }).title ?? "Demo");
      break;
    }
    case "previewComplete": {
      appendLog(`preview complete ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
      const previewId = (payload as { id?: string }).id ?? "";
      const savedRiffLooped = handleSavedRiffPreviewComplete(previewId);
      if (savedRiffLooped) {
        break;
      }
      handleRiffPreviewPlayback("stop", previewId);
      const capturedLooped = handleCapturedPreviewComplete(previewId);
      if (capturedLooped) {
        break;
      }
      onDemoAudioStopped();
      if (uiState.demoAudioRepeat) {
        previewSelectedDemoAudio();
      } else {
        showNotification("Demo playback finished", (payload as { title?: string }).title ?? "Demo");
      }
      break;
    }
    case "previewStopped": {
      appendLog(`preview stopped ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
      handleRiffPreviewPlayback("stop", (payload as { id?: string }).id ?? "");
      onDemoAudioStopped();
      showNotification("Demo playback stopped", (payload as { title?: string }).title ?? "Demo");
      break;
    }
    case "demoAudioRenderSaved": {
      const info = payload as { path?: string };
      appendLog(`demo audio rendered ← ${info.path ?? "unknown"}`);
      showNotification("Demo audio rendered", info.path ?? "");
      break;
    }
    case "demoAudioRenderFailed": {
      const info = payload as { message?: string };
      appendLog(`demo audio render failed ← ${info.message ?? "unknown"}`);
      showNotification("Demo audio render failed", info.message ?? "");
      break;
    }
    case "error": {
      console.error("Plugin error", payload);
      showNotification((payload as { message?: string }).message ?? "An error occurred", (payload as { detail?: string }).detail ?? "");
      break;
    }
    case "modelLoaded": {
      appendLog(`model loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
      renderActivePreset();
      showNotification("Model loaded", (payload as { path?: string }).path ?? "");
      break;
    }
    case "irLoaded": {
      console.log("[JS] IR loaded event received, path:", (payload as { path?: string }).path);
      appendLog(`IR loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
      renderActivePreset();
      showNotification("IR loaded", (payload as { path?: string }).path ?? "");
      break;
    }
    case "resourceImported": {
      const info = payload as { name?: string; resourceType?: string; filePath?: string };
      appendLog(`resource imported ← ${info.name ?? "unknown"}`);
      showNotification("Resource imported", info.name ?? info.filePath ?? "");
      break;
    }
    case "resourceImportFailed": {
      const info = payload as { message?: string; detail?: string };
      appendLog(`resource import failed ← ${info.message ?? "unknown"}`);
      showNotification(info.message ?? "Import failed", info.detail ?? "");
      break;
    }
    case "toneSharingPackImported": {
      const info = payload as { fileName?: string; path?: string; byteSize?: number };
      const detail = info.path ?? info.fileName ?? "";
      appendLog(`tone sharing pack imported ← ${detail}`);
      registerInstalledToneSharingPackFromImport(info);
      showNotification("Pack imported", detail);
      break;
    }
    case "toneSharingPackImportFailed": {
      const info = payload as { message?: string };
      appendLog(`tone sharing pack import failed ← ${info.message ?? "unknown"}`);
      showNotification("Pack import failed", info.message ?? "");
      break;
    }
    case "resourceData": {
      handleResourceDataMessage(payload as { requestId: string; data?: string; fileName?: string; message?: string });
      break;
    }
    case "resourceDataFailed": {
      handleResourceDataMessage(payload as { requestId: string; data?: string; fileName?: string; message?: string });
      break;
    }
    case "blendExportSaved": {
      const info = payload as { path?: string };
      showNotification("Blend exported", info.path ?? "");
      break;
    }
    case "blendExportFailed": {
      const info = payload as { message?: string };
      showNotification("Blend export failed", info.message ?? "");
      break;
    }
    case "libraryExportSaved": {
      const info = payload as { path?: string };
      showNotification("Library exported", info.path ?? "");
      break;
    }
    case "libraryExportFailed": {
      const info = payload as { message?: string };
      showNotification("Library export failed", info.message ?? "");
      break;
    }
    case "presetExportSaved": {
      const info = payload as { path?: string };
      showNotification("Preset exported", info.path ?? "");
      break;
    }
    case "presetExportFailed": {
      const info = payload as { message?: string };
      showNotification("Preset export failed", info.message ?? "");
      break;
    }
    case "presetSaved": {
      appendLog(`preset saved ← ${(payload as { preset?: Preset }).preset?.name ?? "unknown"}`);
      const savedPreset = (payload as { preset?: Preset }).preset;
      if (savedPreset) {
        normalizePresetResources(savedPreset);
        uiState.activePresetSceneId = normalizePresetScenes(savedPreset, (payload as { sceneId?: string }).sceneId ?? uiState.activePresetSceneId ?? undefined);
        cachePresetInMemory(savedPreset);
        uiState.activePresetId = savedPreset.id;
        setActivePresetIsNew(false);
        uiState.presetCache.set(savedPreset.id, clonePreset(savedPreset));
        setActivePresetSnapshot(savedPreset);
        setActivePresetDraft(savedPreset);
        setPresetDirty(false);
        if (!uiState.presets.some((p) => p.id === savedPreset.id)) {
          uiState.presets.unshift(clonePreset(savedPreset));
          uiState.filteredPresets = uiState.presets.slice();
          populatePresetDropdown();
        }
        renderActivePreset();
        updatePresetDropdownSelection();
      }
      showNotification("Preset saved", (payload as { path?: string }).path ?? savedPreset?.name ?? "");
      break;
    }
    case "presetList": {
      const presetListPayload = payload as { presets?: Array<{ id: string; name: string; category?: string; source?: string }> };
      if (Array.isArray(presetListPayload.presets)) {
        appendLog(`preset list received ← ${presetListPayload.presets.length} presets`);
        for (const p of presetListPayload.presets) {
          if (!uiState.presetCache.has(p.id)) {
            const stub: Preset = { id: p.id, name: p.name, category: p.category ?? "Factory" } as Preset;
            uiState.presetCache.set(p.id, stub);
            if (!uiState.presets.some((existing) => existing.id === p.id)) {
              uiState.presets.push(stub);
            }
          }
        }
        uiState.filteredPresets = uiState.presets.slice();
        populatePresetDropdown();
        renderActivePreset();
      }
      break;
    }
    case "appInfo": {
      const infoPayload = payload as { version?: string; os?: string; cpu?: string };
      applyEnvironmentState({
        standalone: uiState.environment?.standalone ?? false,
        version: infoPayload.version ?? uiState.environment?.version,
        os: infoPayload.os ?? uiState.environment?.os,
        cpu: infoPayload.cpu ?? uiState.environment?.cpu,
      });
      refreshSettingsView();
      break;
    }
    case "presetData": {
      const presetPayload = payload as { preset?: Preset };
      if (presetPayload.preset) {
        migratePresetNodeTypes(presetPayload.preset);
        normalizePresetResources(presetPayload.preset);
        normalizePresetScenes(presetPayload.preset);
        handlePresetDataMessage(presetPayload.preset);
      }
      break;
    }
    case "presetFolders": {
      const foldersPayload = payload as { folders?: PresetFolder[]; activeFolderId?: string | null };
      applyPresetFoldersFromBackend(foldersPayload.folders ?? [], foldersPayload.activeFolderId ?? null);
      break;
    }
    case "presetFavorites": {
      const favoritesPayload = payload as { favorites?: string[] };
      applyPresetFavoritesFromBackend(Array.isArray(favoritesPayload.favorites) ? favoritesPayload.favorites : []);
      break;
    }
    case "presetRatings": {
      const ratingsPayload = payload as { ratings?: Record<string, number> };
      applyPresetRatingsFromBackend(ratingsPayload.ratings ?? {});
      break;
    }
    case "setlists": {
      const setlistsPayload = payload as { setlists?: Setlist[]; activeSetlistId?: string | null };
      applySetlistsFromBackend(setlistsPayload.setlists ?? [], setlistsPayload.activeSetlistId ?? null);
      break;
    }
    case "theme": {
      const themePayload = payload as { theme?: string };
      const theme = themePayload.theme === "light" || themePayload.theme === "classic" ? themePayload.theme : "dark";
      themeSwitcher.applyTheme(theme);
      break;
    }
    case "tunerUpdate": {
      const tunerPayload = payload as { 
        detected?: boolean; 
        noteName?: string; 
        octave?: number;
        frequency?: number;
        centOffset?: number;
        confidence?: number;
        debugRms?: number;
        debugRawFreq?: number;
      };
      
      // Log debug info to console
      const rms = tunerPayload.debugRms?.toFixed(6) ?? "?";
      const rawFreq = tunerPayload.debugRawFreq?.toFixed(2) ?? "?";
      console.log(`[Tuner] RMS=${rms}, rawFreq=${rawFreq}Hz, detected=${tunerPayload.detected}, note=${tunerPayload.noteName ?? "-"}`);
      
      handleTunerUpdate({
        detected: tunerPayload.detected ?? false,
        noteName: tunerPayload.noteName,
        octave: tunerPayload.octave,
        frequency: tunerPayload.frequency,
        centOffset: tunerPayload.centOffset,
        confidence: tunerPayload.confidence,
      });
      break;
    }
    case "tunerStarted": {
      const startPayload = payload as { referenceFrequency?: number; liveMode?: boolean };
      handleTunerStarted(startPayload.referenceFrequency ?? 440.0);
      if (startPayload.liveMode !== undefined) {
        handleTunerLiveModeChanged(startPayload.liveMode);
      }
      break;
    }
    case "tunerStopped": {
      handleTunerStopped();
      break;
    }
    case "tunerReferenceChanged": {
      handleTunerReferenceChanged((payload as { referenceFrequency?: number }).referenceFrequency ?? 440.0);
      break;
    }
    case "tunerLiveModeChanged": {
      handleTunerLiveModeChanged((payload as { liveMode?: boolean }).liveMode ?? true);
      break;
    }
    case "debug": {
      const msg = (payload as { message?: string }).message ?? "";
      console.log("[C++]", msg);
      appendLog(`[C++] ${msg}`);
      break;
    }
    case "captureDebugSnapshot": {
      const source = typeof (payload as { source?: string }).source === "string"
        ? (payload as { source?: string }).source as string
        : "backend-request";
      postUiDebugSnapshot(source);
      break;
    }
    case "debugSnapshotWritten": {
      const info = payload as { path?: string; source?: string };
      console.log("[DebugSnapshot] written", info.path ?? "", info.source ?? "");
      if (info.source === "footer-button") {
        appendLog(`debug snapshot written ← ${info.path ?? "unknown path"}`);
        showNotification("Debug state captured", info.path ?? "logs/debug-state.json");
      }
      break;
    }
    case "inputModeChanged": {
      const modePayload = payload as { monoMode?: boolean; inputChannel?: number };
      handleInputModeChanged(
        modePayload.monoMode ?? true,
        modePayload.inputChannel ?? 1
      );
      appendLog(`Input mode changed: ${modePayload.monoMode ? "Mono" : "Stereo"}, Channel: ${(modePayload.inputChannel ?? 1) + 1}`);
      break;
    }
    case "ampCabStateChanged": {
      const statePayload = payload as { ampEnabled?: boolean; cabEnabled?: boolean };
      handleAmpCabStateChanged(
        statePayload.ampEnabled ?? true,
        statePayload.cabEnabled ?? true
      );
      appendLog(`Amp: ${statePayload.ampEnabled ? "ON" : "OFF"}, Cab: ${statePayload.cabEnabled ? "ON" : "OFF"}`);
      break;
    }
    case "autoLevelChanged": {
      const autoPayload = payload as { autoInput?: boolean; autoOutput?: boolean };
      const activeId = uiState.activePresetId ?? "";
      const preset = uiState.presetCache.get(activeId) as any;
      if (preset) {
        const globals = preset.globals ?? preset.global ?? {};
        const merged = {
          inputTrim: globals.inputTrim ?? 0,
          outputTrim: globals.outputTrim ?? 0,
          masterVolume: globals.masterVolume ?? globals.outputVolume ?? 1,
          autoLevelInput: autoPayload.autoInput ?? globals.autoLevelInput ?? false,
          autoLevelOutput: autoPayload.autoOutput ?? globals.autoLevelOutput ?? false,
          transpose: globals.transpose ?? 0,
        };
        preset.globals = merged;
        preset.global = merged;
        uiState.presetCache.set(activeId, preset);
      }
      syncAutoLevelControlsFromState();
      break;
    }
    case "uiSettingsChanged": {
      const uiSettings = (payload as { settings?: UiSettings }).settings;
      if (uiSettings) {
        uiState.uiSettings = uiSettings;
        applyUiSettings(uiSettings);
        applyPresetRecentsFromAppSettings();
      }
      break;
    }
    case "dspPerformance": {
      const stats = payload as {
        stats?: import("./types.js").DSPPerformanceStats;
        sampleRate?: number;
        blockSize?: number;
      };
      if (stats.stats) {
        const mergedStats: import("./types.js").DSPPerformanceStats = {
          ...stats.stats,
          sampleRate: stats.sampleRate ?? stats.stats.sampleRate,
          blockSize: stats.blockSize ?? stats.stats.blockSize,
        };
        uiState.dspPerformance = mergedStats;
        uiState.dspPerformanceHistory.push(mergedStats);
        if (uiState.dspPerformanceHistory.length > 100) {
          uiState.dspPerformanceHistory.shift();
        }
       
        // Update plot if panel is visible
        updateDSPPerformancePlot();
      }
      break;
    }
    case "signalLevelDiagnostics": {
      const diagnostics = payload as unknown as import("./types.js").SignalLevelDiagnostics;
      if (diagnostics && diagnostics.input && diagnostics.output) {
        uiState.signalDiagnostics = diagnostics;
        updateSignalDiagnosticsView();
        handleUserInputCalibrationDiagnosticsUpdate();
        updateSelectedNodePeakMeter();
        refreshSavePresetModalPeakInfoIfOpen();
      }
      break;
    }
    case "signalPathNodeConfigUpdated": {
      const update = payload as {
        nodeId?: string;
        key?: string;
        value?: string;
        valueLength?: number;
        captured?: boolean;
        silent?: boolean;
      };
      if (typeof update.nodeId === "string" && typeof update.key === "string") {
        if (typeof update.value === "string") {
          applySignalPathNodeConfigUpdate(update.nodeId, update.key, update.value, update.valueLength);
        } else if (update.captured && update.key === "pluginStateBase64") {
          applySignalPathNodeConfigUpdate(update.nodeId, update.key, undefined, update.valueLength);
        }
        if (update.key === "pluginStateBase64" && !update.silent) {
          showNotification("Plugin state captured", "success");
        }
      }
      break;
    }
    case "globalSignalChainChanged":
    case "globalChain": {
      const chainPayload = payload as { config?: import("./types.js").GlobalSignalChainConfig; globalSignalChain?: import("./types.js").GlobalSignalChainConfig };
      const chainConfig = chainPayload.config ?? chainPayload.globalSignalChain;
      if (chainConfig) {
        uiState.globalSignalChain = normalizeGlobalSignalChain(chainConfig) ?? uiState.globalSignalChain;
        appendLog("Global signal chain configuration loaded");
        syncControlsFromState();
      }
      break;
    }
    case "layoutLibraryLoaded": {
      const libraryPayload = payload as { layoutLibrary?: LayoutLibrary };
      if (libraryPayload.layoutLibrary) {
        uiState.layoutLibrary = libraryPayload.layoutLibrary;
        appendLog("Layout library loaded");
        renderLayoutList();
        // Ensure any open node params panel picks up the updated layout mapping.
        renderActivePreset();
      }
      break;
    }
    case "layoutSaved": {
      const savePayload = payload as { effectType?: string; blendId?: string; layoutId?: string; lookupKey?: string };
      const displayKey = savePayload.blendId ? `${savePayload.effectType} (blend: ${savePayload.blendId})` : savePayload.effectType;
      appendLog(`Layout saved for ${displayKey ?? "effect"}${savePayload.layoutId ? ` (${savePayload.layoutId})` : ""}`);
      showNotification("Layout saved", "success");
      // layoutLibraryLoaded will follow and trigger a full refresh.
      break;
    }
    case "layoutImageSelected": {
      console.log("[Messages] layoutImageSelected received:", payload);
      const imgPayload = payload as { purpose?: string; imageId?: string; fileName?: string; dataUrl?: string; layerIndex?: number; paramKey?: string };
      if (imgPayload.purpose && imgPayload.imageId && imgPayload.fileName) {
        // Add image to layout library so it can be resolved
        if (uiState.layoutLibrary) {
          const existingIdx = uiState.layoutLibrary.images.findIndex(img => img.imageId === imgPayload.imageId);
          const imageEntry = { 
            imageId: imgPayload.imageId, 
            fileName: imgPayload.fileName, 
            dataUrl: imgPayload.dataUrl,
            type: imgPayload.purpose as "background" | "knob" | "general" 
          };
          if (existingIdx >= 0) {
            uiState.layoutLibrary.images[existingIdx] = imageEntry;
          } else {
            uiState.layoutLibrary.images.push(imageEntry);
          }
        }
        layoutDesigner.handleImageSelected(
          imgPayload.purpose,
          imgPayload.imageId,
          imgPayload.layerIndex,
          imgPayload.paramKey
        );
      }
      break;
    }
    case "layoutExportSaved": {
      const exportPayload = payload as { path?: string };
      if (exportPayload.path) {
        showNotification(`Layout exported to ${exportPayload.path}`, "success");
        appendLog(`Layout exported: ${exportPayload.path}`);
      }
      break;
    }
    case "layoutExportFailed": {
      const failPayload = payload as { message?: string };
      showNotification(failPayload.message ?? "Layout export failed", "error");
      appendLog(`Layout export failed: ${failPayload.message ?? "unknown error"}`);
      break;
    }
    case "compositeLibrary": {
      const compPayload = payload as { definitions?: CompositeEffectDefinition[] };
      if (compPayload.definitions) {
        handleCompositeLibrary(compPayload.definitions);
        refreshFxSelector();
      }
      break;
    }
    case "customEffectLibrary": {
      const customPayload = payload as { entries?: import("./types.js").CustomEffectLibrary };
      if (Array.isArray(customPayload.entries)) {
        handleCustomEffectLibrary(customPayload.entries);
        refreshFxSelector();
      }
      break;
    }
    case "customEffectSaved": {
      const customPayload = payload as { name?: string; applyToNode?: boolean };
      const detail = customPayload.name ?? "Custom Effect";
      appendLog(`custom effect saved ← ${detail}`);
      showNotification(customPayload.applyToNode ? "Custom Effect applied" : "Custom Effect saved", detail);
      break;
    }
    case "generatedCustomEffectBundleExportSaved": {
      const exportPayload = payload as { path?: string };
      showNotification("Custom Effect bundle exported", exportPayload.path ?? "");
      break;
    }
    case "generatedCustomEffectBundleExportFailed": {
      const exportPayload = payload as { message?: string };
      showNotification("Custom Effect bundle export failed", exportPayload.message ?? "");
      break;
    }
    case "effectCatalog": {
      const catalogPayload = payload as { catalog?: Array<Record<string, unknown>> };
      if (Array.isArray(catalogPayload.catalog)) {
        for (const entry of catalogPayload.catalog) {
          if (!entry || typeof entry !== "object") {
            continue;
          }
          const effect = entry as {
            type?: unknown;
            name?: unknown;
            category?: unknown;
            parameters?: unknown;
            requiresResource?: unknown;
            resourceType?: unknown;
            exposedResources?: unknown;
          };
          const type = typeof effect.type === "string" ? effect.type : "";
          if (!type) {
            continue;
          }
          const existing = EffectTypeRegistry.get(type);
          const displayName = typeof effect.name === "string" ? effect.name : existing?.displayName ?? type;
          const category = typeof effect.category === "string" ? effect.category : existing?.category ?? "utility";
          const requiresResource =
            typeof effect.requiresResource === "boolean" ? effect.requiresResource : existing?.requiresResource ?? false;
          const resourceType = typeof effect.resourceType === "string" ? effect.resourceType : existing?.resourceType;
          const existingParamsByKey = new Map(
            (existing?.parameters ?? []).map((param) => [param.key, param]),
          );
          const parameters = Array.isArray(effect.parameters)
            ? effect.parameters
                .filter((param) => param && typeof param === "object")
                .map((param) => {
                  const p = param as {
                    key?: unknown;
                    name?: unknown;
                    default?: unknown;
                    min?: unknown;
                    max?: unknown;
                    unit?: unknown;
                    step?: unknown;
                    labels?: unknown;
                    group?: unknown;
                    advanced?: unknown;
                  };
                  const key = typeof p.key === "string" ? p.key : "";
                  const existingParam = key ? existingParamsByKey.get(key) : undefined;
                  const labels = Array.isArray(p.labels) ? p.labels.filter((label) => typeof label === "string") : null;
                  return {
                    key,
                    name: typeof p.name === "string" ? p.name : existingParam?.name ?? "",
                    default: typeof p.default === "number" ? p.default : existingParam?.default ?? 0,
                    min: typeof p.min === "number" ? p.min : existingParam?.min ?? 0,
                    max: typeof p.max === "number" ? p.max : existingParam?.max ?? 1,
                    unit: typeof p.unit === "string" ? p.unit : existingParam?.unit ?? "",
                    step: typeof p.step === "number" ? p.step : existingParam?.step,
                    labels: labels ?? existingParam?.labels,
                    group: typeof p.group === "string" ? p.group : existingParam?.group,
                    advanced: typeof p.advanced === "boolean" ? p.advanced : existingParam?.advanced,
                  };
                })
                .filter((param) => param.key !== "")
            : existing?.parameters ?? [];

          const exposedResources = Array.isArray(effect.exposedResources)
            ? effect.exposedResources
                .filter((resource) => resource && typeof resource === "object")
                .map((resource) => {
                  const r = resource as {
                    resourceId?: unknown;
                    displayName?: unknown;
                    nodeId?: unknown;
                    resourceType?: unknown;
                    resourceIndex?: unknown;
                    allowBrowseFile?: unknown;
                    parameterId?: unknown;
                    parameterValue?: unknown;
                  };
                  return {
                    resourceId: typeof r.resourceId === "string" ? r.resourceId : "",
                    displayName: typeof r.displayName === "string" ? r.displayName : "",
                    nodeId: typeof r.nodeId === "string" ? r.nodeId : "",
                    resourceType: typeof r.resourceType === "string" ? r.resourceType : "",
                    resourceIndex: typeof r.resourceIndex === "number" ? r.resourceIndex : 0,
                    allowBrowseFile: typeof r.allowBrowseFile === "boolean" ? r.allowBrowseFile : true,
                    parameterId: typeof r.parameterId === "string" ? r.parameterId : undefined,
                    parameterValue: typeof r.parameterValue === "number" ? r.parameterValue : undefined,
                  };
                })
                .filter((resource) => resource.resourceId && resource.resourceType)
            : existing?.exposedResources;

          EffectTypeRegistry.register(type, {
            type,
            displayName,
            category,
            catalogHidden: existing?.catalogHidden ?? (type === EffectGuids.kAmpNam || type === EffectGuids.kAmpNamBlend),
            requiresResource,
            resourceType,
            parameters,
            exposedResources,
          });
        }
        refreshFxSelector();
        if (getActivePresetForRender()) {
          renderActivePreset();
          refreshSelectedNodeParams();
        }
      }
      break;
    }
    case "compositeDefinitionAdded": {
      const compAddPayload = payload as { definition?: CompositeEffectDefinition };
      if (compAddPayload.definition) {
        handleCompositeDefinitionAdded(compAddPayload.definition);
        refreshFxSelector();
        renderCompositeList();
      }
      break;
    }
    case "compositeDefinitionRemoved": {
      const compRemovePayload = payload as { id?: string };
      if (compRemovePayload.id) {
        handleCompositeDefinitionRemoved(compRemovePayload.id);
        refreshFxSelector();
        renderCompositeList();
      }
      break;
    }
    case "compositeEditState": {
      // C++ broadcasts the composite's current inner graph after each mutation
      const editPayload = payload as { definition?: CompositeEffectDefinition };
      if (editPayload.definition) {
        const isAlreadyEditing = uiState.compositeEditMode;
        if (isAlreadyEditing) {
          updateCompositeEditState(editPayload.definition);
        } else {
          enterCompositeEditState(editPayload.definition);
        }
        renderSignalPathBar();
        handleCompositeEditStateUpdate();
      }
      break;
    }
    case "compositeEditModeExited": {
      // C++ confirms we've left composite edit mode
      exitCompositeEditState();
      handleCompositeEditModeExited();
      renderSignalPathBar();
      break;
    }
    case "compositePresetList": {
      const list = (payload as { compositePresets?: CompositePreset[] }).compositePresets;
      if (Array.isArray(list)) {
        handleCompositePresetList(list);
      }
      break;
    }
    case "compositePresetSaved": {
      const saved = payload as { id?: string; name?: string };
      handleCompositePresetSaved(saved.id ?? "", saved.name ?? "");
      break;
    }
    case "compositePresetLoaded": {
      const loaded = payload as { id?: string; name?: string };
      handleCompositePresetLoaded(loaded.id ?? "", loaded.name ?? "");
      break;
    }
    default:
      console.warn("Unknown message type", payload.type);
  }

  if (!DEBUG_SNAPSHOT_SKIP_TYPES.has(type)) {
    scheduleUiDebugSnapshot(`incoming:${type}`);
  }
}

// Optional: handle full mixer state sync from plugin
export function handleMixerStateMessage(message: Record<string, unknown>): void {
  const mixer = message as { activePresetIds?: string[]; presets?: Record<string, unknown>; masterGain?: number; limiterEnabled?: boolean };
  uiState.mixer = uiState.mixer ?? { activePresetIds: [], presets: {}, masterGain: 1.0, limiterEnabled: false };
  if (Array.isArray(mixer.activePresetIds)) {
    uiState.mixer.activePresetIds = mixer.activePresetIds.slice();
  }
  if (typeof mixer.masterGain === "number") {
    uiState.mixer.masterGain = mixer.masterGain as number;
  }
  if (typeof mixer.limiterEnabled === "boolean") {
    uiState.mixer.limiterEnabled = mixer.limiterEnabled as boolean;
  }
  // Merge per-preset states if provided
  if (mixer.presets && typeof mixer.presets === "object") {
    for (const [pid, st] of Object.entries(mixer.presets)) {
      const ps = st as { mix?: number; pan?: number; mute?: boolean; solo?: boolean };
      const cur = uiState.mixer.presets[pid] || { id: pid, mix: 1.0, pan: 0.0, mute: false, solo: false };
      uiState.mixer.presets[pid] = {
        id: pid,
        mix: typeof ps.mix === "number" ? ps.mix : cur.mix,
        pan: typeof ps.pan === "number" ? ps.pan : cur.pan,
        mute: typeof ps.mute === "boolean" ? ps.mute : cur.mute,
        solo: typeof ps.solo === "boolean" ? ps.solo : cur.solo,
      };
    }
  }
}

/**
 * Send a global signal chain parameter change to the plugin.
 * @param paramPath - Dot-notation path to the parameter (e.g., "postChainGraph.global_eq.params.lowGain")
 * @param value - The new value for the parameter
 */
export function sendGlobalChainParam(paramPath: string, value: number | boolean): void {
  window.NAMBridge?.postMessage({
    type: "setGlobalChainParam",
    path: paramPath,
    value,
  });
}

/**
 * Request the current global signal chain configuration from the plugin.
 */
export function requestGlobalChainState(): void {
  window.NAMBridge?.postMessage({
    type: "getGlobalChain",
  });
}
