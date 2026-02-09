import { uiState, clonePreset, getActivePresetForRender, setActivePresetDraft, setActivePresetSnapshot, setPresetDirty } from "./state.js";
import { renderActivePreset, applyPresetFromLibrary, populatePresetDropdown, updatePresetDropdownSelection, savePresetToLocalStorage, updatePresetActionButtons } from "./presets.js";
import { syncControlsFromState, handleInputModeChanged, handleAmpCabStateChanged, syncAutoLevelControlsFromState, applyStoredInputChannel } from "./controls.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { previewSelectedDemoAudio, onDemoAudioStarted, onDemoAudioStopped } from "./demoAudio.js";
import { handleTunerUpdate, handleTunerStarted, handleTunerStopped, handleTunerReferenceChanged, handleTunerLiveModeChanged } from "./tuner.js";
import { applyUiSettings } from "./windowSettings.js";
import { updateDSPPerformancePlot, updateSignalDiagnosticsView } from "./views.js";
import { refreshSettingsView } from "./settings.js";
import { refreshSelectedNodeParams, renderSignalPathBar } from "./signalPath.js";
import { refreshFxSelector } from "./fxSelector.js";
import { applyEnvironmentState, applyMetronomeState } from "./metronome.js";
import type { GlobalSignalChainConfig, Preset, ResourceRef, UiSettings } from "./types.js";
import { handleResourceDataMessage } from "./archiveUtils.js";
import { layoutDesigner } from "./layoutDesigner.js";
import type { LayoutLibrary, EffectLayout } from "./layoutTypes.js";
import { layoutLookupKey } from "./layoutTypes.js";
import { handleCompositeLibrary, handleCompositeDefinitionAdded, handleCompositeDefinitionRemoved } from "./compositeEffects.js";
import type { CompositeEffectDefinition } from "./compositeTypes.js";
import { renderCompositeList, handleCompositeEditModeExited, handleCompositeEditStateUpdate } from "./compositeEditor.js";
import { renderLayoutList } from "./layoutManager.js";
import { enterCompositeEditState, updateCompositeEditState, exitCompositeEditState } from "./state.js";

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
  if (!preset?.graph?.nodes) return;
  preset.graph.nodes.forEach((node) => {
    if (Array.isArray(node.resources)) {
      node.resources.forEach((ref) => normalizeResourceRef(ref));
    }
  });
}

function presetSignature(preset?: Preset | null): string {
  return preset ? JSON.stringify(preset) : "";
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
        applyStoredInputChannel();
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
      const environment = (payload as { environment?: { standalone?: boolean } }).environment;
      if (environment) {
        applyEnvironmentState({ standalone: Boolean(environment.standalone) });
      }
      const metronome = (payload as { metronome?: { bpm?: number; enabled?: boolean; editable?: boolean; source?: string; volumeDb?: number; pan?: number; clickType?: string; clickTypes?: Array<{ id?: string; label?: string }> } }).metronome;
      if (metronome) {
        applyMetronomeState({
          bpm: typeof metronome.bpm === "number" ? metronome.bpm : uiState.metronome?.bpm ?? 120,
          enabled: Boolean(metronome.enabled),
          editable: metronome.editable !== undefined ? Boolean(metronome.editable) : true,
          source: metronome.source === "host" ? "host" : "app",
          volumeDb: typeof metronome.volumeDb === "number" ? metronome.volumeDb : uiState.metronome?.volumeDb ?? -12,
          pan: typeof metronome.pan === "number" ? metronome.pan : uiState.metronome?.pan ?? 0,
          clickType: typeof metronome.clickType === "string" ? metronome.clickType : uiState.metronome?.clickType ?? "click",
          clickTypes: Array.isArray(metronome.clickTypes)
            ? metronome.clickTypes
                .filter((entry) => entry && typeof entry.id === "string")
                .map((entry) => ({ id: entry.id ?? "", label: typeof entry.label === "string" ? entry.label : entry.id }))
            : uiState.metronome?.clickTypes,
        });
      }
      uiState.signalTest = null;
      const preset = (payload as { preset?: Preset }).preset;
      if (preset) {
        normalizePresetResources(preset);
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
        normalizePresetResources(preset);
        uiState.activePresetId = preset.id;
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
      uiState.demoAudioSelectedId = (payload as { id?: string }).id ?? uiState.demoAudioSelectedId;
      const selector = document.getElementById("demo-audio-select") as HTMLSelectElement | null;
      if (selector && uiState.demoAudioSelectedId) {
        selector.value = uiState.demoAudioSelectedId;
      }
      onDemoAudioStarted();
      showNotification("Playing demo audio", (payload as { title?: string }).title ?? "Demo");
      break;
    }
    case "previewComplete": {
      appendLog(`preview complete ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
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
      onDemoAudioStopped();
      showNotification("Demo playback stopped", (payload as { title?: string }).title ?? "Demo");
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
    case "namCalibrationStatus": {
      const info = payload as { nodeId?: string; status?: string };
      if (info.nodeId) {
        uiState.namCalibrationStatus = uiState.namCalibrationStatus ?? {};
        if (info.status === "calibrating") {
          uiState.namCalibrationStatus[info.nodeId] = "calibrating";
        } else {
          delete uiState.namCalibrationStatus[info.nodeId];
        }
        renderActivePreset();
      }
      break;
    }
    case "namCalibrationApplied": {
      const info = payload as { nodeId?: string; params?: Record<string, number> };
      if (!info.nodeId || !info.params) {
        break;
      }
      const activePresetId = uiState.activePresetId ?? "";
      const preset = getActivePresetForRender();
      if (preset?.graph) {
        const node = preset.graph.nodes.find((n) => n.id === info.nodeId);
        if (node) {
          Object.entries(info.params).forEach(([key, value]) => {
            if (typeof value === "number") {
              node.params[key] = value;
            }
          });
          uiState.presetCache.set(preset.id, preset);
          renderActivePreset();
          refreshSelectedNodeParams();
        }
      }
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
        savePresetToLocalStorage(savedPreset);
        uiState.activePresetId = savedPreset.id;
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
      }
      break;
    }
    case "dspPerformance": {
      const stats = payload as { stats?: import("./types.js").DSPPerformanceStats };
      if (stats.stats) {
        uiState.dspPerformance = stats.stats;
        uiState.dspPerformanceHistory.push(stats.stats);
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
      }
      break;
    }
    case "globalSignalChainChanged": {
      const chainPayload = payload as { globalSignalChain?: import("./types.js").GlobalSignalChainConfig };
      if (chainPayload.globalSignalChain) {
        uiState.globalSignalChain = normalizeGlobalSignalChain(chainPayload.globalSignalChain) ?? uiState.globalSignalChain;
        appendLog("Global signal chain configuration updated");
        // Sync any UI controls that show global chain state
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
      }
      break;
    }
    case "layoutSaved": {
      const savePayload = payload as { effectType?: string; blendId?: string; lookupKey?: string; layout?: EffectLayout };
      if (savePayload.effectType && savePayload.layout && uiState.layoutLibrary) {
        // Use the lookup key from the backend, or compute it from effectType + blendId
        const key = savePayload.lookupKey || layoutLookupKey(savePayload.effectType, savePayload.blendId);

        // Update layout in library using the composite key
        if (!uiState.layoutLibrary.byEffectType[key]) {
          uiState.layoutLibrary.byEffectType[key] = [];
        }
        const layouts = uiState.layoutLibrary.byEffectType[key];
        const existingIdx = layouts.findIndex(e => e.layoutId === key + "-default");
        const entry = {
          layout: savePayload.layout,
          isDefault: true,
          layoutId: key + "-default",
        };
        if (existingIdx >= 0) {
          layouts[existingIdx] = entry;
        } else {
          layouts.push(entry);
        }
        uiState.layoutLibrary.defaults[key] = entry.layoutId;

        const displayKey = savePayload.blendId ? `${savePayload.effectType} (blend: ${savePayload.blendId})` : savePayload.effectType;
        appendLog(`Layout saved for ${displayKey}`);
        showNotification("Layout saved", "success");
        // Refresh signal path view to use new layout
        renderActivePreset();
      }
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
    default:
      console.warn("Unknown message type", payload.type);
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
    paramPath,
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
