import { appendLog } from "./logging.js";
import { setAppSetting } from "./bridge.js";
import { postMessage, setParameter } from "./bridge.js";
import { sendGlobalChainParam } from "./messages.js";
import { uiState } from "./state.js";
import { drawEqCurve, type EqBand, EqCurveInteraction, type EqBandConfig } from "./eqCurve.js";
import type { GraphNode, SignalGraph } from "./types.js";

export interface KnobConfig {
  knobElement: HTMLElement;
  paramId: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  displayFormat: (value: number) => string;
  valueDisplayId?: string;
  valueDisplay?: HTMLElement | null;
  sensitivity?: number;
  onValueChange?: (value: number) => void;
  onValueCommit?: (value: number) => void;
  sendParameter?: boolean;
}

export class GenericKnob {
  private knobElement: HTMLElement;
  private paramId: string;
  private minValue: number;
  private maxValue: number;
  private defaultValue: number;
  private currentValue: number;
  private displayFormat: (value: number) => string;
  private valueDisplay: HTMLElement | null;
  private sensitivity: number;
  private onValueChange?: (value: number) => void;
  private onValueCommit?: (value: number) => void;
  private sendParameter: boolean;
  private isDragging = false;
  private startY = 0;
  private startValue = 0;

  constructor(config: KnobConfig) {
    this.knobElement = config.knobElement;
    this.paramId = config.paramId;
    this.minValue = config.minValue;
    this.maxValue = config.maxValue;
    this.defaultValue = config.defaultValue;
    this.currentValue = config.defaultValue;
    this.displayFormat = config.displayFormat;
    this.sensitivity = config.sensitivity ?? 0.5;
    this.onValueChange = config.onValueChange;
    this.onValueCommit = config.onValueCommit;
    this.sendParameter = config.sendParameter ?? true;
    
    this.valueDisplay = config.valueDisplay
      ?? (config.valueDisplayId ? document.getElementById(config.valueDisplayId) : null);

    this.initialize();
  }

  private initialize(): void {
    // Set initial value from data attribute if present
    const dataValue = parseFloat(this.knobElement.dataset.value ?? "");
    if (!isNaN(dataValue)) {
      this.currentValue = dataValue;
    }

    this.updateDisplay(this.currentValue);
    this.setupEventListeners();
  }

  private setupEventListeners(): void {
    this.knobElement.addEventListener("mousedown", (e) => this.onMouseDown(e));
    this.knobElement.addEventListener("dblclick", (e) => this.onDoubleClick(e));
    document.addEventListener("mousemove", (e) => this.onMouseMove(e));
    document.addEventListener("mouseup", () => this.onMouseUp());
  }

  private onDoubleClick(e: MouseEvent): void {
    e.preventDefault();
    e.stopPropagation();
    this.setValue(this.defaultValue);
    if (this.sendParameter) {
      setParameter(this.paramId, this.defaultValue);
      appendLog(`${this.paramId} → ${this.defaultValue.toFixed(2)} (reset to default)`);
    }
    
    if (this.onValueChange) {
      this.onValueChange(this.defaultValue);
    }

    if (this.onValueCommit) {
      this.onValueCommit(this.defaultValue);
    }
  }

  private onMouseDown(e: MouseEvent): void {
    this.isDragging = true;
    this.startY = e.clientY;
    this.startValue = this.currentValue;
    e.preventDefault();
  }

  private onMouseMove(e: MouseEvent): void {
    if (!this.isDragging) return;

    const deltaY = this.startY - e.clientY;
    let newValue = this.startValue + deltaY * this.sensitivity;
    newValue = Math.max(this.minValue, Math.min(this.maxValue, newValue));
    
    this.currentValue = newValue;
    this.knobElement.dataset.value = newValue.toString();
    this.updateDisplay(newValue);
    
    // Send parameter value while dragging
    if (this.sendParameter) {
      setParameter(this.paramId, this.currentValue);
    }
    
    if (this.onValueChange) {
      this.onValueChange(this.currentValue);
    }
  }

  private onMouseUp(): void {
    if (!this.isDragging) return;
    
    this.isDragging = false;
    // Final value send and log on release
    if (this.sendParameter) {
      setParameter(this.paramId, this.currentValue);
      appendLog(`${this.paramId} → ${this.currentValue.toFixed(2)}`);
    }

    if (this.onValueCommit) {
      this.onValueCommit(this.currentValue);
    }
  }

  private updateDisplay(value: number): void {
    if (this.valueDisplay) {
      this.valueDisplay.textContent = this.displayFormat(value);
    }

    const rotation = ((value - this.minValue) / (this.maxValue - this.minValue)) * 270 - 135;
    const indicator = this.knobElement.querySelector(".knob-indicator") as HTMLElement | null;
    if (indicator) {
      indicator.style.transform = `translateX(-50%) rotate(${rotation}deg)`;
    }
  }

  public setValue(value: number): void {
    this.currentValue = Math.max(this.minValue, Math.min(this.maxValue, value));
    this.knobElement.dataset.value = this.currentValue.toString();
    this.updateDisplay(this.currentValue);
  }

  public getValue(): number {
    return this.currentValue;
  }
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
      },
      onValueCommit: (value) => {
        if (uiState.globalSignalChain) {
          uiState.globalSignalChain.outputGain = value;
        }
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
      minValue: -36,
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
        const transposeNode = getPreChainTransposeNode();
        if (transposeNode) {
          transposeNode.params.semitones = rounded;
          transposeNode.bypassed = rounded === 0;
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
  initializeIRQualityControls();
  initializeEQControls();
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
  syncIRQualityFromState();
  syncEQControlsFromState();
}

// Input mode state
let currentMonoMode = true;
let currentInputChannel = 0;
const INPUT_CHANNEL_SETTING = "inputChannel.mono";

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

function persistInputChannel(channel: number): void {
  uiState.appSettings[INPUT_CHANNEL_SETTING] = channel;
  setAppSetting(INPUT_CHANNEL_SETTING, channel);
}

export function applyStoredInputChannel(): void {
  const stored = getStoredInputChannel();
  if (stored === null) return;

  currentInputChannel = stored;
  const inputChannelSelect = document.getElementById("input-channel-select") as HTMLSelectElement | null;
  if (inputChannelSelect) {
    inputChannelSelect.value = stored.toString();
  }

  if (currentMonoMode) {
    sendInputModeToPlugin();
  }
}

function sendInputModeToPlugin(): void {
  const message = JSON.stringify({
    type: "setInputMode",
    monoMode: currentMonoMode,
    inputChannel: currentInputChannel,
  });
  
  // Use IPlugSendMsg if available (standard IPlug2 bridge)
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

  if (stereoToggle) {
    stereoToggle.checked = !currentMonoMode;
  }
  // Set initial state
  updateChannelSelectorVisibility();
  applyStoredInputChannel();
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
  setKnobControlDisabled("input-control", autoLevelInputEnabled);
  setKnobControlDisabled("output-control", autoLevelOutputEnabled);
}

function readAutoLevelFromPreset(): { input?: boolean; output?: boolean } {
  const activeId = uiState.activePresetId ?? "";
  const preset = uiState.presetCache.get(activeId) as import("./types.js").Preset | undefined;
  const globals = (preset as any)?.globals ?? (preset as any)?.global;
  return {
    input: typeof globals?.autoLevelInput === "boolean" ? globals.autoLevelInput : undefined,
    output: typeof globals?.autoLevelOutput === "boolean" ? globals.autoLevelOutput : undefined,
  };
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

// ===== IR Quality Control =====
let currentIRQuality = 1; // Default to Standard

const irQualityInfo: Record<number, string> = {
  0: "~480 samples (~10 ms)",
  1: "~2048 samples (~43 ms)",
  2: "~8192 samples (~170 ms)",
  3: "Full length (unlimited)",
};

function updateIRQualityDisplay(): void {
  const infoSpan = document.getElementById("ir-quality-samples");
  if (infoSpan) {
    infoSpan.textContent = irQualityInfo[currentIRQuality] || "";
  }
}

function initializeIRQualityControls(): void {
  const irQualitySelect = document.getElementById("ir-quality-select") as HTMLSelectElement | null;

  if (irQualitySelect) {
    irQualitySelect.addEventListener("change", () => {
      currentIRQuality = parseInt(irQualitySelect.value, 10);
      setParameter("ir_quality", currentIRQuality);
      appendLog(`ir_quality → ${currentIRQuality} (${["Economy", "Standard", "High", "Full"][currentIRQuality]})`);
      updateIRQualityDisplay();
    });
  }

  // Initial display update
  updateIRQualityDisplay();
}

export function syncIRQualityFromState(): void {
  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  const irQualitySelect = document.getElementById("ir-quality-select") as HTMLSelectElement | null;
  if (irQualitySelect && typeof paramValues.ir_quality === "number") {
    currentIRQuality = Math.round(paramValues.ir_quality);
    irQualitySelect.value = currentIRQuality.toString();
    updateIRQualityDisplay();
  }
}

// ===== Parametric EQ Controls =====
let eqEnabled = false;

type GlobalEqParamBinding = {
  path: string;
  read: (node: GraphNode) => number;
  apply: (node: GraphNode, value: number) => void;
};

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
  findGraphNode(uiState.globalSignalChain?.preChainGraph, "global_gate", "dynamics_gate");

const getPreChainTransposeNode = (): GraphNode | undefined =>
  findGraphNode(uiState.globalSignalChain?.preChainGraph, "global_transpose", "transpose");

const getPostChainDoublerNode = (): GraphNode | undefined =>
  findGraphNode(uiState.globalSignalChain?.postChainGraph, "global_doubler", "delay_doubler");

const getPostChainEqNode = (): GraphNode | undefined =>
  findGraphNode(uiState.globalSignalChain?.postChainGraph, "global_eq", "eq_parametric");

const GLOBAL_EQ_PARAM_MAP: Record<string, GlobalEqParamBinding> = {
  eq_low_gain: {
    path: "eq.lowGain",
    read: (node) => node.params.lowGain,
    apply: (node, value) => { node.params.lowGain = value; },
  },
  eq_low_freq: {
    path: "eq.lowFreq",
    read: (node) => node.params.lowFreq,
    apply: (node, value) => { node.params.lowFreq = value; },
  },
  eq_lowmid_gain: {
    path: "eq.lowMidGain",
    read: (node) => node.params.lowMidGain,
    apply: (node, value) => { node.params.lowMidGain = value; },
  },
  eq_lowmid_freq: {
    path: "eq.lowMidFreq",
    read: (node) => node.params.lowMidFreq,
    apply: (node, value) => { node.params.lowMidFreq = value; },
  },
  eq_lowmid_q: {
    path: "eq.lowMidQ",
    read: (node) => node.params.lowMidQ,
    apply: (node, value) => { node.params.lowMidQ = value; },
  },
  eq_highmid_gain: {
    path: "eq.highMidGain",
    read: (node) => node.params.highMidGain,
    apply: (node, value) => { node.params.highMidGain = value; },
  },
  eq_highmid_freq: {
    path: "eq.highMidFreq",
    read: (node) => node.params.highMidFreq,
    apply: (node, value) => { node.params.highMidFreq = value; },
  },
  eq_highmid_q: {
    path: "eq.highMidQ",
    read: (node) => node.params.highMidQ,
    apply: (node, value) => { node.params.highMidQ = value; },
  },
  eq_high_gain: {
    path: "eq.highGain",
    read: (node) => node.params.highGain,
    apply: (node, value) => { node.params.highGain = value; },
  },
  eq_high_freq: {
    path: "eq.highFreq",
    read: (node) => node.params.highFreq,
    apply: (node, value) => { node.params.highFreq = value; },
  },
};

let eqCurveInteraction: EqCurveInteraction | null = null;

const EQ_BAND_PARAM_IDS: { gain: string; freq: string; q: string | null }[] = [
  { gain: "eq_low_gain", freq: "eq_low_freq", q: null },
  { gain: "eq_lowmid_gain", freq: "eq_lowmid_freq", q: "eq_lowmid_q" },
  { gain: "eq_highmid_gain", freq: "eq_highmid_freq", q: "eq_highmid_q" },
  { gain: "eq_high_gain", freq: "eq_high_freq", q: null },
];

const EQ_BAND_LABELS = ["Low", "Low Mid", "High Mid", "High"];

const EQ_BAND_RANGES = [
  { freqMin: 20, freqMax: 500, hasQ: false, qMin: 0.1, qMax: 10 },
  { freqMin: 200, freqMax: 2000, hasQ: true, qMin: 0.1, qMax: 10 },
  { freqMin: 1000, freqMax: 8000, hasQ: true, qMin: 0.1, qMax: 10 },
  { freqMin: 4000, freqMax: 20000, hasQ: false, qMin: 0.1, qMax: 10 },
];

const EQ_BAND_FREQ_DEFAULTS = [100, 400, 2000, 8000];

function buildEqBandConfigs(): EqBandConfig[] {
  return EQ_BAND_PARAM_IDS.map((ids, i) => ({
    freq: readEqParamValue(ids.freq, EQ_BAND_FREQ_DEFAULTS[i]),
    gainDb: readEqParamValue(ids.gain, 0),
    q: ids.q ? readEqParamValue(ids.q, 1.0) : 1.0,
    freqMin: EQ_BAND_RANGES[i].freqMin,
    freqMax: EQ_BAND_RANGES[i].freqMax,
    gainMin: -18,
    gainMax: 18,
    hasQ: EQ_BAND_RANGES[i].hasQ,
    qMin: EQ_BAND_RANGES[i].qMin,
    qMax: EQ_BAND_RANGES[i].qMax,
    label: EQ_BAND_LABELS[i],
  }));
}

function handleEqCurveBandChange(bandIndex: number, freq: number, gainDb: number, q: number): void {
  const ids = EQ_BAND_PARAM_IDS[bandIndex];

  // Update knob display values to stay in sync
  const freqKnob = knobInstances.get(ids.freq);
  if (freqKnob) freqKnob.setValue(freq);

  const gainKnob = knobInstances.get(ids.gain);
  if (gainKnob) gainKnob.setValue(gainDb);

  if (ids.q) {
    const qKnob = knobInstances.get(ids.q);
    if (qKnob) qKnob.setValue(q);
  }

  // Send to plugin for real-time audio feedback
  const gainMapping = GLOBAL_EQ_PARAM_MAP[ids.gain];
  if (gainMapping) {
    sendGlobalChainParam(gainMapping.path, gainDb);
    const eqNode = getPostChainEqNode();
    if (eqNode) gainMapping.apply(eqNode, gainDb);
  }

  const freqMapping = GLOBAL_EQ_PARAM_MAP[ids.freq];
  if (freqMapping) {
    sendGlobalChainParam(freqMapping.path, freq);
    const eqNode = getPostChainEqNode();
    if (eqNode) freqMapping.apply(eqNode, freq);
  }

  if (ids.q) {
    const qMapping = GLOBAL_EQ_PARAM_MAP[ids.q];
    if (qMapping) {
      sendGlobalChainParam(qMapping.path, q);
      const eqNode = getPostChainEqNode();
      if (eqNode) qMapping.apply(eqNode, q);
    }
  }
}

function handleEqCurveBandCommit(bandIndex: number, freq: number, gainDb: number, q: number): void {
  handleEqCurveBandChange(bandIndex, freq, gainDb, q);
  appendLog(`EQ ${EQ_BAND_LABELS[bandIndex]}: ${Math.round(freq)}Hz ${gainDb >= 0 ? "+" : ""}${gainDb.toFixed(1)}dB Q:${q.toFixed(1)}`);
}

function updateEQSectionState(): void {
  const sections = document.querySelectorAll(".eq-section");
  sections.forEach((section) => {
    section.classList.toggle("enabled", eqEnabled);
  });

  const eqControl = document.querySelector(".eq-control");
  if (eqControl) {
    eqControl.classList.toggle("enabled", eqEnabled);
  }
}

function readEqParamValue(paramId: string, fallback: number): number {
  const knobInstance = knobInstances.get(paramId);
  if (knobInstance) {
    return knobInstance.getValue();
  }

  const mapping = GLOBAL_EQ_PARAM_MAP[paramId];
  const eqNode = getPostChainEqNode();
  if (mapping && eqNode) {
    return mapping.read(eqNode);
  }

  return fallback;
}

function updateEqModalVisualization(): void {
  if (eqCurveInteraction) {
    eqCurveInteraction.updateBands(buildEqBandConfigs());
    return;
  }

  // Fallback: non-interactive drawing
  const canvas = document.getElementById("eq-curve-canvas") as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const width = Math.max(1, canvas.clientWidth);
  const height = Math.max(1, canvas.clientHeight);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }

  const bands: EqBand[] = [
    {
      freq: readEqParamValue("eq_low_freq", 100),
      gainDb: readEqParamValue("eq_low_gain", 0),
      q: 1.0,
    },
    {
      freq: readEqParamValue("eq_lowmid_freq", 400),
      gainDb: readEqParamValue("eq_lowmid_gain", 0),
      q: readEqParamValue("eq_lowmid_q", 1.0),
    },
    {
      freq: readEqParamValue("eq_highmid_freq", 2000),
      gainDb: readEqParamValue("eq_highmid_gain", 0),
      q: readEqParamValue("eq_highmid_q", 1.0),
    },
    {
      freq: readEqParamValue("eq_high_freq", 8000),
      gainDb: readEqParamValue("eq_high_gain", 0),
      q: 1.0,
    },
  ];

  drawEqCurve(canvas, bands);
}

export function refreshEqModalVisualization(): void {
  updateEqModalVisualization();
}

function initializeEQControls(): void {
  const eqToggle = document.getElementById("eq-toggle") as HTMLInputElement | null;
  const eqModalToggle = document.getElementById("eq-modal-toggle") as HTMLInputElement | null;

  const applyEqEnabled = (nextValue: boolean, shouldSend: boolean): void => {
    eqEnabled = nextValue;
    if (eqToggle) {
      eqToggle.checked = eqEnabled;
    }
    if (eqModalToggle) {
      eqModalToggle.checked = eqEnabled;
    }
    if (shouldSend) {
      sendGlobalChainParam("eq.enabled", eqEnabled);
      const eqNode = getPostChainEqNode();
      if (eqNode) {
        eqNode.bypassed = !eqEnabled;
      }
      appendLog(`eq.enabled → ${eqEnabled}`);
    }
    updateEQSectionState();
    updateEqModalVisualization();
  };

  if (eqToggle) {
    eqToggle.addEventListener("change", () => {
      applyEqEnabled(eqToggle.checked, true);
    });
  }

  if (eqModalToggle) {
    eqModalToggle.addEventListener("change", () => {
      applyEqEnabled(eqModalToggle.checked, true);
    });
  }

  const onEqValueChange = () => updateEqModalVisualization();
  const onEqValueCommit = (paramId: string, value: number): void => {
    const mapping = GLOBAL_EQ_PARAM_MAP[paramId];
    if (!mapping) {
      return;
    }
    sendGlobalChainParam(mapping.path, value);
    const eqNode = getPostChainEqNode();
    if (eqNode) {
      mapping.apply(eqNode, value);
    }
    appendLog(`${mapping.path} → ${value.toFixed(2)}`);
  };

  // Low Shelf Band
  const lowGainKnob = document.querySelector('.eq-knob[data-param="eq_low_gain"]') as HTMLElement | null;
  if (lowGainKnob) {
    const knobInstance = new GenericKnob({
      knobElement: lowGainKnob,
      paramId: "eq_low_gain",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "eq-low-gain-value",
      sensitivity: 0.1,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_low_gain", value),
      sendParameter: false,
    });
    knobInstances.set("eq_low_gain", knobInstance);
  }

  const lowFreqKnob = document.querySelector('.eq-knob[data-param="eq_low_freq"]') as HTMLElement | null;
  if (lowFreqKnob) {
    const knobInstance = new GenericKnob({
      knobElement: lowFreqKnob,
      paramId: "eq_low_freq",
      minValue: 20.0,
      maxValue: 500.0,
      defaultValue: 100.0,
      displayFormat: (value) => `${Math.round(value)} Hz`,
      valueDisplayId: "eq-low-freq-value",
      sensitivity: 2.0,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_low_freq", value),
      sendParameter: false,
    });
    knobInstances.set("eq_low_freq", knobInstance);
  }

  // Low-Mid Band
  const lowMidGainKnob = document.querySelector('.eq-knob[data-param="eq_lowmid_gain"]') as HTMLElement | null;
  if (lowMidGainKnob) {
    const knobInstance = new GenericKnob({
      knobElement: lowMidGainKnob,
      paramId: "eq_lowmid_gain",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "eq-lowmid-gain-value",
      sensitivity: 0.1,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_lowmid_gain", value),
      sendParameter: false,
    });
    knobInstances.set("eq_lowmid_gain", knobInstance);
  }

  const lowMidFreqKnob = document.querySelector('.eq-knob[data-param="eq_lowmid_freq"]') as HTMLElement | null;
  if (lowMidFreqKnob) {
    const knobInstance = new GenericKnob({
      knobElement: lowMidFreqKnob,
      paramId: "eq_lowmid_freq",
      minValue: 100.0,
      maxValue: 2000.0,
      defaultValue: 500.0,
      displayFormat: (value) => `${Math.round(value)} Hz`,
      valueDisplayId: "eq-lowmid-freq-value",
      sensitivity: 5.0,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_lowmid_freq", value),
      sendParameter: false,
    });
    knobInstances.set("eq_lowmid_freq", knobInstance);
  }

  const lowMidQKnob = document.querySelector('.eq-knob[data-param="eq_lowmid_q"]') as HTMLElement | null;
  if (lowMidQKnob) {
    const knobInstance = new GenericKnob({
      knobElement: lowMidQKnob,
      paramId: "eq_lowmid_q",
      minValue: 0.1,
      maxValue: 10.0,
      defaultValue: 1.0,
      displayFormat: (value) => value.toFixed(1),
      valueDisplayId: "eq-lowmid-q-value",
      sensitivity: 0.05,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_lowmid_q", value),
      sendParameter: false,
    });
    knobInstances.set("eq_lowmid_q", knobInstance);
  }

  // High-Mid Band
  const highMidGainKnob = document.querySelector('.eq-knob[data-param="eq_highmid_gain"]') as HTMLElement | null;
  if (highMidGainKnob) {
    const knobInstance = new GenericKnob({
      knobElement: highMidGainKnob,
      paramId: "eq_highmid_gain",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "eq-highmid-gain-value",
      sensitivity: 0.1,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_highmid_gain", value),
      sendParameter: false,
    });
    knobInstances.set("eq_highmid_gain", knobInstance);
  }

  const highMidFreqKnob = document.querySelector('.eq-knob[data-param="eq_highmid_freq"]') as HTMLElement | null;
  if (highMidFreqKnob) {
    const knobInstance = new GenericKnob({
      knobElement: highMidFreqKnob,
      paramId: "eq_highmid_freq",
      minValue: 500.0,
      maxValue: 8000.0,
      defaultValue: 2000.0,
      displayFormat: (value) => value >= 1000 ? `${(value / 1000).toFixed(1)}k` : `${Math.round(value)} Hz`,
      valueDisplayId: "eq-highmid-freq-value",
      sensitivity: 20.0,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_highmid_freq", value),
      sendParameter: false,
    });
    knobInstances.set("eq_highmid_freq", knobInstance);
  }

  const highMidQKnob = document.querySelector('.eq-knob[data-param="eq_highmid_q"]') as HTMLElement | null;
  if (highMidQKnob) {
    const knobInstance = new GenericKnob({
      knobElement: highMidQKnob,
      paramId: "eq_highmid_q",
      minValue: 0.1,
      maxValue: 10.0,
      defaultValue: 1.0,
      displayFormat: (value) => value.toFixed(1),
      valueDisplayId: "eq-highmid-q-value",
      sensitivity: 0.05,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_highmid_q", value),
      sendParameter: false,
    });
    knobInstances.set("eq_highmid_q", knobInstance);
  }

  // High Shelf Band
  const highGainKnob = document.querySelector('.eq-knob[data-param="eq_high_gain"]') as HTMLElement | null;
  if (highGainKnob) {
    const knobInstance = new GenericKnob({
      knobElement: highGainKnob,
      paramId: "eq_high_gain",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "eq-high-gain-value",
      sensitivity: 0.1,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_high_gain", value),
      sendParameter: false,
    });
    knobInstances.set("eq_high_gain", knobInstance);
  }

  const highFreqKnob = document.querySelector('.eq-knob[data-param="eq_high_freq"]') as HTMLElement | null;
  if (highFreqKnob) {
    const knobInstance = new GenericKnob({
      knobElement: highFreqKnob,
      paramId: "eq_high_freq",
      minValue: 2000.0,
      maxValue: 16000.0,
      defaultValue: 8000.0,
      displayFormat: (value) => `${(value / 1000).toFixed(1)}k`,
      valueDisplayId: "eq-high-freq-value",
      sensitivity: 50.0,
      onValueChange: onEqValueChange,
      onValueCommit: (value) => onEqValueCommit("eq_high_freq", value),
      sendParameter: false,
    });
    knobInstances.set("eq_high_freq", knobInstance);
  }

  // Create interactive EQ curve with draggable handles
  const eqCanvas = document.getElementById("eq-curve-canvas") as HTMLCanvasElement | null;
  if (eqCanvas) {
    eqCurveInteraction = new EqCurveInteraction(
      eqCanvas,
      buildEqBandConfigs(),
      handleEqCurveBandChange,
      handleEqCurveBandCommit
    );
  }

  updateEQSectionState();
  updateEqModalVisualization();
}

export function syncEQControlsFromState(): void {
  const eqNode = getPostChainEqNode();
  const paramValues: Record<string, number> = eqNode
    ? {
        eq_low_gain: eqNode.params.lowGain,
        eq_low_freq: eqNode.params.lowFreq,
        eq_lowmid_gain: eqNode.params.lowMidGain,
        eq_lowmid_freq: eqNode.params.lowMidFreq,
        eq_lowmid_q: eqNode.params.lowMidQ,
        eq_highmid_gain: eqNode.params.highMidGain,
        eq_highmid_freq: eqNode.params.highMidFreq,
        eq_highmid_q: eqNode.params.highMidQ,
        eq_high_gain: eqNode.params.highGain,
        eq_high_freq: eqNode.params.highFreq,
      }
    : {};

  // Sync toggle
  const eqToggle = document.getElementById("eq-toggle") as HTMLInputElement | null;
  const eqModalToggle = document.getElementById("eq-modal-toggle") as HTMLInputElement | null;
  if (eqToggle && eqNode) {
    const enabled = typeof (eqNode as { enabled?: boolean }).enabled === "boolean"
      ? (eqNode as { enabled?: boolean }).enabled === true
      : !eqNode.bypassed;
    eqEnabled = enabled;
    eqToggle.checked = eqEnabled;
    updateEQSectionState();
  }
  if (eqModalToggle && eqNode) {
    eqModalToggle.checked = eqEnabled;
  }

  // Sync all knobs
  const eqKnobs = [
    "eq_low_gain", "eq_low_freq",
    "eq_lowmid_gain", "eq_lowmid_freq", "eq_lowmid_q",
    "eq_highmid_gain", "eq_highmid_freq", "eq_highmid_q",
    "eq_high_gain", "eq_high_freq"
  ];

  eqKnobs.forEach((knobId) => {
    const knobInstance = knobInstances.get(knobId);
    if (knobInstance && typeof paramValues[knobId] === "number") {
      knobInstance.setValue(paramValues[knobId]);
    }
  });

  if (eqCurveInteraction) {
    eqCurveInteraction.updateBands(buildEqBandConfigs());
  }

  updateEqModalVisualization();
}

export { initializeEQControls, initializeIRQualityControls };


function getActivePresetGlobals(): import("./types.js").GlobalSettings | null {
  const activeId = uiState.activePresetId ?? "";
  const activePreset = uiState.presetCache.get(activeId) as import("./types.js").Preset | undefined;
  const globals = (activePreset as any)?.globals ?? (activePreset as any)?.global;
  if (!globals) return null;
  return {
    inputTrim: globals.inputTrim ?? 0,
    outputTrim: globals.outputTrim ?? 0,
    masterVolume: globals.masterVolume ?? globals.outputVolume ?? 1,
    autoLevelInput: globals.autoLevelInput ?? false,
    autoLevelOutput: globals.autoLevelOutput ?? false,
  };
}

function updateActivePresetGlobals(next: Partial<import("./types.js").GlobalSettings>): void {
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

  const syncFromGlobals = (): void => {
    const globals = readAutoLevelFromPreset();
    if (typeof globals.input === "boolean") {
      autoLevelInputEnabled = globals.input;
    }
    if (typeof globals.output === "boolean") {
      autoLevelOutputEnabled = globals.output;
    }
    if (autoIn) autoIn.checked = autoLevelInputEnabled;
    if (autoOut) autoOut.checked = autoLevelOutputEnabled;
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
  if (typeof uiState.globalSignalChain?.autoLevelInput === "boolean") {
    autoLevelInputEnabled = uiState.globalSignalChain.autoLevelInput;
  } else {
    const globals = readAutoLevelFromPreset();
    if (typeof globals.input === "boolean") {
      autoLevelInputEnabled = globals.input;
    }
  }

  if (typeof uiState.globalSignalChain?.autoLevelOutput === "boolean") {
    autoLevelOutputEnabled = uiState.globalSignalChain.autoLevelOutput;
  } else {
    const globals = readAutoLevelFromPreset();
    if (typeof globals.output === "boolean") {
      autoLevelOutputEnabled = globals.output;
    }
  }

  const autoIn = document.getElementById("auto-level-input-toggle") as HTMLInputElement | null;
  const autoOut = document.getElementById("auto-level-output-toggle") as HTMLInputElement | null;
  if (autoIn) autoIn.checked = autoLevelInputEnabled;
  if (autoOut) autoOut.checked = autoLevelOutputEnabled;
  updateAutoLevelKnobStates();
}