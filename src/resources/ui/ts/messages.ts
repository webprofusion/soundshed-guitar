import { uiState, clonePreset } from "./state.js";
import { renderActivePreset, applyPresetFromLibrary, populatePresetDropdown, updatePresetDropdownSelection, savePresetToLocalStorage } from "./presets.js";
import { syncControlsFromState } from "./controls.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { previewSelectedDemoAudio } from "./demoAudio.js";
import type { Preset } from "./types.js";

export function handleIncomingMessage(message: string): void {
  const payload = JSON.parse(message) as Record<string, unknown>;
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
          modelPath: (parameters as { modelPath?: string }).modelPath ?? "",
          irPath: (parameters as { irPath?: string }).irPath ?? "",
        };
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
          modelPath: (parameters as { modelPath?: string }).modelPath ?? uiState.parameters.modelPath,
          irPath: (parameters as { irPath?: string }).irPath ?? uiState.parameters.irPath,
        };
      }
      if (preset) {
        uiState.presetCache.set(preset.id, preset);
      }
      renderActivePreset();
      syncControlsFromState();
      break;
    }
    case "signalPathTestResult": {
      const result = payload as Record<string, unknown>;
      uiState.signalTest = {
        frequency: (result.frequency as number) ?? 0,
        duration: (result.duration as number) ?? 0,
        sampleRate: (result.sampleRate as number) ?? 0,
        inputRMS: (result.inputRMS as number) ?? 0,
        outputLeft: Array.isArray(result.outputRMS) ? ((result.outputRMS as number[])[0] ?? 0) : 0,
        outputRight: Array.isArray(result.outputRMS) ? ((result.outputRMS as number[])[1] ?? 0) : 0,
        passed: Boolean(result.passed),
        message: (result.message as string) ?? "",
      };
      renderActivePreset();
      showNotification(
        uiState.signalTest.passed ? "Signal path test passed" : "Signal path test failed",
        uiState.signalTest.message ?? "",
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
      uiState.parameters.modelPath = (payload as { path?: string }).path ?? "";
      renderActivePreset();
      showNotification("Model loaded", (payload as { path?: string }).path ?? "");
      break;
    }
    case "irLoaded": {
      appendLog(`IR loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
      uiState.parameters.irPath = (payload as { path?: string }).path ?? "";
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
    default:
      console.warn("Unknown message type", payload.type);
  }
}
