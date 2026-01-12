import { uiState, clonePreset } from "./state.js";
import { renderActivePreset, applyPresetFromLibrary, populatePresetDropdown, updatePresetDropdownSelection, savePresetToLocalStorage, updatePresetActionButtons } from "./presets.js";
import { syncControlsFromState, handleInputModeChanged, handleAmpCabStateChanged, syncAutoLevelControlsFromState } from "./controls.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { previewSelectedDemoAudio } from "./demoAudio.js";
import { handleTunerUpdate, handleTunerStarted, handleTunerStopped, handleTunerReferenceChanged, handleTunerLiveModeChanged } from "./tuner.js";
import { applyUiSettings } from "./windowSettings.js";
import { updateDSPPerformancePlot } from "./views.js";
import type { Preset, UiSettings } from "./types.js";

export function handleIncomingMessage(message: string): void {
  console.log("[JS] handleIncomingMessage received:", message.substring(0, 200));
  const payload = JSON.parse(message) as Record<string, unknown>;
  console.log("[JS] Parsed message type:", payload.type);

  if (payload.type === "dspPerformance") {
    console.log("[JS] DSP Performance message received:", payload);
  }
  switch (payload.type) {
    case "state": {
      uiState.activePresetId = (payload as { activePresetId?: string }).activePresetId ?? null;
      const parameters = (payload as { parameters?: Record<string, unknown> }).parameters;
      if (parameters) {
        uiState.parameters = {
          values: Array.isArray((parameters as { parameters?: unknown }).parameters)
            ? ((parameters as { parameters: [] }).parameters as [])
            : [],
          gateEnabled: (parameters as { gateEnabled?: boolean }).gateEnabled ?? false,
          gateThreshold: typeof (parameters as { gateThreshold?: unknown }).gateThreshold === "number"
            ? ((parameters as { gateThreshold: number }).gateThreshold as number)
            : null,
        };
      }
      // Process resource library
      const resourceLibrary = (payload as { resourceLibrary?: Record<string, unknown[]> }).resourceLibrary;
      if (resourceLibrary) {
        uiState.resourceLibrary = resourceLibrary as import("./types.js").ResourceLibrary;
      }
      const uiSettings = (payload as { uiSettings?: UiSettings }).uiSettings;
      if (uiSettings) {
        uiState.uiSettings = uiSettings;
        applyUiSettings(uiSettings);
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
      showNotification("");
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
          gateEnabled: (parameters as { gateEnabled?: boolean }).gateEnabled ?? uiState.parameters.gateEnabled,
          gateThreshold: typeof (parameters as { gateThreshold?: unknown }).gateThreshold === "number"
            ? ((parameters as { gateThreshold: number }).gateThreshold as number)
            : uiState.parameters.gateThreshold,
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
    case "irLoaded": {
      console.log("[JS] IR loaded event received, path:", (payload as { path?: string }).path);
      appendLog(`IR loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
      renderActivePreset();
      showNotification("IR loaded", (payload as { path?: string }).path ?? "");
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
        console.log("DSP Performance:", stats.stats.dspLoadPercent.toFixed(1) + "% load", stats.stats.nodeProcessingTimesUs);
        // Update plot if panel is visible
        updateDSPPerformancePlot();
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
