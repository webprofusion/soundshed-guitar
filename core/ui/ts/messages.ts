import { uiState, clonePreset, enterCompositeEditState, exitCompositeEditState, getActivePresetForRender, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, updateCompositeEditState } from "./state.js";
import { recordDspLoadSample } from "./dspPerformance.js";
import { renderActivePreset, populatePresetDropdown, updatePresetDropdownSelection, cachePresetInMemory, updatePresetActionButtons, applyPresetFoldersFromBackend, applyPresetFavoritesFromBackend, applyPresetRecentsFromAppSettings, applyPresetRatingsFromBackend, applySetlistsFromBackend, applySetlistCursorFromBackend, handlePresetDataMessage, recordRecentPreset, refreshSavePresetModalPeakInfoIfOpen, rejectPendingPresetRequest, applyPresetArchiveSessionState, refreshPresetCacheEntryFromBackend } from "./presets.js";
import { syncControlsFromState, handleInputModeChanged, handleAmpCabStateChanged, syncAutoLevelControlsFromState, applyStoredInputChannel } from "./controls.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { applyStoredDemoAudioSelection, previewSelectedDemoAudio, onDemoAudioStarted, onDemoAudioStopped, refreshDemoAudioSelectors, syncDemoAudioSelectionFromPreview } from "./demoAudio.js";
import { handleTunerUpdate, handleTunerStarted, handleTunerStopped, handleTunerReferenceChanged, handleTunerLiveModeChanged } from "./tuner.js";
import { applyUiSettings } from "./windowSettings.js";
import { updateDSPPerformancePlot, updateSignalDiagnosticsView } from "./views.js";
import { refreshSettingsView, handleUserInputCalibrationDiagnosticsUpdate } from "./settings.js";
import { applyRiffCaptureProgress, applyRiffCaptureState, applyRiffLibraryState, handleCapturedPreviewComplete, handleRiffPreviewPlayback, handleSavedRiffPreviewComplete } from "./riffLibrary.js";
import { applyPracticeToolFileLoaded, applyPracticeToolPlaybackEnded, applyPracticeToolTransportState } from "./practiceTool.js";
import { getRiffLibrary, postMessage, requestGlobalChainState, requestSignalDiagnosticsRoster } from "./bridge.js";
import { refreshEffectPresetsFlyout, applySpatialPositionUpdate, handleHostedPluginResourceLoadFailed, handleHostedPluginResourceLoadCompleted, handleNodeResourceBrowseCancelled, refreshSelectedNodeParams, renderSignalPathBar, updateSelectedNodeAnalyzerPanel, updateSelectedNodeDspStatus, updateSelectedNodePeakMeter } from "./signalPath.js";
import { refreshFxSelector } from "./fxSelector.js";
import { applyEnvironmentState, applyMetronomeBeat, applyMetronomeState } from "./metronome.js";
import { applyAutomationState, handleMidiLogEntry, handleMidiLearnCapture } from "./automationPanel.js";
import { applyToneSharingAppSettings, registerInstalledToneSharingPackFromImport, handleToneSharingDeepLink } from "./toneSharingPanel.js";
import { applyJamAppSettings } from "./jam.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import type { AppSettings, AutomationRegistryEntry, AutomationSlot, BlendLibrary, CompositePreset, CustomEffectLibrary, DSPPerformanceStats, GlobalSignalChainConfig, InputAnalyzerTelemetry, LibraryResource, MixerPresetState, MixerState, Preset, PresetArchiveSessionState, PresetFolder, ResourceLibrary, ResourceRef, RiffLibrary, Setlist, SignalDiagnosticsAnalyzerFrame, SignalDiagnosticsFrame, SignalDiagnosticsRoster, SignalLevelDiagnostics, SignalLevelMetrics, SignalLevelNodeMetrics, StoredEffectPreset, UiSettings, UiViewState } from "./types.js";
import { SIGNAL_DIAGNOSTICS_NODE_TUPLE_LENGTH, SIGNAL_DIAGNOSTICS_TUPLE_LENGTH } from "./types.js";
import { EffectGuids } from "./effectGuids.js";
import { EffectTypeRegistry, migratePresetNodeTypes, setNodeParam } from "./presetV2.js";
import { handleResourceDataMessage } from "./archiveUtils.js";
import { layoutDesigner } from "./layoutDesigner.js";
import type { LayoutImageRef, LayoutLibrary } from "./layoutTypes.js";
import { handleCompositeLibrary, handleCompositeDefinitionAdded, handleCompositeDefinitionRemoved } from "./compositeEffects.js";
import type { CompositeEffectDefinition } from "./compositeTypes.js";
import { renderCompositeList, handleCompositeEditModeExited, handleCompositeEditStateUpdate } from "./compositeEditor.js";
import { handleCustomEffectLibrary } from "./customEffects.js";
import { renderLayoutList } from "./layoutManager.js";
import { ensureLayoutImagesLoaded, markLayoutImagesLoaded, areLayoutImagesLoaded } from "./layoutImages.js";
import { renderBlendList } from "./blendManager.js";
import { handleCompositePresetList, handleCompositePresetSaved, handleCompositePresetLoaded } from "./multiPresetMixer.js";
import { themeSwitcher } from "./theme-switcher.js";
import { applyUiViewState } from "./navigation.js";
import { triggerUpdateCheck } from "./updateCheck.js";
import { getPresetSceneGraphs, normalizePresetScenes } from "./presetScenes.js";
import { shouldMarkSignalPathNodeConfigUpdateDirty } from "./signalPathConfigUpdates.js";
import { applyPerformancePadAppSettings, refreshPerformancePads } from "./performancePads.js";

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

let ignoreNextStatePresetId: string | null = null;

function normalizeResourcePath(path: string): string {
  return path.trim().replace(/\\/g, "/").toLowerCase();
}

function upsertImportedResourceInUiState(info: { id?: string; name?: string; resourceType?: string; filePath?: string }): void {
  const resourceType = (info.resourceType ?? "").trim();
  const resourceId = (info.id ?? "").trim();
  if (!resourceType || !resourceId) return;

  const currentList = uiState.resourceLibrary[resourceType] ?? [];
  const importedPath = normalizeResourcePath(info.filePath ?? "");
  const existingIndex = currentList.findIndex((resource) => {
    if (resource.id === resourceId) return true;
    return importedPath.length > 0 && normalizeResourcePath(resource.filePath ?? "") === importedPath;
  });

  const existing = existingIndex >= 0 ? currentList[existingIndex] : null;
  const name = (info.name ?? "").trim() || existing?.name || resourceId;
  const filePath = (info.filePath ?? "").trim() || existing?.filePath || "";

  const nextResource: LibraryResource = {
    id: resourceId,
    name,
    category: existing?.category ?? "Uncategorized",
    description: existing?.description ?? "",
    tags: existing?.tags ?? [],
    filePath,
    hash: existing?.hash,
    metadata: existing?.metadata,
    fileMissing: existing?.fileMissing,
  };

  const nextList = [...currentList];
  if (existingIndex >= 0) {
    nextList[existingIndex] = nextResource;
  } else {
    nextList.push(nextResource);
  }

  uiState.resourceLibrary = {
    ...uiState.resourceLibrary,
    [resourceType]: nextList,
  };
}

function removeResourceFromUiState(info: { id?: string; resourceType?: string }): void {
  const resourceType = (info.resourceType ?? "").trim();
  const resourceId = (info.id ?? "").trim();
  if (!resourceType || !resourceId) return;

  const currentList = uiState.resourceLibrary[resourceType] ?? [];
  const nextList = currentList.filter((resource) => resource.id !== resourceId);
  if (nextList.length === currentList.length) return;

  uiState.resourceLibrary = {
    ...uiState.resourceLibrary,
    [resourceType]: nextList,
  };
}
let ignoreNextStatePresetExpiresAtMs = 0;

function markIgnoreNextStatePreset(id: string): void {
  const presetId = id.trim();
  if (!presetId) {
    return;
  }
  ignoreNextStatePresetId = presetId;
  ignoreNextStatePresetExpiresAtMs = Date.now() + 1500;
}

function shouldIgnoreStatePreset(incoming: Preset): boolean {
  const incomingId = incoming.id?.trim() ?? "";
  if (!incomingId || !ignoreNextStatePresetId) {
    return false;
  }

  if (incomingId !== ignoreNextStatePresetId) {
    return false;
  }

  const stillValid = Date.now() <= ignoreNextStatePresetExpiresAtMs;
  // One-shot guard: clear once a matching state payload is observed.
  ignoreNextStatePresetId = null;
  ignoreNextStatePresetExpiresAtMs = 0;
  return stillValid;
}

const DEBUG_SNAPSHOT_SKIP_TYPES = new Set(["dspPerformance", "sld", "sldA", "sldRoster", "spatialPosition", "captureDebugSnapshot", "debugSnapshotWritten"]);
let debugSnapshotTimer: number | null = null;

/* ── Signal diagnostics reassembly ─────────────────────────────────────────
 * The backend streams levels as bare numeric tuples ("sld", 20 Hz) resolved against
 * a roster ("sldRoster") that is only re-sent when the node set changes, plus separate
 * analyzer payloads ("sldA"). Everything is rebuilt here into the SignalLevelDiagnostics
 * shape the views consume, so the wire format stays independent of the render code.
 */
let signalDiagnosticsRoster: SignalDiagnosticsRoster | null = null;
const signalDiagnosticsAnalyzerByNodeId = new Map<string, InputAnalyzerTelemetry>();

/** Frames arrive at 20 Hz; one roster request per second is enough to recover. */
const SIGNAL_DIAGNOSTICS_ROSTER_RETRY_MS = 1000;
let signalDiagnosticsRosterRequestedAt = 0;

function requestSignalDiagnosticsRosterThrottled(): void {
  const now = Date.now();
  if (now - signalDiagnosticsRosterRequestedAt < SIGNAL_DIAGNOSTICS_ROSTER_RETRY_MS) {
    return;
  }
  signalDiagnosticsRosterRequestedAt = now;
  requestSignalDiagnosticsRoster();
}

function signalDiagnosticsMetrics(values: number[], offset: number): SignalLevelMetrics {
  const peakDbfs = values[offset];
  return {
    peakDbfs,
    rmsDbfs: values[offset + 1],
    // Derived rather than transmitted: the backend computes it the same way.
    headroomDb: Math.max(0, -peakDbfs),
    clipCount: values[offset + 2],
    clipped: values[offset + 3] === 1,
  };
}

function applySignalDiagnosticsRoster(roster: SignalDiagnosticsRoster): void {
  if (!roster || !Array.isArray(roster.nodes)) {
    return;
  }
  signalDiagnosticsRoster = roster;

  // Drop cached analyzer telemetry for nodes that are no longer present, so a removed
  // analyzer cannot keep painting stale spectrogram data.
  const liveNodeIds = new Set(roster.nodes.map((entry) => entry[2]));
  for (const nodeId of [...signalDiagnosticsAnalyzerByNodeId.keys()]) {
    if (!liveNodeIds.has(nodeId)) {
      signalDiagnosticsAnalyzerByNodeId.delete(nodeId);
    }
  }
}

function applySignalDiagnosticsFrame(frame: SignalDiagnosticsFrame): boolean {
  const roster = signalDiagnosticsRoster;
  // A frame that predates the roster we hold describes a different node set — drop it
  // and wait for the matching roster rather than mismapping levels onto the wrong nodes.
  if (!roster || frame?.seq !== roster.seq || !Array.isArray(frame.d)) {
    // Nothing will arrive on its own: rosters are only sent when the node set changes,
    // so with none held (or a stale one) this drops every frame forever. Frames are the
    // signal that the backend is streaming and we cannot read it, so ask for a roster.
    if (Array.isArray(frame?.d)) {
      requestSignalDiagnosticsRosterThrottled();
    }
    return false;
  }
  if (frame.d.length !== roster.nodes.length * SIGNAL_DIAGNOSTICS_NODE_TUPLE_LENGTH) {
    return false;
  }

  const nodes: SignalLevelNodeMetrics[] = roster.nodes.map((entry, index) => {
    const [scope, presetId, nodeId, nodeType] = entry;
    const offset = index * SIGNAL_DIAGNOSTICS_NODE_TUPLE_LENGTH;
    const node: SignalLevelNodeMetrics = {
      scope,
      nodeId,
      nodeType,
      // Per-frame, not from the roster: it follows the signal, not the node set.
      channelCount: frame.d[offset + SIGNAL_DIAGNOSTICS_TUPLE_LENGTH],
      levels: signalDiagnosticsMetrics(frame.d, offset),
    };
    if (presetId) {
      node.presetId = presetId;
    }
    const analyzer = signalDiagnosticsAnalyzerByNodeId.get(nodeId);
    if (analyzer) {
      node.analyzer = analyzer;
    }
    return node;
  });

  const diagnostics: SignalLevelDiagnostics = {
    rawInput: signalDiagnosticsMetrics(frame.r, 0),
    input: signalDiagnosticsMetrics(frame.i, 0),
    output: signalDiagnosticsMetrics(frame.o, 0),
    nodes,
  };
  uiState.signalDiagnostics = diagnostics;
  return true;
}

function applySignalDiagnosticsAnalyzer(frame: SignalDiagnosticsAnalyzerFrame): boolean {
  const roster = signalDiagnosticsRoster;
  if (!roster || frame?.seq !== roster.seq || !frame.id || !Array.isArray(frame.l)) {
    return false;
  }

  const [spectrogramMinDbfs, spectrogramMaxDbfs, spectrogramMinHz, spectrogramMaxHz] = roster.spectrogramRange;
  const [barkMinDbfs, barkMaxDbfs, barkMinHz, barkMaxHz] = roster.barkRange;
  const stereo = frame.l[9] === 1;

  const analyzer: InputAnalyzerTelemetry = {
    levels: {
      peakPercent: frame.l[0],
      rmsPercent: frame.l[1],
      rmsDbu: frame.l[2],
      rmsDbv: frame.l[3],
      rmsVolts: frame.l[4],
      momentaryLufs: frame.l[5],
      shortTermLufs: frame.l[6],
      integratedLufs: frame.l[7],
      activeChannelCount: frame.l[8],
      stereo,
      loudnessValid: frame.l[10] === 1,
      channelMode: stereo ? "stereo" : "mono",
    },
    spectrogram: {
      binsDb: frame.s ?? [],
      minDbfs: spectrogramMinDbfs,
      maxDbfs: spectrogramMaxDbfs,
      minFrequencyHz: spectrogramMinHz,
      maxFrequencyHz: spectrogramMaxHz,
      generatedAtMs: frame.t,
    },
    bark: {
      bandsDb: frame.b ?? [],
      minDbfs: barkMinDbfs,
      maxDbfs: barkMaxDbfs,
      minFrequencyHz: barkMinHz,
      maxFrequencyHz: barkMaxHz,
      generatedAtMs: frame.t,
    },
  };

  signalDiagnosticsAnalyzerByNodeId.set(frame.id, analyzer);

  // Analyzer payloads follow their frame, so patch the snapshot the frame just built
  // rather than waiting a further 50 ms for the next one.
  const node = uiState.signalDiagnostics?.nodes.find((candidate) => candidate.nodeId === frame.id);
  if (node) {
    node.analyzer = analyzer;
  }
  return true;
}

const TELEMETRY_UI_FPS = 30;
const TELEMETRY_UI_FRAME_MS = 1000 / TELEMETRY_UI_FPS;

let telemetryUiRafId: number | null = null;
let telemetryUiDelayTimer: number | null = null;
let telemetryUiLastFlushMs = 0;
let telemetryUiPendingDsp = false;
let telemetryUiPendingSignalDiagnostics = false;
let sharedSyncRefreshTimer: number | null = null;
let pendingSharedPresetHydration = false;

function flushTelemetryUiUpdates(): void {
  if (telemetryUiPendingDsp) {
    updateDSPPerformancePlot();
    updateSelectedNodeDspStatus();
    telemetryUiPendingDsp = false;
  }

  if (telemetryUiPendingSignalDiagnostics) {
    updateSignalDiagnosticsView();
    handleUserInputCalibrationDiagnosticsUpdate();
    updateSelectedNodePeakMeter();
    updateSelectedNodeDspStatus();
    updateSelectedNodeAnalyzerPanel();
    refreshSavePresetModalPeakInfoIfOpen();
    telemetryUiPendingSignalDiagnostics = false;
  }
}

function scheduleTelemetryUiFlush(): void {
  if (telemetryUiRafId !== null || telemetryUiDelayTimer !== null) {
    return;
  }

  telemetryUiRafId = window.requestAnimationFrame((timestamp) => {
    telemetryUiRafId = null;

    const elapsedMs = timestamp - telemetryUiLastFlushMs;
    if (elapsedMs < TELEMETRY_UI_FRAME_MS) {
      const delayMs = Math.ceil(TELEMETRY_UI_FRAME_MS - elapsedMs);
      telemetryUiDelayTimer = window.setTimeout(() => {
        telemetryUiDelayTimer = null;
        scheduleTelemetryUiFlush();
      }, delayMs);
      return;
    }

    telemetryUiLastFlushMs = timestamp;
    flushTelemetryUiUpdates();
  });
}

function queueTelemetryUiUpdate(kind: "dsp" | "signalDiagnostics"): void {

  if (kind === "dsp") {
    telemetryUiPendingDsp = true;
  } else {
    telemetryUiPendingSignalDiagnostics = true;
  }

  scheduleTelemetryUiFlush();
}

function scheduleSharedSyncRefresh(): void {
  if (sharedSyncRefreshTimer !== null) {
    return;
  }

  sharedSyncRefreshTimer = window.setTimeout(() => {
    sharedSyncRefreshTimer = null;
    postMessage({ type: "getSharedSyncState" });
  }, 1000);
}

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
  // Primitives, dates, arrays, maps and plain objects have all returned by
  // now; only functions, symbols and bigints reach here, where String() is
  // the correct rendering.
  // eslint-disable-next-line @typescript-eslint/no-base-to-string
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
  // Long enough that a burst of messages (a preset switch is ~4) coalesces into one
  // snapshot, and that the snapshot lands after the interaction rather than during it.
  debugSnapshotTimer = window.setTimeout(() => {
    debugSnapshotTimer = null;
    postUiDebugSnapshot(source);
  }, 1500);
}

function summarizeGraphForDebug(graph?: Preset["graph"] | null): Record<string, unknown> {
  const nodes = graph?.nodes ?? [];
  const edges = graph?.edges ?? [];
  return {
    nodeCount: nodes.length,
    edgeCount: edges.length,
    nodes: nodes.map((node) => ({ id: node.id, type: node.type })),
    edges: edges.map((edge) => ({ from: edge.from, to: edge.to })),
  };
}

function summarizePresetForDebug(preset?: Preset | null): Record<string, unknown> | null {
  if (!preset) {
    return null;
  }

  return {
    id: preset.id,
    name: preset.name,
    graph: summarizeGraphForDebug(preset.graph),
    scenes: (preset.scenes ?? []).map((scene) => ({
      id: scene.id,
      title: scene.title,
      graph: summarizeGraphForDebug(scene.graph),
    })),
  };
}

window.SoundshedDebug = {
  captureSnapshot(reason = "manual"): Record<string, unknown> {
    return postUiDebugSnapshot(reason);
  },
  getUiSnapshot(reason = "manual"): Record<string, unknown> {
    return buildUiDebugSnapshot(reason);
  },
  getPresetSummary(): Record<string, unknown> {
    const activePresetId = uiState.activePresetId ?? null;
    const activePresetForRender = getActivePresetForRender();
    const cachedActivePreset = activePresetId ? (uiState.presetCache.get(activePresetId) ?? null) : null;
    return {
      activePresetId,
      activePresetSceneId: uiState.activePresetSceneId ?? null,
      activePresetIsNew: uiState.activePresetIsNew,
      presetDirty: uiState.presetDirty,
      activePresetForRender: summarizePresetForDebug(activePresetForRender),
      activePresetDraft: summarizePresetForDebug(uiState.activePresetDraft),
      activePresetSnapshot: summarizePresetForDebug(uiState.activePresetSnapshot),
      cachedActivePreset: summarizePresetForDebug(cachedActivePreset),
    };
  },
};

type SignalPathNodeConfigUpdateOptions = {
  markDirty?: boolean;
};

function applySignalPathNodeConfigUpdate(
  nodeId: string,
  key: string,
  value: string | undefined,
  valueLength?: number,
  options: SignalPathNodeConfigUpdateOptions = {},
): boolean {
  const preset = getActivePresetForRender();
  if (!preset) {
    return false;
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
    return false;
  }

  setActivePresetDraft(preset);
  if (options.markDirty !== false) {
    setPresetDirty(true);
  }
  refreshSelectedNodeParams();
  renderSignalPathBar();
  return true;
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

/** A decoded message from the backend. Shape depends on `type`. */
type IncomingPayload = Record<string, unknown>;

type MessageHandler = (payload: IncomingPayload) => void;

function onState(payload: IncomingPayload): void {
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
    uiState.appSettings = appSettings as AppSettings;
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
    if (!shouldIgnoreStatePreset(preset)) {
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
    } else {
      appendLog(`state preset ignored ← ${preset.name ?? preset.id ?? "unknown"} (stale post-save state)`);
    }
  }
  renderActivePreset();
  refreshPerformancePads();
  syncControlsFromState();
  updatePresetActionButtons();
  updatePresetDropdownSelection();
  showNotification("");
  refreshSettingsView();
}

function onMetronomeState(payload: IncomingPayload): void {
  applyMetronomeState(payload as Record<string, unknown>);
}

function onMetronomeBeat(payload: IncomingPayload): void {
  const beat = payload as { beatIndex?: number; beatsPerBar?: number; level?: string };
  if (typeof beat.beatIndex !== "number") return;
  applyMetronomeBeat(beat.beatIndex);
}

function onRiffCaptureProgress(payload: IncomingPayload): void {
  applyRiffCaptureProgress(
    (payload as { capturedSamples?: number }).capturedSamples ?? 0,
    Array.isArray((payload as { waveformPeaks?: unknown[] }).waveformPeaks)
      ? ((payload as { waveformPeaks?: unknown[] }).waveformPeaks as unknown[])
          .filter((value): value is number => typeof value === "number")
      : [],
  );
}

function onRiffCaptureStarted(payload: IncomingPayload): void {
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
}

function onRiffCaptureStopped(payload: IncomingPayload): void {
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
}

function onRiffCaptureCanceled(payload: IncomingPayload): void {
  appendLog(`riff capture cancelled ← ${(payload as { takeId?: string }).takeId ?? "take"}`);
  applyRiffCaptureState({ active: false, complete: false, takeId: "", capturedSamples: 0, sampleRate: 0, hasAudio: false, waveformPeaks: [] });
  showNotification("Riff capture canceled");
}

function onRiffSaved(payload: IncomingPayload): void {
  appendLog(`riff saved ← ${(payload as { riffId?: string }).riffId ?? "riff"}`);
  const riffLibrary = (payload as { library?: RiffLibrary }).library;
  if (riffLibrary) {
    applyRiffLibraryState(riffLibrary);
  }
  showNotification("Riff saved", (payload as { path?: string }).path ?? "");
  if (!riffLibrary) {
    getRiffLibrary();
  }
  refreshDemoAudioSelectors();
}

function onPracticeToolFileLoaded(payload: IncomingPayload): void {
  const info = payload as { path?: string; title?: string; durationSec?: number; waveformPeaksL?: unknown[]; waveformPeaksR?: unknown[] };
  applyPracticeToolFileLoaded(info);
}

function onPracticeToolTransportState(payload: IncomingPayload): void {
  const info = payload as { state?: string; positionSec?: number };
  applyPracticeToolTransportState(info);
}

function onPracticeToolPlaybackEnded(): void {
  applyPracticeToolPlaybackEnded();
}

function onResourceCleanupResult(payload: IncomingPayload): void {
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
}

function onPresetLoaded(payload: IncomingPayload): void {
  const preset = (payload as { preset?: Preset }).preset;
  if (preset) {
    // Clear loading state before re-rendering — the re-render below removes
    // all loading classes and overlays baked into the DOM by the render functions.
    uiState.presetLoadingId = null;
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
  refreshPerformancePads();
  syncControlsFromState();
  updatePresetActionButtons();
}

function onSignalPathTestResult(payload: IncomingPayload): void {
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
}

function onPreviewStarted(payload: IncomingPayload): void {
  appendLog(`preview started ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
  handleRiffPreviewPlayback("start", (payload as { id?: string }).id ?? "");
  syncDemoAudioSelectionFromPreview((payload as { id?: string }).id ?? null);
  onDemoAudioStarted();
  showNotification("Playing demo audio", (payload as { title?: string }).title ?? "Demo");
}

function onPreviewComplete(payload: IncomingPayload): void {
  appendLog(`preview complete ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
  const previewId = (payload as { id?: string }).id ?? "";
  const savedRiffLooped = handleSavedRiffPreviewComplete(previewId);
  if (savedRiffLooped) {
    return;
  }
  handleRiffPreviewPlayback("stop", previewId);
  const capturedLooped = handleCapturedPreviewComplete(previewId);
  if (capturedLooped) {
    return;
  }
  onDemoAudioStopped();
  if (uiState.demoAudioRepeat) {
    void previewSelectedDemoAudio();
  } else {
    showNotification("Demo playback finished", (payload as { title?: string }).title ?? "Demo");
  }
}

function onPreviewStopped(payload: IncomingPayload): void {
  appendLog(`preview stopped ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
  handleRiffPreviewPlayback("stop", (payload as { id?: string }).id ?? "");
  onDemoAudioStopped();
  showNotification("Demo playback stopped", (payload as { title?: string }).title ?? "Demo");
}

function onDemoAudioRenderSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string; sampleRate?: number };
  const sampleRate = typeof info.sampleRate === "number" && info.sampleRate > 0
    ? `${Math.round(info.sampleRate / 100) / 10} kHz`
    : "";
  appendLog(`demo audio rendered ← ${info.path ?? "unknown"}${sampleRate ? ` @ ${sampleRate}` : ""}`);
  showNotification("Demo audio rendered", sampleRate ? `${sampleRate} - ${info.path ?? ""}` : info.path ?? "");
}

function onDemoAudioRenderFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  appendLog(`demo audio render failed ← ${info.message ?? "unknown"}`);
  showNotification("Demo audio render failed", info.message ?? "");
}

function onError(payload: IncomingPayload): void {
  console.error("Plugin error", payload);
  const requestId = (payload as { requestId?: string }).requestId;
  if (requestId) {
    rejectPendingPresetRequest(requestId, (payload as { message?: string }).message ?? "An error occurred", (payload as { detail?: string }).detail);
  }
  showNotification((payload as { message?: string }).message ?? "An error occurred", (payload as { detail?: string }).detail ?? "");
  if (uiState.presetLoadingId) {
    // A backend-driven load (e.g. a setlist step onto a missing preset) failed, so the
    // "presetLoaded" that would clear the loading state is never coming.
    uiState.presetLoadingId = null;
    renderActivePreset();
  }
}

function onModelLoaded(payload: IncomingPayload): void {
  appendLog(`model loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
  renderActivePreset();
  showNotification("Model loaded", (payload as { path?: string }).path ?? "");
}

function onIrLoaded(payload: IncomingPayload): void {
  console.log("[JS] IR loaded event received, path:", (payload as { path?: string }).path);
  appendLog(`IR loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
  renderActivePreset();
  showNotification("IR loaded", (payload as { path?: string }).path ?? "");
}

function onResourceImported(payload: IncomingPayload): void {
  const info = payload as { id?: string; name?: string; resourceType?: string; filePath?: string };
  upsertImportedResourceInUiState(info);
  appendLog(`resource imported ← ${info.name ?? "unknown"}`);
  showNotification("Resource imported", info.name ?? info.filePath ?? "");
  document.dispatchEvent(new CustomEvent("resource-browser:resource-imported", {
    detail: {
      id: info.id ?? "",
      name: info.name ?? "",
      resourceType: info.resourceType ?? "",
      filePath: info.filePath ?? "",
    },
  }));
}

function onResourceImportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string; detail?: string };
  appendLog(`resource import failed ← ${info.message ?? "unknown"}`);
  showNotification(info.message ?? "Import failed", info.detail ?? "");
}

function onResourceRemoved(payload: IncomingPayload): void {
  const info = payload as { id?: string; resourceType?: string };
  removeResourceFromUiState(info);
  appendLog(`resource removed ← ${info.id ?? "unknown"}`);
  document.dispatchEvent(new CustomEvent("resource-browser:resource-removed", {
    detail: {
      id: info.id ?? "",
      resourceType: info.resourceType ?? "",
    },
  }));
}

function onResourceDeleteFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string; detail?: string; presetName?: string };
  appendLog(`resource delete failed ← ${info.message ?? "unknown"}`);
  showNotification(info.message ?? "Resource delete failed", info.detail ?? info.presetName ?? "");
}

function onResourceUsageInfo(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:usage-info", { detail: payload }));
}

function onResourceFolderPicked(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:folder-picked", { detail: payload }));
}

function onResourceFolderListing(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:folder-listing", { detail: payload }));
}

function onResourceFolderMetadata(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:folder-metadata", { detail: payload }));
}

function onResourceFolderListingFailed(payload: IncomingPayload): void {
  const info = payload as { path?: string; message?: string };
  appendLog(`folder listing failed ← ${info.message ?? "unknown"}`);
  document.dispatchEvent(new CustomEvent("resource-browser:folder-listing-failed", { detail: payload }));
}

function onHostedPluginResourceLoadFailed(payload: IncomingPayload): void {
  handleHostedPluginResourceLoadFailed(payload as {
    nodeId?: string;
    resourceType?: string;
    resourceId?: string;
    filePath?: string;
    resourceIndex?: number;
    message?: string;
    errorCode?: string;
  });
}

function onHostedPluginResourceLoadCompleted(payload: IncomingPayload): void {
  handleHostedPluginResourceLoadCompleted(payload as {
    nodeId?: string;
    resourceType?: string;
  });
}

function onNodeResourceBrowseCancelled(payload: IncomingPayload): void {
  handleNodeResourceBrowseCancelled(payload as {
    nodeId?: string;
    resourceType?: string;
  });
}

function onToneSharingPackImported(payload: IncomingPayload): void {
  const info = payload as { fileName?: string; path?: string; byteSize?: number };
  const detail = info.path ?? info.fileName ?? "";
  appendLog(`tone sharing pack imported ← ${detail}`);
  registerInstalledToneSharingPackFromImport(info);
  showNotification("Pack imported", detail);
}

function onToneSharingPackImportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  appendLog(`tone sharing pack import failed ← ${info.message ?? "unknown"}`);
  showNotification("Pack import failed", info.message ?? "");
}

function onResourceData(payload: IncomingPayload): void {
  handleResourceDataMessage(payload as { requestId: string; data?: string; fileName?: string; message?: string });
}

function onResourceDataFailed(payload: IncomingPayload): void {
  handleResourceDataMessage(payload as { requestId: string; data?: string; fileName?: string; message?: string });
}

function onBlendExportSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string };
  showNotification("Blend exported", info.path ?? "");
}

function onBlendExportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  showNotification("Blend export failed", info.message ?? "");
}

function onLibraryExportSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string };
  showNotification("Library exported", info.path ?? "");
}

function onLibraryExportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  showNotification("Library export failed", info.message ?? "");
}

function onPresetExportSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string };
  showNotification("Preset exported", info.path ?? "");
}

function onPresetExportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  showNotification("Preset export failed", info.message ?? "");
}

function onPresetSaved(payload: IncomingPayload): void {
  const savedPreset = (payload as { preset?: Preset }).preset;
  appendLog(
    `preset saved ← ${savedPreset?.name ?? "unknown"} `
    + `(graphNodes=${savedPreset?.graph?.nodes?.length ?? 0}, scenes=${savedPreset?.scenes?.length ?? 0})`,
  );
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
    markIgnoreNextStatePreset(savedPreset.id);
  }
  showNotification("Preset saved", (payload as { path?: string }).path ?? savedPreset?.name ?? "");
}

function onPresetArchiveSessionStarted(payload: IncomingPayload): void {
  const sessionPayload = payload as { active?: boolean; archiveName?: string; archiveKey?: string; presetCount?: number };
  applyPresetArchiveSessionState({
    active: Boolean(sessionPayload.active),
    archiveName: sessionPayload.archiveName,
    archiveKey: sessionPayload.archiveKey,
    presetCount: sessionPayload.presetCount,
  });
  renderActivePreset();
  updatePresetActionButtons();
  showNotification("Preset archive session started", sessionPayload.archiveName ?? "");
}

function onPresetArchiveSessionEnded(): void {
  applyPresetArchiveSessionState({ active: false });
  renderActivePreset();
  updatePresetActionButtons();
  showNotification("Preset archive session ended");
}

function onPresetArchiveSessionFailed(payload: IncomingPayload): void {
  showNotification("Preset archive session failed", (payload as { message?: string }).message ?? "");
}

function onPresetList(payload: IncomingPayload): void {
  const presetListPayload = payload as { presets?: Array<{ id: string; name: string; category?: string; source?: string }> };
  if (Array.isArray(presetListPayload.presets)) {
    appendLog(`preset list received ← ${presetListPayload.presets.length} presets`);
    const cachedBeforeUpdate = new Set(uiState.presetCache.keys());
    const nextPresets: Preset[] = [];
    for (const p of presetListPayload.presets) {
      const incomingCategory = p.category ?? "Factory";
      const existingCached = uiState.presetCache.get(p.id);
      const nextPreset: Preset = existingCached
        ? {
            ...existingCached,
            name: p.name,
            category: incomingCategory,
          }
        : { id: p.id, name: p.name, category: incomingCategory } as Preset;
      uiState.presetCache.set(p.id, nextPreset);
      nextPresets.push(nextPreset);
    }
    uiState.presets = nextPresets;
    uiState.filteredPresets = nextPresets.slice();
    populatePresetDropdown();
    renderActivePreset();

    if (pendingSharedPresetHydration) {
      pendingSharedPresetHydration = false;
      presetListPayload.presets.forEach((presetSummary) => {
        if (cachedBeforeUpdate.has(presetSummary.id)) {
          refreshPresetCacheEntryFromBackend(presetSummary.id);
        }
      });
    }
  }
}

function onAppInfo(payload: IncomingPayload): void {
  const infoPayload = payload as { version?: string; os?: string; cpu?: string };
  applyEnvironmentState({
    standalone: uiState.environment?.standalone ?? false,
    version: infoPayload.version ?? uiState.environment?.version,
    os: infoPayload.os ?? uiState.environment?.os,
    cpu: infoPayload.cpu ?? uiState.environment?.cpu,
  });
  refreshSettingsView();
}

function onSharedSyncUpdated(): void {
  pendingSharedPresetHydration = true;
  scheduleSharedSyncRefresh();
}

function onSharedSyncState(payload: IncomingPayload): void {
  pendingSharedPresetHydration = true;
  const sharedPayload = payload as {
    appSettings?: Record<string, unknown>;
    uiSettings?: UiSettings;
    resourceLibrary?: Record<string, unknown[]>;
    blendLibrary?: unknown[];
    customEffectLibrary?: unknown[];
    presetArchiveSession?: PresetArchiveSessionState | null;
  };

  if (sharedPayload.appSettings) {
    uiState.appSettings = sharedPayload.appSettings as AppSettings;
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

function onPresetData(payload: IncomingPayload): void {
  const presetPayload = payload as { preset?: Preset };
  if (presetPayload.preset) {
    migratePresetNodeTypes(presetPayload.preset);
    normalizePresetResources(presetPayload.preset);
    normalizePresetScenes(presetPayload.preset);
    handlePresetDataMessage(presetPayload.preset, (payload as { requestId?: string }).requestId);
  }
}

function onPresetFolders(payload: IncomingPayload): void {
  const foldersPayload = payload as { folders?: PresetFolder[]; activeFolderId?: string | null };
  applyPresetFoldersFromBackend(foldersPayload.folders ?? [], foldersPayload.activeFolderId ?? null);
}

function onPresetFavorites(payload: IncomingPayload): void {
  const favoritesPayload = payload as { favorites?: string[] };
  applyPresetFavoritesFromBackend(Array.isArray(favoritesPayload.favorites) ? favoritesPayload.favorites : []);
}

function onPresetRatings(payload: IncomingPayload): void {
  const ratingsPayload = payload as { ratings?: Record<string, number> };
  applyPresetRatingsFromBackend(ratingsPayload.ratings ?? {});
}

function onSetlists(payload: IncomingPayload): void {
  const setlistsPayload = payload as { setlists?: Setlist[]; activeSetlistId?: string | null };
  applySetlistsFromBackend(setlistsPayload.setlists ?? [], setlistsPayload.activeSetlistId ?? null);
  refreshPerformancePads();
}

function onEffectPresets(payload: IncomingPayload): void {
  const effectPresetsPayload = payload as { byEffectType?: Record<string, StoredEffectPreset[]> };
  uiState.effectPresets = effectPresetsPayload.byEffectType ?? {};
  // The backend re-broadcasts after each save/delete, so both the params panel
  // and an open presets flyout need to pick up the new list.
  refreshSelectedNodeParams();
  refreshEffectPresetsFlyout();
}

function onSetlistCursorChanged(payload: IncomingPayload): void {
  const cursorPayload = payload as { cursorIndex?: number; presetId?: string; activeSetlistId?: string };
  if (typeof cursorPayload.cursorIndex === "number") {
    applySetlistCursorFromBackend(cursorPayload.cursorIndex, cursorPayload.presetId, cursorPayload.activeSetlistId);
    refreshPerformancePads();
  }
}

function onAutomation(payload: IncomingPayload): void {
  const autoPayload = payload as {
    slots?: AutomationSlot[];
    registry?: AutomationRegistryEntry[];
    maxCustomSlots?: number;
  };
  applyAutomationState({
    slots: autoPayload.slots ?? [],
    registry: autoPayload.registry ?? [],
    maxCustomSlots: autoPayload.maxCustomSlots ?? 16,
  });
}

function onMidiLog(payload: IncomingPayload): void {
  const logPayload = payload as { midiType?: string; channel?: number; data1?: number; data2?: number };
  handleMidiLogEntry({
    type: logPayload.midiType ?? "Unknown",
    channel: logPayload.channel ?? 0,
    data1: logPayload.data1 ?? 0,
    data2: logPayload.data2 ?? 0,
  });
}

function onMidiLearnCapture(payload: IncomingPayload): void {
  const capturePayload = payload as { slotId?: string };
  if (typeof capturePayload.slotId === "string") {
    handleMidiLearnCapture(capturePayload.slotId);
  }
}

function onTheme(payload: IncomingPayload): void {
  const themePayload = payload as { theme?: string };
  const theme = themePayload.theme === "light" || themePayload.theme === "classic" ? themePayload.theme : "dark";
  themeSwitcher.applyTheme(theme);
}

function onTunerUpdate(payload: IncomingPayload): void {
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
}

function onTunerStarted(payload: IncomingPayload): void {
  const startPayload = payload as { referenceFrequency?: number; liveMode?: boolean };
  handleTunerStarted(startPayload.referenceFrequency ?? 440.0);
  if (startPayload.liveMode !== undefined) {
    handleTunerLiveModeChanged(startPayload.liveMode);
  }
}

function onTunerStopped(): void {
  handleTunerStopped();
}

function onTunerReferenceChanged(payload: IncomingPayload): void {
  handleTunerReferenceChanged((payload as { referenceFrequency?: number }).referenceFrequency ?? 440.0);
}

function onTunerLiveModeChanged(payload: IncomingPayload): void {
  handleTunerLiveModeChanged((payload as { liveMode?: boolean }).liveMode ?? true);
}

function onDebug(payload: IncomingPayload): void {
  const msg = (payload as { message?: string }).message ?? "";
  console.log("[C++]", msg);
  appendLog(`[C++] ${msg}`);
}

function onCaptureDebugSnapshot(payload: IncomingPayload): void {
  const source = typeof (payload as { source?: string }).source === "string"
    ? (payload as { source?: string }).source as string
    : "backend-request";
  postUiDebugSnapshot(source);
}

function onDebugSnapshotWritten(payload: IncomingPayload): void {
  const info = payload as { path?: string; source?: string };
  console.log("[DebugSnapshot] written", info.path ?? "", info.source ?? "");
  if (info.source === "footer-button") {
    appendLog(`debug snapshot written ← ${info.path ?? "unknown path"}`);
    showNotification("Debug state captured", info.path ?? "logs/debug-state.json");
  }
}

function onInputModeChanged(payload: IncomingPayload): void {
  const modePayload = payload as { monoMode?: boolean; inputChannel?: number };
  handleInputModeChanged(
    modePayload.monoMode ?? true,
    modePayload.inputChannel ?? 1
  );
  appendLog(`Input mode changed: ${modePayload.monoMode ? "Mono" : "Stereo"}, Channel: ${(modePayload.inputChannel ?? 1) + 1}`);
}

function onAmpCabStateChanged(payload: IncomingPayload): void {
  const statePayload = payload as { ampEnabled?: boolean; cabEnabled?: boolean };
  handleAmpCabStateChanged(
    statePayload.ampEnabled ?? true,
    statePayload.cabEnabled ?? true
  );
  appendLog(`Amp: ${statePayload.ampEnabled ? "ON" : "OFF"}, Cab: ${statePayload.cabEnabled ? "ON" : "OFF"}`);
}

function onAutoLevelChanged(payload: IncomingPayload): void {
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
}

function onUiSettingsChanged(payload: IncomingPayload): void {
  const uiSettings = (payload as { settings?: UiSettings }).settings;
  if (uiSettings) {
    uiState.uiSettings = uiSettings;
    applyUiSettings(uiSettings);
    applyPresetRecentsFromAppSettings();
  }
}

function onDspPerformance(payload: IncomingPayload): void {
  const stats = payload as {
    stats?: DSPPerformanceStats;
    sampleRate?: number;
    blockSize?: number;
  };
  if (stats.stats) {
    const mergedStats: DSPPerformanceStats = {
      ...stats.stats,
      sampleRate: stats.sampleRate ?? stats.stats.sampleRate,
      blockSize: stats.blockSize ?? stats.stats.blockSize,
    };
    uiState.dspPerformance = mergedStats;
    recordDspLoadSample(mergedStats.dspLoadPercent);
    queueTelemetryUiUpdate("dsp");
  }
}

function onSldRoster(payload: IncomingPayload): void {
  applySignalDiagnosticsRoster(payload as unknown as SignalDiagnosticsRoster);
}

function onSld(payload: IncomingPayload): void {
  if (applySignalDiagnosticsFrame(payload as unknown as SignalDiagnosticsFrame)) {
    queueTelemetryUiUpdate("signalDiagnostics");
  }
}

function onSldA(payload: IncomingPayload): void {
  if (applySignalDiagnosticsAnalyzer(payload as unknown as SignalDiagnosticsAnalyzerFrame)) {
    queueTelemetryUiUpdate("signalDiagnostics");
  }
}

function onSpatialPosition(payload: IncomingPayload): void {
  // Live source position from the spatialiser's motion engine, so the on-screen
  // puck matches what is being heard. Purely cosmetic: if it never arrives, the
  // widget just shows the anchor position instead.
  const nodes = payload.nodes;
  if (Array.isArray(nodes)) {
    applySpatialPositionUpdate(nodes as never);
  }
}

function onSignalPathNodeConfigUpdated(payload: IncomingPayload): void {
  const update = payload as {
    nodeId?: string;
    key?: string;
    value?: string;
    valueLength?: number;
    captured?: boolean;
    dirty?: boolean;
    persist?: boolean;
    silent?: boolean;
  };
  if (typeof update.nodeId === "string" && typeof update.key === "string") {
    const markDirty = shouldMarkSignalPathNodeConfigUpdateDirty(update);
    let applied = false;
    if (typeof update.value === "string") {
      applied = applySignalPathNodeConfigUpdate(update.nodeId, update.key, update.value, update.valueLength, { markDirty });
    } else if (update.captured && update.key === "pluginStateBase64") {
      applied = applySignalPathNodeConfigUpdate(update.nodeId, update.key, undefined, update.valueLength, { markDirty });
    }
    if (!applied && markDirty) {
      setPresetDirty(true);
    }
    if (update.key === "pluginStateBase64" && !update.silent) {
      showNotification("Plugin state captured", "success");
    }
  }
}

function onSignalPathNodeParamUpdated(payload: IncomingPayload): void {
  const update = payload as { nodeId?: string; key?: string; value?: number };
  if (typeof update.nodeId === "string" && typeof update.key === "string" && typeof update.value === "number") {
    const preset = getActivePresetForRender();
    if (preset) {
      try {
        setNodeParam(preset, update.nodeId, update.key, update.value);
      } catch {
        // Node not in the active preset draft — ignore silently.
      }
    }
    refreshSelectedNodeParams();
  }
}

function onSignalPathNodeBypassUpdated(payload: IncomingPayload): void {
  const update = payload as { nodeId?: string; bypassed?: boolean };
  if (typeof update.nodeId === "string" && typeof update.bypassed === "boolean") {
    const preset = getActivePresetForRender();
    const node = preset?.graph?.nodes?.find((n) => n.id === update.nodeId);
    if (node) {
      (node as unknown as { bypassed?: boolean }).bypassed = update.bypassed;
      (node as unknown as { enabled?: boolean }).enabled = !update.bypassed;
    }
    refreshSelectedNodeParams();
    renderActivePreset();
  }
}

function onGlobalSignalChainChanged(payload: IncomingPayload): void {
  const chainPayload = payload as { config?: GlobalSignalChainConfig; globalSignalChain?: GlobalSignalChainConfig };
  const chainConfig = chainPayload.config ?? chainPayload.globalSignalChain;
  if (chainConfig) {
    uiState.globalSignalChain = normalizeGlobalSignalChain(chainConfig) ?? uiState.globalSignalChain;
    appendLog("Global signal chain configuration loaded");
    syncControlsFromState();
  }
}

function onLayoutLibraryLoaded(payload: IncomingPayload): void {
  const libraryPayload = payload as { layoutLibrary?: LayoutLibrary };
  if (libraryPayload.layoutLibrary) {
    // Images are no longer shipped in this payload (loaded on demand). Preserve any
    // images we already received so an open designer keeps its backgrounds, then
    // refresh them if they had been loaded (picks up additions/removals).
    const previousImages = uiState.layoutLibrary?.images ?? [];
    uiState.layoutLibrary = libraryPayload.layoutLibrary;
    if ((!uiState.layoutLibrary.images || uiState.layoutLibrary.images.length === 0) && previousImages.length) {
      uiState.layoutLibrary.images = previousImages;
    }
    if (areLayoutImagesLoaded()) {
      ensureLayoutImagesLoaded(true);
    }
    appendLog("Layout library loaded");
    renderLayoutList();
    // Ensure any open node params panel picks up the updated layout mapping.
    renderActivePreset();
  }
}

function onLayoutSaved(payload: IncomingPayload): void {
  const savePayload = payload as { effectType?: string; blendId?: string; layoutId?: string; lookupKey?: string };
  const displayKey = savePayload.blendId ? `${savePayload.effectType} (blend: ${savePayload.blendId})` : savePayload.effectType;
  appendLog(`Layout saved for ${displayKey ?? "effect"}${savePayload.layoutId ? ` (${savePayload.layoutId})` : ""}`);
  showNotification("Layout saved", "success");
  // layoutLibraryLoaded will follow and trigger a full refresh.
}

function onLayoutImagesLoaded(payload: IncomingPayload): void {
  const imagesPayload = payload as { images?: LayoutImageRef[] };
  const images = Array.isArray(imagesPayload.images) ? imagesPayload.images : [];
  if (!uiState.layoutLibrary) {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  }
  uiState.layoutLibrary.images = images;
  markLayoutImagesLoaded();
  appendLog(`Layout images loaded (${images.length})`);
  renderLayoutList();
  // Re-render the designer canvas so backgrounds appear once images arrive.
  layoutDesigner.notifyImagesLoaded();
  // Refresh the live signal-path node params panel so custom-layout backgrounds resolve.
  refreshSelectedNodeParams();
}

function onLayoutImageSelected(payload: IncomingPayload): void {
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
}

function onLayoutExportSaved(payload: IncomingPayload): void {
  const exportPayload = payload as { path?: string };
  if (exportPayload.path) {
    showNotification(`Layout exported to ${exportPayload.path}`, "success");
    appendLog(`Layout exported: ${exportPayload.path}`);
  }
}

function onLayoutExportFailed(payload: IncomingPayload): void {
  const failPayload = payload as { message?: string };
  showNotification(failPayload.message ?? "Layout export failed", "error");
  appendLog(`Layout export failed: ${failPayload.message ?? "unknown error"}`);
}

function onCompositeLibrary(payload: IncomingPayload): void {
  const compPayload = payload as { definitions?: CompositeEffectDefinition[] };
  if (compPayload.definitions) {
    handleCompositeLibrary(compPayload.definitions);
    refreshFxSelector();
  }
}

function onCustomEffectLibrary(payload: IncomingPayload): void {
  const customPayload = payload as { entries?: CustomEffectLibrary };
  if (Array.isArray(customPayload.entries)) {
    handleCustomEffectLibrary(customPayload.entries);
    refreshFxSelector();
  }
}

function onCustomEffectSaved(payload: IncomingPayload): void {
  const customPayload = payload as { name?: string; applyToNode?: boolean };
  const detail = customPayload.name ?? "Custom Effect";
  appendLog(`custom effect saved ← ${detail}`);
  showNotification(customPayload.applyToNode ? "Custom Effect applied" : "Custom Effect saved", detail);
}

function onGeneratedCustomEffectBundleExportSaved(payload: IncomingPayload): void {
  const exportPayload = payload as { path?: string };
  showNotification("Custom Effect bundle exported", exportPayload.path ?? "");
}

function onGeneratedCustomEffectBundleExportFailed(payload: IncomingPayload): void {
  const exportPayload = payload as { message?: string };
  showNotification("Custom Effect bundle export failed", exportPayload.message ?? "");
}

function onEffectCatalog(payload: IncomingPayload): void {
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
        presets?: unknown;
        requiresResource?: unknown;
        resourceType?: unknown;
        exposedResources?: unknown;
      };
      const type = typeof effect.type === "string" ? effect.type : "";
      if (!type) {
        continue;
      }
      const existing = EffectTypeRegistry.get(type);
      const rawName = typeof effect.name === "string" ? effect.name.trim() : "";
      const rawCategory = typeof effect.category === "string" ? effect.category.trim() : "";
      const displayName = rawName || existing?.displayName || type;
      const category = rawCategory || existing?.category || "utility";
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
      const presets = Array.isArray(effect.presets)
        ? effect.presets
            .filter((preset) => preset && typeof preset === "object")
            .flatMap((preset) => {
              const p = preset as {
                id?: unknown;
                name?: unknown;
                source?: unknown;
                parameters?: unknown;
                parameterOrder?: unknown;
              };
              if (typeof p.id !== "string" || !p.id || typeof p.name !== "string") {
                return [];
              }
              const parameters = p.parameters && typeof p.parameters === "object" && !Array.isArray(p.parameters)
                ? Object.fromEntries(
                    Object.entries(p.parameters)
                      .filter(([, value]) => typeof value === "number" && Number.isFinite(value)),
                  )
                : {};
              return [{
                id: p.id,
                name: p.name,
                source: p.source === "custom" ? "custom" as const : "factory" as const,
                parameters,
                parameterOrder: Array.isArray(p.parameterOrder)
                  ? p.parameterOrder.filter((key): key is string => typeof key === "string" && key in parameters)
                  : undefined,
              }];
            })
        : existing?.presets;

      EffectTypeRegistry.register(type, {
        type,
        displayName,
        category,
        catalogHidden: existing?.catalogHidden ?? (type === EffectGuids.kAmpNam || type === EffectGuids.kAmpNamBlend),
        requiresResource,
        resourceType,
        parameters,
        presets,
        exposedResources,
      });
    }
    refreshFxSelector();
    if (getActivePresetForRender()) {
      renderActivePreset();
      refreshSelectedNodeParams();
    }
  }
}

function onCompositeDefinitionAdded(payload: IncomingPayload): void {
  const compAddPayload = payload as { definition?: CompositeEffectDefinition };
  if (compAddPayload.definition) {
    handleCompositeDefinitionAdded(compAddPayload.definition);
    refreshFxSelector();
    renderCompositeList();
  }
}

function onCompositeDefinitionRemoved(payload: IncomingPayload): void {
  const compRemovePayload = payload as { id?: string };
  if (compRemovePayload.id) {
    handleCompositeDefinitionRemoved(compRemovePayload.id);
    refreshFxSelector();
    renderCompositeList();
  }
}

function onCompositeEditState(payload: IncomingPayload): void {
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
}

function onCompositeEditModeExited(): void {
  // C++ confirms we've left composite edit mode
  exitCompositeEditState();
  handleCompositeEditModeExited();
  renderSignalPathBar();
}

function onCompositePresetList(payload: IncomingPayload): void {
  const list = (payload as { compositePresets?: CompositePreset[] }).compositePresets;
  if (Array.isArray(list)) {
    handleCompositePresetList(list);
  }
}

function onCompositePresetSaved(payload: IncomingPayload): void {
  const saved = payload as { id?: string; name?: string };
  handleCompositePresetSaved(saved.id ?? "", saved.name ?? "");
}

function onCompositePresetLoaded(payload: IncomingPayload): void {
  const loaded = payload as { id?: string; name?: string };
  handleCompositePresetLoaded(loaded.id ?? "", loaded.name ?? "");
}

function onNavigateToToneSharingDeepLink(payload: IncomingPayload): void {
  const deepLink = payload as { deepLink?: string };
  if (deepLink.deepLink) {
    handleToneSharingDeepLink(deepLink.deepLink);
  }
}

/**
 * Every message the backend can send, and what handles it.
 *
 * This replaced a single 1,451-line `switch (type)`. Each arm is now an
 * independently readable — and independently testable — function.
 */
const MESSAGE_HANDLERS: Record<string, MessageHandler> = {
  "state": onState,
  "metronomeState": onMetronomeState,
  "metronomeBeat": onMetronomeBeat,
  "riffCaptureProgress": onRiffCaptureProgress,
  "riffCaptureStarted": onRiffCaptureStarted,
  "riffCaptureStopped": onRiffCaptureStopped,
  "riffCaptureCanceled": onRiffCaptureCanceled,
  "riffSaved": onRiffSaved,
  "practiceToolFileLoaded": onPracticeToolFileLoaded,
  "practiceToolTransportState": onPracticeToolTransportState,
  "practiceToolPlaybackEnded": onPracticeToolPlaybackEnded,
  "resourceCleanupResult": onResourceCleanupResult,
  "presetLoaded": onPresetLoaded,
  "signalPathTestResult": onSignalPathTestResult,
  "previewStarted": onPreviewStarted,
  "previewComplete": onPreviewComplete,
  "previewStopped": onPreviewStopped,
  "demoAudioRenderSaved": onDemoAudioRenderSaved,
  "demoAudioRenderFailed": onDemoAudioRenderFailed,
  "error": onError,
  "modelLoaded": onModelLoaded,
  "irLoaded": onIrLoaded,
  "resourceImported": onResourceImported,
  "resourceImportFailed": onResourceImportFailed,
  "resourceRemoved": onResourceRemoved,
  "resourceDeleteFailed": onResourceDeleteFailed,
  "resourceUsageInfo": onResourceUsageInfo,
  "resourceFolderPicked": onResourceFolderPicked,
  "resourceFolderListing": onResourceFolderListing,
  "resourceFolderMetadata": onResourceFolderMetadata,
  "resourceFolderListingFailed": onResourceFolderListingFailed,
  "hostedPluginResourceLoadFailed": onHostedPluginResourceLoadFailed,
  "hostedPluginResourceLoadCompleted": onHostedPluginResourceLoadCompleted,
  "nodeResourceBrowseCancelled": onNodeResourceBrowseCancelled,
  "toneSharingPackImported": onToneSharingPackImported,
  "toneSharingPackImportFailed": onToneSharingPackImportFailed,
  "resourceData": onResourceData,
  "resourceDataFailed": onResourceDataFailed,
  "blendExportSaved": onBlendExportSaved,
  "blendExportFailed": onBlendExportFailed,
  "libraryExportSaved": onLibraryExportSaved,
  "libraryExportFailed": onLibraryExportFailed,
  "presetExportSaved": onPresetExportSaved,
  "presetExportFailed": onPresetExportFailed,
  "presetSaved": onPresetSaved,
  "presetArchiveSessionStarted": onPresetArchiveSessionStarted,
  "presetArchiveSessionEnded": onPresetArchiveSessionEnded,
  "presetArchiveSessionFailed": onPresetArchiveSessionFailed,
  "presetList": onPresetList,
  "appInfo": onAppInfo,
  "sharedSyncUpdated": onSharedSyncUpdated,
  "sharedSyncState": onSharedSyncState,
  "presetData": onPresetData,
  "presetFolders": onPresetFolders,
  "presetFavorites": onPresetFavorites,
  "presetRatings": onPresetRatings,
  "setlists": onSetlists,
  "effectPresets": onEffectPresets,
  "setlistCursorChanged": onSetlistCursorChanged,
  "automation": onAutomation,
  "midiLog": onMidiLog,
  "midiLearnCapture": onMidiLearnCapture,
  "theme": onTheme,
  "tunerUpdate": onTunerUpdate,
  "tunerStarted": onTunerStarted,
  "tunerStopped": onTunerStopped,
  "tunerReferenceChanged": onTunerReferenceChanged,
  "tunerLiveModeChanged": onTunerLiveModeChanged,
  "debug": onDebug,
  "captureDebugSnapshot": onCaptureDebugSnapshot,
  "debugSnapshotWritten": onDebugSnapshotWritten,
  "inputModeChanged": onInputModeChanged,
  "ampCabStateChanged": onAmpCabStateChanged,
  "autoLevelChanged": onAutoLevelChanged,
  "uiSettingsChanged": onUiSettingsChanged,
  "dspPerformance": onDspPerformance,
  "sldRoster": onSldRoster,
  "sld": onSld,
  "sldA": onSldA,
  "spatialPosition": onSpatialPosition,
  "signalPathNodeConfigUpdated": onSignalPathNodeConfigUpdated,
  "signalPathNodeParamUpdated": onSignalPathNodeParamUpdated,
  "signalPathNodeBypassUpdated": onSignalPathNodeBypassUpdated,
  "globalSignalChainChanged": onGlobalSignalChainChanged,
  "globalChain": onGlobalSignalChainChanged,
  "layoutLibraryLoaded": onLayoutLibraryLoaded,
  "layoutSaved": onLayoutSaved,
  "layoutImagesLoaded": onLayoutImagesLoaded,
  "layoutImageSelected": onLayoutImageSelected,
  "layoutExportSaved": onLayoutExportSaved,
  "layoutExportFailed": onLayoutExportFailed,
  "compositeLibrary": onCompositeLibrary,
  "customEffectLibrary": onCustomEffectLibrary,
  "customEffectSaved": onCustomEffectSaved,
  "generatedCustomEffectBundleExportSaved": onGeneratedCustomEffectBundleExportSaved,
  "generatedCustomEffectBundleExportFailed": onGeneratedCustomEffectBundleExportFailed,
  "effectCatalog": onEffectCatalog,
  "compositeDefinitionAdded": onCompositeDefinitionAdded,
  "compositeDefinitionRemoved": onCompositeDefinitionRemoved,
  "compositeEditState": onCompositeEditState,
  "compositeEditModeExited": onCompositeEditModeExited,
  "compositePresetList": onCompositePresetList,
  "compositePresetSaved": onCompositePresetSaved,
  "compositePresetLoaded": onCompositePresetLoaded,
  "navigateToToneSharingDeepLink": onNavigateToToneSharingDeepLink,
};

export function handleIncomingMessage(message: string): void {
  const payload = JSON.parse(message) as Record<string, unknown>;
  const type = typeof payload.type === "string" ? payload.type : "";
  // Frequent diagnostics messages; avoid spamming console.
  if (type !== "dspPerformance" && type !== "sld" && type !== "sldA" && type !== "sldRoster" && type !== "spatialPosition") {
    console.log("[JS] handleIncomingMessage received:", message.substring(0, 200));
    console.log("[JS] Parsed message type:", type);
  }

  const handler = MESSAGE_HANDLERS[type];
  if (handler) {
    handler(payload);
  } else {
    console.warn("Unknown message type", payload.type);
  }

  // Building this snapshot serializes the whole of uiState — resource library, preset cache
  // and all — which is tens of MB, on the main thread, and then ships it over the bridge for
  // the backend to write to disk. It is a diagnostic for the Debug State Capture feature, so
  // it must not run at all unless that feature is switched on.
  if (isFeatureEnabled(Features.DebugStateCapture) && !DEBUG_SNAPSHOT_SKIP_TYPES.has(type)) {
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
