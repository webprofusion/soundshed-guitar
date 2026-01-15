import { appendLog } from "./logging.js";
import { setAppSetting } from "./bridge.js";
import { postMessage, setParameter } from "./bridge.js";
import { uiState } from "./state.js";

interface KnobConfig {
  knobElement: HTMLElement;
  paramId: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  displayFormat: (value: number) => string;
  valueDisplayId?: string;
  sensitivity?: number;
  onValueChange?: (value: number) => void;
}

class GenericKnob {
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
    
    this.valueDisplay = config.valueDisplayId 
      ? document.getElementById(config.valueDisplayId) 
      : null;

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
    setParameter(this.paramId, this.defaultValue);
    appendLog(`${this.paramId} → ${this.defaultValue.toFixed(2)} (reset to default)`);
    
    if (this.onValueChange) {
      this.onValueChange(this.defaultValue);
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
    setParameter(this.paramId, this.currentValue);
    
    if (this.onValueChange) {
      this.onValueChange(this.currentValue);
    }
  }

  private onMouseUp(): void {
    if (!this.isDragging) return;
    
    this.isDragging = false;
    // Final value send and log on release
    setParameter(this.paramId, this.currentValue);
    appendLog(`${this.paramId} → ${this.currentValue.toFixed(2)}`);
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

function initializeDoublerControls(): void {
  const doublerToggle = document.getElementById("doubler-toggle") as HTMLInputElement | null;

  if (doublerToggle) {
    doublerToggle.addEventListener("change", () => {
      const enabled = doublerToggle.checked ? 1.0 : 0.0;
      setParameter("doubler_enabled", enabled);
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
    });
    knobInstances.set("doubler_delay", delayKnobInstance);
  }
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
      onValueChange: (value) => {
        // Snap to integer values for semitones and send to plugin
        const rounded = Math.round(value);
        if (Math.abs(value - rounded) > 0.01) {
          transposeKnobInstance.setValue(rounded);
        }
        // Always send the rounded integer value to the plugin
        setParameter("transpose", rounded);
      },
    });
    knobInstances.set("transpose", transposeKnobInstance);
  }
}

function initializeGateControls(): void {
  const gateToggle = document.getElementById("gate-toggle") as HTMLInputElement | null;

  if (gateToggle) {
    gateToggle.addEventListener("change", () => {
      const enabled = gateToggle.checked ? 1.0 : 0.0;
      setParameter("gate_enabled", enabled);
      appendLog(`gate_enabled → ${enabled}`);
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
    });
    knobInstances.set("gate_threshold", thresholdKnobInstance);
  }
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
  if (gateToggle && typeof paramValues.gate_enabled === "number") {
    gateToggle.checked = paramValues.gate_enabled > 0.5;
  }

  // Sync threshold knob
  const thresholdKnobInstance = knobInstances.get("gate_threshold");
  if (thresholdKnobInstance && typeof paramValues.gate_threshold === "number") {
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
  initializeSimpleCabControls();
  initializeIRQualityControls();
  initializeEQControls();
  initializeDelayControls();
  initializeReverbControls();
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
  if (doublerToggle && typeof paramValues.doubler_enabled === "number") {
    doublerToggle.checked = paramValues.doubler_enabled > 0.5;
  }

  // Sync delay knob using GenericKnob instance
  const delayKnobInstance = knobInstances.get("doubler_delay");
  if (delayKnobInstance && typeof paramValues.doubler_delay === "number") {
    delayKnobInstance.setValue(paramValues.doubler_delay);
  }

  // Sync input level knob
  const inputKnobInstance = knobInstances.get("input_trim");
  if (inputKnobInstance && typeof paramValues.input_trim === "number") {
    inputKnobInstance.setValue(paramValues.input_trim);
  }

  // Sync output level knob
  const outputKnobInstance = knobInstances.get("output_trim");
  if (outputKnobInstance && typeof paramValues.output_trim === "number") {
    outputKnobInstance.setValue(paramValues.output_trim);
  }

  // Sync transpose knob
  const transposeKnobInstance = knobInstances.get("transpose");
  if (transposeKnobInstance && typeof paramValues.transpose === "number") {
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
  syncSimpleCabControlsFromState();
  syncIRQualityFromState();
  syncEQControlsFromState();
  syncDelayControlsFromState();
  syncReverbControlsFromState();
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
  const inputModeRadios = document.querySelectorAll('input[name="input-mode"]') as NodeListOf<HTMLInputElement>;
  const inputChannelSelector = document.getElementById("input-channel-selector");
  const inputChannelSelect = document.getElementById("input-channel-select") as HTMLSelectElement | null;

  // Show/hide channel selector based on mono mode
  function updateChannelSelectorVisibility(): void {
    if (inputChannelSelector) {
      if (currentMonoMode) {
        inputChannelSelector.classList.remove("hidden");
      } else {
        inputChannelSelector.classList.add("hidden");
      }
    }
  }

  // Initialize radio button listeners
  inputModeRadios.forEach((radio) => {
    radio.addEventListener("change", () => {
      if (radio.checked) {
        currentMonoMode = radio.value === "mono";
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
      }
    });
  });

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

  // Set initial state
  updateChannelSelectorVisibility();
  applyStoredInputChannel();
  sendInputModeToPlugin();
}

export function handleInputModeChanged(monoMode: boolean, inputChannel: number): void {
  currentMonoMode = monoMode;
  currentInputChannel = inputChannel;

  // Update radio buttons
  const inputModeRadios = document.querySelectorAll('input[name="input-mode"]') as NodeListOf<HTMLInputElement>;
  inputModeRadios.forEach((radio) => {
    radio.checked = (radio.value === "mono") === monoMode;
  });

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

// Simple Cab controls
let simpleCabEnabled = false;

function updateSimpleCabSectionState(): void {
  const section = document.querySelector(".simplecab-section");
  if (section) {
    section.classList.toggle("enabled", simpleCabEnabled);
  }
}

function sendSimpleCabStateToPlugin(): void {
  setParameter("simplecab_enabled", simpleCabEnabled ? 1.0 : 0.0);
  appendLog(`Simple Cab: ${simpleCabEnabled ? "ON" : "OFF"}`);
}

function initializeSimpleCabControls(): void {
  const simpleCabToggle = document.getElementById("simplecab-toggle") as HTMLInputElement | null;
  
  if (simpleCabToggle) {
    simpleCabToggle.addEventListener("change", () => {
      simpleCabEnabled = simpleCabToggle.checked;
      updateSimpleCabSectionState();
      sendSimpleCabStateToPlugin();
    });
  }

  // Initialize Bass knob
  const bassKnob = document.querySelector('.knob[data-param="simplecab-bass"]') as HTMLElement | null;
  if (bassKnob) {
    const bassKnobInstance = new GenericKnob({
      knobElement: bassKnob,
      paramId: "simplecab_bass",
      minValue: 0.0,
      maxValue: 1.0,
      defaultValue: 0.5,
      displayFormat: (value) => `${Math.round(value * 100)}%`,
      valueDisplayId: "simplecab-bass-value",
      sensitivity: 0.005,
    });
    knobInstances.set("simplecab_bass", bassKnobInstance);
  }

  // Initialize Presence knob
  const presenceKnob = document.querySelector('.knob[data-param="simplecab-presence"]') as HTMLElement | null;
  if (presenceKnob) {
    const presenceKnobInstance = new GenericKnob({
      knobElement: presenceKnob,
      paramId: "simplecab_presence",
      minValue: 0.0,
      maxValue: 1.0,
      defaultValue: 0.5,
      displayFormat: (value) => `${Math.round(value * 100)}%`,
      valueDisplayId: "simplecab-presence-value",
      sensitivity: 0.005,
    });
    knobInstances.set("simplecab_presence", presenceKnobInstance);
  }

  // Initialize Brightness knob
  const brightnessKnob = document.querySelector('.knob[data-param="simplecab-brightness"]') as HTMLElement | null;
  if (brightnessKnob) {
    const brightnessKnobInstance = new GenericKnob({
      knobElement: brightnessKnob,
      paramId: "simplecab_brightness",
      minValue: 0.0,
      maxValue: 1.0,
      defaultValue: 0.5,
      displayFormat: (value) => `${Math.round(value * 100)}%`,
      valueDisplayId: "simplecab-brightness-value",
      sensitivity: 0.005,
    });
    knobInstances.set("simplecab_brightness", brightnessKnobInstance);
  }

  // Initial state update
  updateSimpleCabSectionState();
}

export function syncSimpleCabControlsFromState(): void {
  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  // Sync toggle
  const simpleCabToggle = document.getElementById("simplecab-toggle") as HTMLInputElement | null;
  if (simpleCabToggle && typeof paramValues.simplecab_enabled === "number") {
    simpleCabEnabled = paramValues.simplecab_enabled > 0.5;
    simpleCabToggle.checked = simpleCabEnabled;
    updateSimpleCabSectionState();
  }

  // Sync knobs
  const bassKnobInstance = knobInstances.get("simplecab_bass");
  if (bassKnobInstance && typeof paramValues.simplecab_bass === "number") {
    bassKnobInstance.setValue(paramValues.simplecab_bass);
  }

  const presenceKnobInstance = knobInstances.get("simplecab_presence");
  if (presenceKnobInstance && typeof paramValues.simplecab_presence === "number") {
    presenceKnobInstance.setValue(paramValues.simplecab_presence);
  }

  const brightnessKnobInstance = knobInstances.get("simplecab_brightness");
  if (brightnessKnobInstance && typeof paramValues.simplecab_brightness === "number") {
    brightnessKnobInstance.setValue(paramValues.simplecab_brightness);
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

function updateEQSectionState(): void {
  const section = document.querySelector(".eq-section");
  if (section) {
    section.classList.toggle("enabled", eqEnabled);
  }
}

function initializeEQControls(): void {
  const eqToggle = document.getElementById("eq-toggle") as HTMLInputElement | null;

  if (eqToggle) {
    eqToggle.addEventListener("change", () => {
      eqEnabled = eqToggle.checked;
      setParameter("eq_enabled", eqEnabled ? 1.0 : 0.0);
      appendLog(`eq_enabled → ${eqEnabled ? 1.0 : 0.0}`);
      updateEQSectionState();
    });
  }

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
    });
    knobInstances.set("eq_high_freq", knobInstance);
  }

  updateEQSectionState();
}

export function syncEQControlsFromState(): void {
  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  // Sync toggle
  const eqToggle = document.getElementById("eq-toggle") as HTMLInputElement | null;
  if (eqToggle && typeof paramValues.eq_enabled === "number") {
    eqEnabled = paramValues.eq_enabled > 0.5;
    eqToggle.checked = eqEnabled;
    updateEQSectionState();
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
}

export { initializeSimpleCabControls, initializeEQControls, initializeIRQualityControls, initializeDelayControls, initializeReverbControls };

// ===== Delay Effect Controls =====
let delayEnabled = false;

function updateDelaySectionState(): void {
  const section = document.querySelector(".effect-section:has(#delay-toggle)");
  if (section) {
    section.classList.toggle("enabled", delayEnabled);
  }
}

function initializeDelayControls(): void {
  const delayToggle = document.getElementById("delay-toggle") as HTMLInputElement | null;

  if (delayToggle) {
    delayToggle.addEventListener("change", () => {
      delayEnabled = delayToggle.checked;
      setParameter("delay_enabled", delayEnabled ? 1.0 : 0.0);
      appendLog(`delay_enabled → ${delayEnabled ? 1.0 : 0.0}`);
      updateDelaySectionState();
    });
  }

  // Delay Time knob
  const delayTimeKnob = document.querySelector('.effect-knob[data-param="delay_time"]') as HTMLElement | null;
  if (delayTimeKnob) {
    const knobInstance = new GenericKnob({
      knobElement: delayTimeKnob,
      paramId: "delay_time",
      minValue: 1.0,
      maxValue: 2000.0,
      defaultValue: 300.0,
      displayFormat: (value) => `${Math.round(value)} ms`,
      valueDisplayId: "delay-time-value",
      sensitivity: 5.0,
    });
    knobInstances.set("delay_time", knobInstance);
  }

  // Delay Feedback knob
  const delayFeedbackKnob = document.querySelector('.effect-knob[data-param="delay_feedback"]') as HTMLElement | null;
  if (delayFeedbackKnob) {
    const knobInstance = new GenericKnob({
      knobElement: delayFeedbackKnob,
      paramId: "delay_feedback",
      minValue: 0.0,
      maxValue: 95.0,
      defaultValue: 30.0,
      displayFormat: (value) => `${Math.round(value)}%`,
      valueDisplayId: "delay-feedback-value",
      sensitivity: 0.5,
    });
    knobInstances.set("delay_feedback", knobInstance);
  }

  // Delay Mix knob
  const delayMixKnob = document.querySelector('.effect-knob[data-param="delay_mix"]') as HTMLElement | null;
  if (delayMixKnob) {
    const knobInstance = new GenericKnob({
      knobElement: delayMixKnob,
      paramId: "delay_mix",
      minValue: 0.0,
      maxValue: 100.0,
      defaultValue: 30.0,
      displayFormat: (value) => `${Math.round(value)}%`,
      valueDisplayId: "delay-mix-value",
      sensitivity: 0.5,
    });
    knobInstances.set("delay_mix", knobInstance);
  }

  updateDelaySectionState();
}

export function syncDelayControlsFromState(): void {
  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  // Sync toggle
  const delayToggle = document.getElementById("delay-toggle") as HTMLInputElement | null;
  if (delayToggle && typeof paramValues.delay_enabled === "number") {
    delayEnabled = paramValues.delay_enabled > 0.5;
    delayToggle.checked = delayEnabled;
    updateDelaySectionState();
  }

  // Sync all knobs
  const delayKnobs = ["delay_time", "delay_feedback", "delay_mix"];
  delayKnobs.forEach((knobId) => {
    const knobInstance = knobInstances.get(knobId);
    if (knobInstance && typeof paramValues[knobId] === "number") {
      knobInstance.setValue(paramValues[knobId]);
    }
  });
}

// ===== Reverb Effect Controls =====
let reverbEnabled = false;

function updateReverbSectionState(): void {
  const section = document.querySelector(".effect-section:has(#reverb-toggle)");
  if (section) {
    section.classList.toggle("enabled", reverbEnabled);
  }
}

function initializeReverbControls(): void {
  const reverbToggle = document.getElementById("reverb-toggle") as HTMLInputElement | null;

  if (reverbToggle) {
    reverbToggle.addEventListener("change", () => {
      reverbEnabled = reverbToggle.checked;
      setParameter("reverb_enabled", reverbEnabled ? 1.0 : 0.0);
      appendLog(`reverb_enabled → ${reverbEnabled ? 1.0 : 0.0}`);
      updateReverbSectionState();
    });
  }

  // Reverb Decay knob
  const reverbDecayKnob = document.querySelector('.effect-knob[data-param="reverb_decay"]') as HTMLElement | null;
  if (reverbDecayKnob) {
    const knobInstance = new GenericKnob({
      knobElement: reverbDecayKnob,
      paramId: "reverb_decay",
      minValue: 0.1,
      maxValue: 0.99,
      defaultValue: 0.5,
      displayFormat: (value) => value.toFixed(2),
      valueDisplayId: "reverb-decay-value",
      sensitivity: 0.005,
    });
    knobInstances.set("reverb_decay", knobInstance);
  }

  // Reverb Damping knob
  const reverbDampingKnob = document.querySelector('.effect-knob[data-param="reverb_damping"]') as HTMLElement | null;
  if (reverbDampingKnob) {
    const knobInstance = new GenericKnob({
      knobElement: reverbDampingKnob,
      paramId: "reverb_damping",
      minValue: 0.0,
      maxValue: 1.0,
      defaultValue: 0.5,
      displayFormat: (value) => value.toFixed(2),
      valueDisplayId: "reverb-damping-value",
      sensitivity: 0.005,
    });
    knobInstances.set("reverb_damping", knobInstance);
  }

  // Reverb Mix knob
  const reverbMixKnob = document.querySelector('.effect-knob[data-param="reverb_mix"]') as HTMLElement | null;
  if (reverbMixKnob) {
    const knobInstance = new GenericKnob({
      knobElement: reverbMixKnob,
      paramId: "reverb_mix",
      minValue: 0.0,
      maxValue: 100.0,
      defaultValue: 30.0,
      displayFormat: (value) => `${Math.round(value)}%`,
      valueDisplayId: "reverb-mix-value",
      sensitivity: 0.5,
    });
    knobInstances.set("reverb_mix", knobInstance);
  }

  updateReverbSectionState();
}

export function syncReverbControlsFromState(): void {
  const paramValues: Record<string, number> = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      if (typeof param.value === "number") {
        paramValues[param.id] = param.value;
      }
    });
  }

  // Sync toggle
  const reverbToggle = document.getElementById("reverb-toggle") as HTMLInputElement | null;
  if (reverbToggle && typeof paramValues.reverb_enabled === "number") {
    reverbEnabled = paramValues.reverb_enabled > 0.5;
    reverbToggle.checked = reverbEnabled;
    updateReverbSectionState();
  }

  // Sync all knobs
  const reverbKnobs = ["reverb_decay", "reverb_damping", "reverb_mix"];
  reverbKnobs.forEach((knobId) => {
    const knobInstance = knobInstances.get(knobId);
    if (knobInstance && typeof paramValues[knobId] === "number") {
      knobInstance.setValue(paramValues[knobId]);
    }
  });
}

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
    const globals = getActivePresetGlobals();
    autoLevelInputEnabled = globals?.autoLevelInput ?? false;
    autoLevelOutputEnabled = globals?.autoLevelOutput ?? false;
    if (autoIn) autoIn.checked = autoLevelInputEnabled;
    if (autoOut) autoOut.checked = autoLevelOutputEnabled;
  };

  syncFromGlobals();

  if (autoIn) {
    autoIn.addEventListener("change", () => {
      autoLevelInputEnabled = autoIn.checked;
      updateActivePresetGlobals({ autoLevelInput: autoLevelInputEnabled });
      sendAutoLevelToPlugin();
    });
  }

  if (autoOut) {
    autoOut.addEventListener("change", () => {
      autoLevelOutputEnabled = autoOut.checked;
      updateActivePresetGlobals({ autoLevelOutput: autoLevelOutputEnabled });
      sendAutoLevelToPlugin();
    });
  }
}

export function syncAutoLevelControlsFromState(): void {
  const globals = getActivePresetGlobals();
  autoLevelInputEnabled = globals?.autoLevelInput ?? false;
  autoLevelOutputEnabled = globals?.autoLevelOutput ?? false;

  const autoIn = document.getElementById("auto-level-input-toggle") as HTMLInputElement | null;
  const autoOut = document.getElementById("auto-level-output-toggle") as HTMLInputElement | null;
  if (autoIn) autoIn.checked = autoLevelInputEnabled;
  if (autoOut) autoOut.checked = autoLevelOutputEnabled;
}