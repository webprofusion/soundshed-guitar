import { appendLog } from "./logging.js";
import { setParameter } from "./bridge.js";
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
    document.addEventListener("mousemove", (e) => this.onMouseMove(e));
    document.addEventListener("mouseup", () => this.onMouseUp());
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
  }

  private onMouseUp(): void {
    if (!this.isDragging) return;
    
    this.isDragging = false;
    setParameter(this.paramId, this.currentValue);
    appendLog(`${this.paramId} → ${this.currentValue.toFixed(2)}`);
    
    if (this.onValueChange) {
      this.onValueChange(this.currentValue);
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
      paramId: "input_level",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "input-value",
      sensitivity: 0.1,
    });
    knobInstances.set("input_level", inputKnobInstance);
  }

  // Initialize Output Level knob
  const outputKnob = document.querySelector('.knob[data-param="output"]') as HTMLElement | null;
  if (outputKnob) {
    const outputKnobInstance = new GenericKnob({
      knobElement: outputKnob,
      paramId: "output_level",
      minValue: -12.0,
      maxValue: 12.0,
      defaultValue: 0.0,
      displayFormat: (value) => `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`,
      valueDisplayId: "output-value",
      sensitivity: 0.1,
    });
    knobInstances.set("output_level", outputKnobInstance);
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
        // Snap to integer values for semitones
        const rounded = Math.round(value);
        if (Math.abs(value - rounded) > 0.01) {
          transposeKnobInstance.setValue(rounded);
        }
      },
    });
    knobInstances.set("transpose", transposeKnobInstance);
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
  const inputKnobInstance = knobInstances.get("input_level");
  if (inputKnobInstance && typeof paramValues.input_level === "number") {
    inputKnobInstance.setValue(paramValues.input_level);
  }

  // Sync output level knob
  const outputKnobInstance = knobInstances.get("output_level");
  if (outputKnobInstance && typeof paramValues.output_level === "number") {
    outputKnobInstance.setValue(paramValues.output_level);
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
}

// Input mode state
let currentMonoMode = true;
let currentInputChannel = 1;

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
        sendInputModeToPlugin();
      }
    });
  });

  // Initialize channel select listener
  if (inputChannelSelect) {
    inputChannelSelect.addEventListener("change", () => {
      currentInputChannel = parseInt(inputChannelSelect.value, 10);
      sendInputModeToPlugin();
    });
  }

  // Set initial state
  updateChannelSelectorVisibility();
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
