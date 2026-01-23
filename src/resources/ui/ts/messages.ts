import { uiState, clonePreset } from "./state.js";
import { renderActivePreset, applyPresetFromLibrary, populatePresetDropdown, updatePresetDropdownSelection, savePresetToLocalStorage, updatePresetActionButtons } from "./presets.js";
import { syncControlsFromState, handleInputModeChanged, handleAmpCabStateChanged, syncAutoLevelControlsFromState, applyStoredInputChannel } from "./controls.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { previewSelectedDemoAudio } from "./demoAudio.js";
import { handleTunerUpdate, handleTunerStarted, handleTunerStopped, handleTunerReferenceChanged, handleTunerLiveModeChanged } from "./tuner.js";
import { applyUiSettings } from "./windowSettings.js";
import { updateDSPPerformancePlot, updateSignalDiagnosticsView } from "./views.js";
import { refreshSettingsView } from "./settings.js";
import { refreshSelectedNodeParams } from "./signalPath.js";
import { refreshFxSelector } from "./fxSelector.js";
import { applyEnvironmentState, applyMetronomeState } from "./metronome.js";
import type { Preset, UiSettings } from "./types.js";
import { handleResourceDataMessage } from "./archiveUtils.js";

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
      const blendLibrary = (payload as { blendLibrary?: unknown[] }).blendLibrary;
      if (Array.isArray(blendLibrary)) {
        uiState.blendLibrary = blendLibrary as import("./types.js").BlendLibrary;
        refreshFxSelector();
      }
      const appSettings = (payload as { appSettings?: Record<string, unknown> }).appSettings;
      if (appSettings) {
        uiState.appSettings = appSettings as import("./types.js").AppSettings;
        applyStoredInputChannel();
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
        uiState.presetCache.set(preset.id, preset);
        if (!uiState.presets.some((p) => p.id === preset.id)) {
          uiState.presets = [preset, ...uiState.presets];
          uiState.filteredPresets = uiState.presets.slice();
          populatePresetDropdown();
        }
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
    case "presetLoaded": {
      const preset = (payload as { preset?: Preset }).preset;
      if (preset) {
        uiState.activePresetId = preset.id;
        uiState.presetCache.set(preset.id, preset);
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
        uiState.presetCache.set(preset.id, preset);
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
      showNotification("Playing demo audio", (payload as { title?: string }).title ?? "Demo");
      break;
    }
    case "previewComplete": {
      appendLog(`preview complete ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
      if (uiState.demoAudioRepeat) {
        previewSelectedDemoAudio();
      } else {
        showNotification("Demo playback finished", (payload as { title?: string }).title ?? "Demo");
      }
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
      const preset = uiState.presetCache.get(activePresetId) ?? uiState.presets.find((p) => p.id === activePresetId);
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
        uiState.presetCache.set(savedPreset.id, savedPreset);
        if (!uiState.presets.some((p) => p.id === savedPreset.id)) {
          uiState.presets.unshift(savedPreset);
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
        uiState.globalSignalChain = chainPayload.globalSignalChain;
        appendLog("Global signal chain configuration updated");
        // Sync any UI controls that show global chain state
        syncControlsFromState();
      }
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
 * @param paramPath - Dot-notation path to the parameter (e.g., "preChain.gateEnabled", "postChain.eqLowGain")
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
