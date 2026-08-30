import { appendLog } from "./logging.js";
import { postMessage, sendGlobalChainParam, setAppSetting, setMasterGain, setParameter } from "./bridge.js";
import { uiState } from "./state.js";
import type { GlobalSettings, GraphNode, SignalGraph } from "./types.js";
import { EffectGuids } from "./effectGuids.js";
import { GenericKnob } from "./knob.js";
import { EqPanel } from "./eqPanel.js";
import { clampValue, countStepDecimals, deriveRangeStep } from "./utils.js";

// The knob widget moved to ./knob.js so the EQ panel component (and anything
// else) can use one without importing this module. Re-exported here so the
// modules that already import it from `./controls.js` are unaffected.
export { GenericKnob, type KnobConfig } from "./knob.js";

interface RangeInputInteractionOptions {
  onImmediateChange?: (value: number) => void;
}

export function enhanceRangeInput(
  input: HTMLInputElement,
  options: RangeInputInteractionOptions = {},
): void {
  if (input.dataset.rangeInteractionsBound === "true") {
    return;
  }

  input.dataset.rangeInteractionsBound = "true";
  input.tabIndex = input.disabled ? -1 : 0;

  const getBounds = (): { min: number; max: number; step: number; decimals: number } => {
    const min = Number.parseFloat(input.min || "0");
    const max = Number.parseFloat(input.max || "100");
    const parsedStep = Number.parseFloat(input.step || "");
    const step = deriveRangeStep(min, max, Number.isFinite(parsedStep) && parsedStep > 0 ? parsedStep : undefined);
    return { min, max, step, decimals: countStepDecimals(step) };
  };

  input.addEventListener("pointerdown", () => {
    if (!input.disabled) {
      input.focus();
    }
  });

  input.addEventListener(
    "wheel",
    (event) => {
      if (input.disabled) {
        return;
      }

      event.preventDefault();
      input.focus();

      const { min, max, step, decimals } = getBounds();
      const currentValue = Number.parseFloat(input.value || "0");
      const delta = event.deltaY < 0 ? step : -step;
      const nextValue = clampValue(currentValue + delta, min, max);
      const formatted = decimals > 0 ? nextValue.toFixed(decimals) : `${Math.round(nextValue)}`;

      if (formatted === input.value) {
        return;
      }

      input.value = formatted;
      options.onImmediateChange?.(nextValue);
      input.dispatchEvent(new Event("input", { bubbles: true }));
    },
    { passive: false },
  );
}

function updateControlDisplay(controlId: string, value: number, format: "percent" | "db" | "value" = "percent") {
  const displayElement = document.getElementById(`control-${controlId}-value`);
  if (!displayElement) return;

  let displayText: string;
  switch (format) {
    case "db":
      displayText = `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`;
      break;
    case "percent":
      displayText = `${Math.round(value * 100)}%`;
      break;
    default:
      displayText = value.toFixed(2);
  }
  displayElement.textContent = displayText;
}

// Store knob instances globally for sync
const knobInstances: Map<string, GenericKnob> = new Map();
const outputMuteToggle = document.getElementById("output-mute-toggle") as HTMLButtonElement | null;
let outputMuted = false;
let lastNonMutedMasterGain = 1.0;

function updateOutputMuteToggleState(): void {
  if (!outputMuteToggle) {
    return;
  }

  outputMuteToggle.classList.toggle("muted", outputMuted);
  outputMuteToggle.setAttribute("aria-pressed", outputMuted ? "true" : "false");
  outputMuteToggle.title = outputMuted ? "Unmute output" : "Mute output";
  outputMuteToggle.setAttribute("aria-label", outputMuted ? "Output muted. Click to unmute" : "Mute output");
}

function getCurrentMasterGain(): number {
  const gain = uiState.mixer?.masterGain;
  return typeof gain === "number" && isFinite(gain) ? gain : 1.0;
}

function syncOutputMuteFromState(): void {
  const masterGain = getCurrentMasterGain();
  outputMuted = masterGain <= 1.0e-4;
  if (!outputMuted && masterGain > 1.0e-4) {
    lastNonMutedMasterGain = masterGain;
  }
  updateOutputMuteToggleState();
}

function preserveMuteWhileAdjustingOutput(outputGainDb: number): void {
  if (!outputMuted) {
    return;
  }

  lastNonMutedMasterGain = Math.pow(10.0, outputGainDb / 20.0);
  setMasterGain(0.0);
  if (uiState.mixer) {
    uiState.mixer.masterGain = 0.0;
  }
  updateOutputMuteToggleState();
}

function toggleOutputMute(): void {
  if (!outputMuted) {
    const currentMasterGain = getCurrentMasterGain();
    if (currentMasterGain > 1.0e-4) {
      lastNonMutedMasterGain = currentMasterGain;
    }
    setMasterGain(0.0);
    if (uiState.mixer) {
      uiState.mixer.masterGain = 0.0;
    }
    outputMuted = true;
    appendLog("output muted");
  } else {
    const restoreGain = lastNonMutedMasterGain > 1.0e-4 ? lastNonMutedMasterGain : 1.0;
    setMasterGain(restoreGain);
    if (uiState.mixer) {
      uiState.mixer.masterGain = restoreGain;
    }
    outputMuted = false;
    appendLog(`output unmuted → ${restoreGain.toFixed(3)}`);
  }

  updateOutputMuteToggleState();
}

function setKnobControlDisabled(controlId: string, disabled: boolean): void {
  const control = document.getElementById(controlId);
  if (!control) return;
  control.classList.toggle("disabled", disabled);
}

function updateGateThresholdEnabled(enabled: boolean): void {
  setKnobControlDisabled("gate-threshold-control", !enabled);
}

function updateDelayEnabled(enabled: boolean): void {
  setKnobControlDisabled("delay-control", !enabled);
}

function initializeDoublerControls(): void {
  const doublerToggle = document.getElementById("doubler-toggle") as HTMLInputElement | null;

  if (doublerToggle) {
    doublerToggle.addEventListener("change", () => {
      const enabled = doublerToggle.checked;
      sendGlobalChainParam("doubler.enabled", enabled);
      const doublerNode = getPostChainDoublerNode();
      if (doublerNode) {
        doublerNode.bypassed = !doublerToggle.checked;
      }
      updateDelayEnabled(doublerToggle.checked);
    });
  }

  // Initialize Delay knob using GenericKnob
  const delayKnob = document.querySelector('.knob[data-param="delay"]') as HTMLElement | null;
  if (delayKnob) {
    const delayKnobInstance = new GenericKnob({
      knobElement: delayKnob,
      paramId: "doubler_delay",
      minValue: 0.5,
      maxValue: 50.0,
      defaultValue: 6.0,
      displayFormat: (value) => `${value.toFixed(2)} ms`,
      valueDisplayId: "delay-value",
      sensitivity: 0.5,
      sendParameter: false,
      onValueCommit: (value) => {
        sendGlobalChainParam("doubler.delay", value);
        const doublerNode = getPostChainDoublerNode();
        if (doublerNode) {
          doublerNode.params.time = value;
        }
      },
    });
    knobInstances.set("doubler_delay", delayKnobInstance);
  }

  updateDelayEnabled(doublerToggle?.checked ?? true);
}

function initializeInputOutputKnobs(): void {
  // Initialize Input Level knob
  const inputKnob = document.querySelector('.knob[data-param="input"]') as HTMLElement | null;
  if (inputKnob) {
    const inputKnobInstance = new GenericKnob({
      knobElement: inputKnob,
      paramId: "input_trim",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "input-value",
      sensitivity: 0.1,
      sendParameter: false,
      onValueChange: (value) => {
        sendGlobalChainParam("input.gain", value);
        if (uiState.globalSignalChain) {
          uiState.globalSignalChain.inputGain = value;
        }
      },
      onValueCommit: (value) => {
        if (uiState.globalSignalChain) {
          uiState.globalSignalChain.inputGain = value;
        }
      },
    });
    knobInstances.set("input_trim", inputKnobInstance);
  }

  // Initialize Output Level knob
  const outputKnob = document.querySelector('.knob[data-param="output"]') as HTMLElement | null;
  if (outputKnob) {
    const outputKnobInstance = new GenericKnob({
      knobElement: outputKnob,
      paramId: "output_trim",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "output-value",
      sensitivity: 0.1,
      sendParameter: false,
      onValueChange: (value) => {
        sendGlobalChainParam("output.gain", value);
        if (uiState.globalSignalChain) {
          uiState.globalSignalChain.outputGain = value;
        }
        lastNonMutedMasterGain = Math.pow(10.0, value / 20.0);
        preserveMuteWhileAdjustingOutput(value);
      },
      onValueCommit: (value) => {
        if (uiState.globalSignalChain) {
          uiState.globalSignalChain.outputGain = value;
        }
        lastNonMutedMasterGain = Math.pow(10.0, value / 20.0);
        preserveMuteWhileAdjustingOutput(value);
      },
    });
    knobInstances.set("output_trim", outputKnobInstance);
  }

  // Initialize Transpose knob
  const transposeKnob = document.querySelector('.knob[data-param="transpose"]') as HTMLElement | null;
  if (transposeKnob) {
    const transposeKnobInstance = new GenericKnob({
      knobElement: transposeKnob,
      paramId: "transpose",
      minValue: -12,
      maxValue: 12,
      defaultValue: 0,
      displayFormat: (value) => {
        const rounded = Math.round(value);
        return rounded >= 0 ? `+${rounded} st` : `${rounded} st`;
      },
      valueDisplayId: "transpose-value",
      sensitivity: 0.1,
      sendParameter: false,
      onValueChange: (value) => {
        // Snap to integer values for semitones and send to plugin
        const rounded = Math.round(value);
        if (Math.abs(value - rounded) > 0.01) {
          transposeKnobInstance.setValue(rounded);
        }
        const enabled = rounded !== 0;
        // Always send the rounded integer value to the plugin
        sendGlobalChainParam("transpose.semitones", rounded);
        sendGlobalChainParam("transpose.enabled", enabled);
        const transposeNode = getPreChainTransposeNode();
        if (transposeNode) {
          transposeNode.params.semitones = rounded;
          transposeNode.bypassed = !enabled;
        }
      },
      onValueCommit: (value) => {
        const rounded = Math.round(value);
        const enabled = rounded !== 0;
        sendGlobalChainParam("transpose.semitones", rounded);
        sendGlobalChainParam("transpose.enabled", enabled);
        const transposeNode = getPreChainTransposeNode();
        if (transposeNode) {
          transposeNode.params.semitones = rounded;
          transposeNode.bypassed = !enabled;
        }
      },
    });
    knobInstances.set("transpose", transposeKnobInstance);
  }
}

function initializeGateControls(): void {
  const gateToggle = document.getElementById("gate-toggle") as HTMLInputElement | null;

  if (gateToggle) {
    gateToggle.addEventListener("change", () => {
      const enabled = gateToggle.checked;
      sendGlobalChainParam("gate.enabled", enabled);
      const gateNode = getPreChainGateNode();
      if (gateNode) {
        gateNode.bypassed = !gateToggle.checked;
      }
      appendLog(`preChainGraph.global_gate.enabled → ${enabled}`);
      updateGateThresholdEnabled(gateToggle.checked);
    });
  }

  // Initialize Gate Threshold knob
  const thresholdKnob = document.querySelector('.knob[data-param="gate_threshold"]') as HTMLElement | null;
  if (thresholdKnob) {
    const thresholdKnobInstance = new GenericKnob({
      knobElement: thresholdKnob,
      paramId: "gate_threshold",
      minValue: -80.0,
      maxValue: -20.0,
      defaultValue: -60.0,
      displayFormat: (value) => `${value.toFixed(0)} dB`,
      valueDisplayId: "gate-threshold-value",
      sensitivity: 0.5,
      sendParameter: false,
      onValueCommit: (value) => {
        sendGlobalChainParam("gate.threshold", value);
        const gateNode = getPreChainGateNode();
        if (gateNode) {
          gateNode.params.threshold = value;
        }
      },
    });
    knobInstances.set("gate_threshold", thresholdKnobInstance);
  }

  updateGateThresholdEnabled(gateToggle?.checked ?? true);
}

export function syncGateControlsFromState(): void {
  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  const gateToggle = document.getElementById("gate-toggle") as HTMLInputElement | null;
  const gateNode = getPreChainGateNode();
  if (gateToggle && gateNode) {
    gateToggle.checked = !gateNode.bypassed;
    updateGateThresholdEnabled(gateToggle.checked);
  } else if (gateToggle && typeof paramValues.gate_enabled === "number") {
    gateToggle.checked = paramValues.gate_enabled > 0.5;
    updateGateThresholdEnabled(gateToggle.checked);
  }

  // Sync threshold knob
  const thresholdKnobInstance = knobInstances.get("gate_threshold");
  if (thresholdKnobInstance && gateNode && typeof gateNode.params.threshold === "number") {
    thresholdKnobInstance.setValue(gateNode.params.threshold);
  } else if (thresholdKnobInstance && typeof paramValues.gate_threshold === "number") {
    thresholdKnobInstance.setValue(paramValues.gate_threshold);
  }
}

export function initializeControls(): void {
  const controls = [
    { id: "mix", paramId: "mix", format: "percent" as const },
    { id: "drive", paramId: "drive", format: "percent" as const },
    { id: "output-trim", paramId: "output_trim", format: "db" as const },
    { id: "tone", paramId: "tone", format: "percent" as const },
    { id: "input-trim", paramId: "input_trim", format: "db" as const },
  ];

  controls.forEach(({ id, paramId, format }) => {
    const slider = document.getElementById(`control-${id}`) as HTMLInputElement | null;
    if (!slider) return;

    enhanceRangeInput(slider);

    slider.addEventListener("input", () => {
      const value = parseFloat(slider.value);
      updateControlDisplay(id, value, format);
    });

    slider.addEventListener("change", () => {
      const value = parseFloat(slider.value);
      setParameter(paramId, value);
      appendLog(`${paramId} → ${value}`);
    });
  });

  initializeDoublerControls();
  initializeInputOutputKnobs();
  initializeAutoLevelControls();
  initializeGateControls();
  initializeEQControls();
  if (outputMuteToggle && outputMuteToggle.dataset.bound !== "true") {
    outputMuteToggle.dataset.bound = "true";
    outputMuteToggle.addEventListener("click", toggleOutputMute);
  }
  syncOutputMuteFromState();
}

export function syncDoublerControlsFromState(): void {
  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  const doublerToggle = document.getElementById("doubler-toggle") as HTMLInputElement | null;
  const doublerNode = getPostChainDoublerNode();
  if (doublerToggle && doublerNode) {
    doublerToggle.checked = !doublerNode.bypassed;
    updateDelayEnabled(doublerToggle.checked);
  } else if (doublerToggle && typeof paramValues.doubler_enabled === "number") {
    doublerToggle.checked = paramValues.doubler_enabled > 0.5;
    updateDelayEnabled(doublerToggle.checked);
  }

  // Sync delay knob using GenericKnob instance
  const delayKnobInstance = knobInstances.get("doubler_delay");
  if (delayKnobInstance && doublerNode && typeof doublerNode.params.time === "number") {
    delayKnobInstance.setValue(doublerNode.params.time);
  } else if (delayKnobInstance && typeof paramValues.doubler_delay === "number") {
    delayKnobInstance.setValue(paramValues.doubler_delay);
  }

  // Sync input level knob
  const inputKnobInstance = knobInstances.get("input_trim");
  if (inputKnobInstance && typeof uiState.globalSignalChain?.inputGain === "number") {
    inputKnobInstance.setValue(uiState.globalSignalChain.inputGain);
  } else if (inputKnobInstance && typeof paramValues.input_trim === "number") {
    inputKnobInstance.setValue(paramValues.input_trim);
  }

  // Sync output level knob
  const outputKnobInstance = knobInstances.get("output_trim");
  if (outputKnobInstance && typeof uiState.globalSignalChain?.outputGain === "number") {
    outputKnobInstance.setValue(uiState.globalSignalChain.outputGain);
  } else if (outputKnobInstance && typeof paramValues.output_trim === "number") {
    outputKnobInstance.setValue(paramValues.output_trim);
  }

  // Sync transpose knob
  const transposeKnobInstance = knobInstances.get("transpose");
  const transposeNode = getPreChainTransposeNode();
  if (transposeKnobInstance && transposeNode && typeof transposeNode.params.semitones === "number") {
    transposeKnobInstance.setValue(transposeNode.params.semitones);
  } else if (transposeKnobInstance && typeof paramValues.transpose === "number") {
    transposeKnobInstance.setValue(paramValues.transpose);
  }
}

export function syncControlsFromState(): void {
  const paramToControl: Record<string, { id: string; format: "percent" | "db" }> = {
    mix: { id: "mix", format: "percent" },
    drive: { id: "drive", format: "percent" },
    output_trim: { id: "output-trim", format: "db" },
    tone: { id: "tone", format: "percent" },
    input_trim: { id: "input-trim", format: "db" },
  };

  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  Object.entries(paramToControl).forEach(([paramId, { id, format }]) => {
    const slider = document.getElementById(`control-${id}`) as HTMLInputElement | null;
    if (!slider) return;

    const value = paramValues[paramId];
    if (typeof value === "number") {
      slider.value = value.toString();
      updateControlDisplay(id, value, format);
    }
  });

  syncDoublerControlsFromState();
  syncGateControlsFromState();
  syncAutoLevelControlsFromState();
  syncEQControlsFromState();
  syncOutputMuteFromState();
}

// Input mode state
let currentMonoMode = true;
let currentInputChannel = 0;
const INPUT_CHANNEL_SETTING = "inputChannel.mono";
const MONO_MODE_SETTING = "inputChannel.monoMode";

function isStandaloneUi(): boolean {
  return Boolean(uiState.environment?.standalone || document.body.classList.contains("is-standalone"));
}

function normalizeInputChannel(value: unknown): number | null {
  const numeric = typeof value === "string" ? Number(value) : value;
  if (numeric === 0 || numeric === 1) {
    return numeric;
  }
  return null;
}

function getStoredInputChannel(): number | null {
  return normalizeInputChannel(uiState.appSettings?.[INPUT_CHANNEL_SETTING]);
}

function getStoredMonoMode(): boolean | null {
  const raw = uiState.appSettings?.[MONO_MODE_SETTING];
  if (typeof raw === "boolean") return raw;
  return null;
}

function persistInputChannel(channel: number): void {
  uiState.appSettings[INPUT_CHANNEL_SETTING] = channel;
  setAppSetting(INPUT_CHANNEL_SETTING, channel);
}

function persistMonoMode(mono: boolean): void {
  uiState.appSettings[MONO_MODE_SETTING] = mono;
  setAppSetting(MONO_MODE_SETTING, mono);
}

export function applyStoredInputChannel(): void {
  const storedMono = getStoredMonoMode();
  if (storedMono !== null) {
    currentMonoMode = storedMono;
  }

  const stored = getStoredInputChannel();
  if (stored !== null) {
    currentInputChannel = stored;
    const inputChannelSelect = document.getElementById("input-channel-select") as HTMLSelectElement | null;
    if (inputChannelSelect) {
      inputChannelSelect.value = stored.toString();
    }
  }

  if (isStandaloneUi()) {
    sendInputModeToPlugin();
  }
}

function sendInputModeToPlugin(): void {
  if (!isStandaloneUi()) {
    return;
  }
  const message = JSON.stringify({
    type: "setInputMode",
    monoMode: currentMonoMode,
    inputChannel: currentInputChannel,
  });
  
  // Use the legacy bridge function name when available.
  if (typeof (window as any).IPlugSendMsg === "function") {
    (window as any).IPlugSendMsg(message);
  }
  
  appendLog(`Input mode: ${currentMonoMode ? "Mono" : "Stereo"}, Channel: ${currentInputChannel + 1}`);
}

export function initializeInputModeControls(): void {
  const stereoToggle = document.getElementById("stereo-input-toggle") as HTMLInputElement | null;
  const inputChannelSelector = document.getElementById("input-channel-selector");
  const inputChannelSelect = document.getElementById("input-channel-select") as HTMLSelectElement | null;
  const inputModeStatus = document.getElementById("input-mode-status");

  // Show/hide channel selector based on mono mode
  function updateChannelSelectorVisibility(): void {
    if (inputChannelSelector) {
      if (currentMonoMode) {
        inputChannelSelector.classList.remove("hidden");
      } else {
        inputChannelSelector.classList.add("hidden");
      }
    }

    if (inputModeStatus) {
      if (currentMonoMode) {
        inputModeStatus.textContent = "MONO";
      } else {
        inputModeStatus.textContent = "STEREO";
      }
    }
  }

  if (stereoToggle) {
    stereoToggle.addEventListener("change", () => {
      currentMonoMode = !stereoToggle.checked;
      persistMonoMode(currentMonoMode);
      updateChannelSelectorVisibility();
      if (currentMonoMode) {
        const stored = getStoredInputChannel();
        if (stored !== null) {
          currentInputChannel = stored;
          if (inputChannelSelect) {
            inputChannelSelect.value = stored.toString();
          }
        }
      }
      sendInputModeToPlugin();
    });
  }

  // Initialize channel select listener
  if (inputChannelSelect) {
    inputChannelSelect.addEventListener("change", () => {
      currentInputChannel = parseInt(inputChannelSelect.value, 10);
      if (!Number.isNaN(currentInputChannel)) {
        persistInputChannel(currentInputChannel);
      }
      sendInputModeToPlugin();
    });
  }

  // Restore persisted state before updating UI
  applyStoredInputChannel();

  if (stereoToggle) {
    stereoToggle.checked = !currentMonoMode;
  }
  // Set initial state
  updateChannelSelectorVisibility();
  sendInputModeToPlugin();
}

export function handleInputModeChanged(monoMode: boolean, inputChannel: number): void {
  currentMonoMode = monoMode;
  currentInputChannel = inputChannel;

  const stereoToggle = document.getElementById("stereo-input-toggle") as HTMLInputElement | null;
  if (stereoToggle) {
    stereoToggle.checked = !monoMode;
  }

  const inputModeStatus = document.getElementById("input-mode-status");
  if (inputModeStatus) {
    inputModeStatus.textContent = monoMode ? "MONO" : "STEREO";
  }

  // Update channel select
  const inputChannelSelect = document.getElementById("input-channel-select") as HTMLSelectElement | null;
  if (inputChannelSelect) {
    inputChannelSelect.value = inputChannel.toString();
  }

  // Update visibility
  const inputChannelSelector = document.getElementById("input-channel-selector");
  if (inputChannelSelector) {
    if (monoMode) {
      inputChannelSelector.classList.remove("hidden");
    } else {
      inputChannelSelector.classList.add("hidden");
    }
  }
}

// Amp and Cab power state
let ampEnabled = true;
let cabEnabled = true;

let autoLevelInputEnabled = false;
let autoLevelOutputEnabled = false;

function updateAutoLevelKnobStates(): void {
  setKnobControlDisabled("input-control", false);
  setKnobControlDisabled("output-control", false);
}

function sendAmpCabStateToPlugin(): void {
  const message = JSON.stringify({
    type: "setAmpCabState",
    ampEnabled: ampEnabled,
    cabEnabled: cabEnabled,
  });
  
  if (typeof (window as any).IPlugSendMsg === "function") {
    (window as any).IPlugSendMsg(message);
  }
  
  appendLog(`Amp: ${ampEnabled ? "ON" : "OFF"}, Cab: ${cabEnabled ? "ON" : "OFF"}`);
}

export function initializeAmpCabPowerControls(): void {
  const ampPowerSwitch = document.getElementById("power-switch");
  const cabPowerSwitch = document.getElementById("cab-power-switch");

  if (ampPowerSwitch) {
    ampPowerSwitch.addEventListener("click", () => {
      ampEnabled = !ampEnabled;
      ampPowerSwitch.classList.toggle("off", !ampEnabled);
      sendAmpCabStateToPlugin();
    });
  }

  if (cabPowerSwitch) {
    cabPowerSwitch.addEventListener("click", () => {
      cabEnabled = !cabEnabled;
      cabPowerSwitch.classList.toggle("off", !cabEnabled);
      sendAmpCabStateToPlugin();
    });
  }

  // Send initial state
  sendAmpCabStateToPlugin();
}

export function handleAmpCabStateChanged(newAmpEnabled: boolean, newCabEnabled: boolean): void {
  ampEnabled = newAmpEnabled;
  cabEnabled = newCabEnabled;

  const ampPowerSwitch = document.getElementById("power-switch");
  const cabPowerSwitch = document.getElementById("cab-power-switch");

  if (ampPowerSwitch) {
    ampPowerSwitch.classList.toggle("off", !ampEnabled);
  }

  if (cabPowerSwitch) {
    cabPowerSwitch.classList.toggle("off", !cabEnabled);
  }
}

// ===== Parametric EQ Controls =====
let eqEnabled = false;

const findGraphNode = (graph: SignalGraph | undefined, id: string, type: string): GraphNode | undefined => {
  if (!graph) {
    return undefined;
  }
  const byId = graph.nodes.find((node) => node.id === id);
  if (byId) {
    return byId;
  }
  return graph.nodes.find((node) => node.type === type);
};

const getPreChainGateNode = (): GraphNode | undefined =>
  findGraphNode(uiState.globalSignalChain?.preChainGraph, "global_gate", EffectGuids.kDynamicsGate);

const getPreChainTransposeNode = (): GraphNode | undefined =>
  findGraphNode(uiState.globalSignalChain?.preChainGraph, "global_transpose", EffectGuids.kTranspose);

const getPostChainDoublerNode = (): GraphNode | undefined =>
  findGraphNode(uiState.globalSignalChain?.postChainGraph, "global_doubler", EffectGuids.kDelayDoubler);

const getPostChainEqNode = (): GraphNode | undefined =>
  findGraphNode(uiState.globalSignalChain?.postChainGraph, "global_eq", EffectGuids.kEqParametric);

// ── Global EQ ────────────────────────────────────────────────────────────────
// Every control in the Global EQ modal is the shared EqPanel component
// (ts/eqPanel.ts) — the band knobs, the draggable curve, the enable toggle.
// What lives here is only what is specific to *this* EQ: its values are the
// post-chain  node's params, and a change reaches the engine as a
// global-chain param path. The Practice Tool's Backing Track EQ binds the same
// component to its own state; neither owns any control code.

let globalEqPanel: EqPanel | null = null;

/** Mirrors the modal's enable state onto the control-bar toggle and the
 * .eq-section / .eq-control 'enabled' classes, which are Global-EQ-specific
 * chrome the component knows nothing about. */
function updateEQSectionState(): void {
  document.querySelectorAll('.eq-section').forEach((section) => {
    section.classList.toggle('enabled', eqEnabled);
  });
  document.querySelector('.eq-control')?.classList.toggle('enabled', eqEnabled);

  const eqToggle = document.getElementById('eq-toggle') as HTMLInputElement | null;
  if (eqToggle) {
    eqToggle.checked = eqEnabled;
  }
}

/** Redraws the modal from the current node state. Called after a preset load
 * and whenever the control bar changes the EQ. */
export function refreshEqModalVisualization(): void {
  globalEqPanel?.render();
}

function initializeEQControls(): void {
  const bandsHost = document.getElementById('eq-modal-bands');
  if (bandsHost && !globalEqPanel) {
    globalEqPanel = new EqPanel(
      {
        bandsHost,
        canvas: document.getElementById('eq-curve-canvas') as HTMLCanvasElement | null,
        toggle: document.getElementById('eq-modal-toggle') as HTMLInputElement | null,
        idPrefix: 'global_eq',
        onChanged: () => updateEQSectionState(),
      },
      {
        label: 'eq',
        readParams: () => getPostChainEqNode()?.params ?? {},
        writeParams: (changed) => {
          // The node is the local copy the preset is saved from, so it is
          // written whether or not this is the end of a gesture; the send is
          // cheap enough that the global chain has never coalesced it.
          const eqNode = getPostChainEqNode();
          for (const [key, value] of Object.entries(changed)) {
            if (eqNode) {
              eqNode.params[key] = value;
            }
            sendGlobalChainParam(`eq.${key}`, value);
          }
        },
        readEnabled: () => eqEnabled,
        writeEnabled: (enabled) => applyGlobalEqEnabled(enabled, true),
      }
    );
  }

  const eqToggle = document.getElementById('eq-toggle') as HTMLInputElement | null;
  if (eqToggle && eqToggle.dataset.bound !== 'true') {
    eqToggle.dataset.bound = 'true';
    // The control-bar toggle and the modal's own toggle are two views of one
    // flag; the modal's is owned by the component, this one mirrors it.
    eqToggle.addEventListener('change', () => applyGlobalEqEnabled(eqToggle.checked, true));
  }

  updateEQSectionState();
  refreshEqModalVisualization();
}

function applyGlobalEqEnabled(nextValue: boolean, shouldSend: boolean): void {
  eqEnabled = nextValue;
  const modalToggle = document.getElementById('eq-modal-toggle') as HTMLInputElement | null;
  if (modalToggle) {
    modalToggle.checked = eqEnabled;
  }
  if (shouldSend) {
    sendGlobalChainParam('eq.enabled', eqEnabled);
    const eqNode = getPostChainEqNode();
    if (eqNode) {
      eqNode.bypassed = !eqEnabled;
    }
    appendLog(`eq.enabled → ${eqEnabled}`);
  }
  updateEQSectionState();
  // The control-bar toggle is a second way in, so the panel has to be told
  // rather than assuming its own toggle was the one that moved.
  refreshEqModalVisualization();
}

/** Re-reads everything from the freshly-loaded preset's EQ node. */
export function syncEQControlsFromState(): void {
  const eqNode = getPostChainEqNode();
  if (eqNode) {
    const declaredEnabled = (eqNode as { enabled?: boolean }).enabled;
    applyGlobalEqEnabled(typeof declaredEnabled === 'boolean' ? declaredEnabled : !eqNode.bypassed, false);
  }
  refreshEqModalVisualization();
}

export { initializeEQControls };


function updateActivePresetGlobals(next: Partial<GlobalSettings>): void {
  const activeId = uiState.activePresetId ?? "";
  const preset = uiState.presetCache.get(activeId) as any;
  if (!preset) return;

  const current = (preset.globals ?? preset.global ?? {}) as Record<string, unknown>;
  const merged = {
    inputTrim: current.inputTrim ?? 0,
    outputTrim: current.outputTrim ?? (current.outputVolume ?? 0),
    masterVolume: current.masterVolume ?? current.outputVolume ?? 1,
    autoLevelInput: current.autoLevelInput ?? false,
    autoLevelOutput: current.autoLevelOutput ?? false,
    transpose: current.transpose ?? 0,
    ...next,
  };

  preset.globals = merged;
  preset.global = merged;
  uiState.presetCache.set(activeId, preset);
}

function sendAutoLevelToPlugin(): void {
  postMessage({
    type: "setAutoLevel",
    autoInput: autoLevelInputEnabled,
    autoOutput: autoLevelOutputEnabled,
  });
  appendLog(`setAutoLevel → in:${autoLevelInputEnabled} out:${autoLevelOutputEnabled}`);
}

function initializeAutoLevelControls(): void {
  const autoIn = document.getElementById("auto-level-input-toggle") as HTMLInputElement | null;
  const autoOut = document.getElementById("auto-level-output-toggle") as HTMLInputElement | null;

  // Mixer-wide peak auto-leveling is retired. Keep the legacy wiring inert so
  // old state does not disable the manual input/output controls.
  autoLevelInputEnabled = false;
  autoLevelOutputEnabled = false;
  if (autoIn) autoIn.checked = false;
  if (autoOut) autoOut.checked = false;
  updateAutoLevelKnobStates();

  if (!autoIn && !autoOut) {
    return;
  }

  const syncFromGlobals = (): void => {
    autoLevelInputEnabled = false;
    autoLevelOutputEnabled = false;
    if (autoIn) autoIn.checked = false;
    if (autoOut) autoOut.checked = false;
    updateAutoLevelKnobStates();
  };

  syncFromGlobals();

  if (autoIn) {
    autoIn.addEventListener("change", () => {
      autoLevelInputEnabled = autoIn.checked;
      updateActivePresetGlobals({ autoLevelInput: autoLevelInputEnabled });
      if (uiState.globalSignalChain) {
        uiState.globalSignalChain.autoLevelInput = autoLevelInputEnabled;
      }
      sendAutoLevelToPlugin();
      updateAutoLevelKnobStates();
    });
  }

  if (autoOut) {
    autoOut.addEventListener("change", () => {
      autoLevelOutputEnabled = autoOut.checked;
      updateActivePresetGlobals({ autoLevelOutput: autoLevelOutputEnabled });
      if (uiState.globalSignalChain) {
        uiState.globalSignalChain.autoLevelOutput = autoLevelOutputEnabled;
      }
      sendAutoLevelToPlugin();
      updateAutoLevelKnobStates();
    });
  }
}

export function syncAutoLevelControlsFromState(): void {
  autoLevelInputEnabled = false;
  autoLevelOutputEnabled = false;

  const autoIn = document.getElementById("auto-level-input-toggle") as HTMLInputElement | null;
  const autoOut = document.getElementById("auto-level-output-toggle") as HTMLInputElement | null;
  if (autoIn) autoIn.checked = false;
  if (autoOut) autoOut.checked = false;
  updateAutoLevelKnobStates();
}