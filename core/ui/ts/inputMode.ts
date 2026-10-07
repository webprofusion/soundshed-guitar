/**
 * The input mode control: what the chains are fed (docs/signal-chain.md, "Channel layout").
 *
 * In the standalone app the player picks it and it is stored in app settings; in a DAW the
 * track's bus decides between mono and stereo, and only dual mono, a per-instance choice for a
 * stereo track, is left to pick. The engine reports what its input and output can carry
 * (inputModeChanged), so options they cannot deliver are hidden.
 */
import { appendLog } from "./logging.js";
import { postMessage } from "./bridge.js";
import { updateAppSetting } from "./appSettingsStore.js";
import { uiState } from "./state.js";

type InputModeName = "mono1" | "mono2" | "monoSum" | "stereo" | "dualMono";

interface InputCapabilities {
  hostControlled: boolean;
  inputChannels: number;
  outputChannels: number;
  effectiveMode: "mono" | "stereo" | "dualMono";
  /** In dual mono, effects that still hear both sides: hosted plugins. */
  dualMonoShared: number;
}

let currentInputMode: InputModeName = "mono1";
let inputCapabilities: InputCapabilities = {
  hostControlled: false,
  inputChannels: 2,
  outputChannels: 2,
  effectiveMode: "mono",
  dualMonoShared: 0,
};

// Key names must match core/src/controller/internal/InputModeSettings.h.
const INPUT_CHANNEL_SETTING = "inputChannel.mono";
const MONO_MODE_SETTING = "inputChannel.monoMode";
const DUAL_MONO_SETTING = "inputChannel.dualMono";

const INPUT_MODE_NAMES: readonly InputModeName[] = ["mono1", "mono2", "monoSum", "stereo", "dualMono"];

function isStandaloneUi(): boolean {
  return Boolean(uiState.environment?.standalone || document.body.classList.contains("is-standalone"));
}

function isInputModeName(value: unknown): value is InputModeName {
  return typeof value === "string" && (INPUT_MODE_NAMES as readonly string[]).includes(value);
}

function inputModeFromSettings(monoMode: boolean, inputChannel: number, dualMono: boolean): InputModeName {
  if (monoMode) {
    return inputChannel === 1 ? "mono2" : inputChannel === 2 ? "monoSum" : "mono1";
  }
  return dualMono ? "dualMono" : "stereo";
}

function settingsFromInputMode(mode: InputModeName): { monoMode: boolean; inputChannel: number; dualMono: boolean } {
  switch (mode) {
    case "mono2": return { monoMode: true, inputChannel: 1, dualMono: false };
    case "monoSum": return { monoMode: true, inputChannel: 2, dualMono: false };
    case "stereo": return { monoMode: false, inputChannel: 0, dualMono: false };
    case "dualMono": return { monoMode: false, inputChannel: 0, dualMono: true };
    default: return { monoMode: true, inputChannel: 0, dualMono: false };
  }
}

function getStoredInputMode(): InputModeName | null {
  const settings = uiState.appSettings ?? {};
  const rawMono = settings[MONO_MODE_SETTING];
  const rawChannel = typeof settings[INPUT_CHANNEL_SETTING] === "string"
    ? Number(settings[INPUT_CHANNEL_SETTING])
    : settings[INPUT_CHANNEL_SETTING];
  const rawDual = settings[DUAL_MONO_SETTING];

  if (typeof rawMono !== "boolean" && typeof rawChannel !== "number" && typeof rawDual !== "boolean") {
    return null;
  }

  const channel = rawChannel === 1 || rawChannel === 2 ? rawChannel : 0;
  return inputModeFromSettings(rawMono !== false, channel, rawDual === true);
}

function persistInputMode(mode: InputModeName): void {
  const settings = settingsFromInputMode(mode);
  updateAppSetting(MONO_MODE_SETTING, settings.monoMode);
  updateAppSetting(INPUT_CHANNEL_SETTING, settings.inputChannel);
  updateAppSetting(DUAL_MONO_SETTING, settings.dualMono);
}

/** Whether `mode` can be had with the input and output the engine reports. */
function isInputModeAvailable(mode: InputModeName): boolean {
  const twoInputs = inputCapabilities.inputChannels >= 2;
  const twoOutputs = inputCapabilities.outputChannels >= 2;

  if (inputCapabilities.hostControlled) {
    // The track's bus decides mono or stereo; "stereo" stands for "as the track is".
    return mode === "stereo" || (mode === "dualMono" && twoInputs && twoOutputs);
  }

  if (mode === "mono1") {
    return true;
  }

  if (mode === "dualMono") {
    return twoInputs && twoOutputs;
  }

  return twoInputs;
}

function describeEffectiveMode(): string {
  switch (inputCapabilities.effectiveMode) {
    case "stereo": return "STEREO";
    case "dualMono": return "DUAL MONO";
    default: return "MONO";
  }
}

function renderInputModeControl(): void {
  const select = document.getElementById("input-mode-select") as HTMLSelectElement | null;
  const status = document.getElementById("input-mode-status");

  if (select) {
    for (const option of Array.from(select.options)) {
      const mode = option.value as InputModeName;
      const available = isInputModeAvailable(mode);
      option.hidden = !available;
      option.disabled = !available;

      if (mode === "stereo") {
        option.textContent = inputCapabilities.hostControlled
          ? (inputCapabilities.inputChannels >= 2 ? "Track (stereo)" : "Track (mono)")
          : "Stereo";
      }
    }

    select.value = inputCapabilities.hostControlled && currentInputMode !== "dualMono" ? "stereo" : currentInputMode;
    // A DAW's mono track leaves nothing to choose.
    select.disabled = inputCapabilities.hostControlled && !isInputModeAvailable("dualMono");
  }

  if (status) {
    const shared = inputCapabilities.effectiveMode === "dualMono" ? inputCapabilities.dualMonoShared : 0;
    status.textContent = shared > 0 ? `${describeEffectiveMode()} *` : describeEffectiveMode();
    status.title = shared > 0
      ? `${shared} hosted plugin${shared === 1 ? "" : "s"} hear${shared === 1 ? "s" : ""} both inputs: a plugin cannot be doubled up for dual mono.`
      : inputCapabilities.hostControlled
        ? "Set by the track: a mono track is processed in mono, a stereo track in stereo."
        : inputCapabilities.inputChannels < 2
          ? "The audio device has one input, so the chain runs in mono."
          : "";
  }
}

function sendInputModeToPlugin(): void {
  const settings = settingsFromInputMode(currentInputMode);
  postMessage({
    type: "setInputMode",
    mode: currentInputMode,
    monoMode: settings.monoMode,
    inputChannel: settings.inputChannel,
    dualMono: settings.dualMono,
  });
  appendLog(`Input mode: ${currentInputMode}`);
}

/** Applies the stored mode (standalone only: in a DAW the instance's own state carries it). */
export function applyStoredInputChannel(): void {
  if (!isStandaloneUi()) {
    return;
  }

  const stored = getStoredInputMode();
  if (stored !== null) {
    currentInputMode = stored;
  }

  renderInputModeControl();
  sendInputModeToPlugin();
}

export function initializeInputModeControls(): void {
  const select = document.getElementById("input-mode-select") as HTMLSelectElement | null;

  if (select) {
    select.addEventListener("change", () => {
      if (!isInputModeName(select.value)) {
        return;
      }

      currentInputMode = select.value;

      // In a DAW the choice belongs to this instance and is saved with the project, not in app
      // settings shared by every instance.
      if (isStandaloneUi()) {
        persistInputMode(currentInputMode);
      }

      renderInputModeControl();
      sendInputModeToPlugin();
    });
  }

  if (isStandaloneUi()) {
    applyStoredInputChannel();
  } else {
    renderInputModeControl();
    // Ask for the instance's state; the reply fills the control in.
    postMessage({ type: "setInputMode" });
  }
}

export interface InputModeChangedPayload {
  mode?: string;
  monoMode?: boolean;
  inputChannel?: number;
  dualMono?: boolean;
  effectiveMode?: string;
  hostControlled?: boolean;
  inputChannels?: number;
  outputChannels?: number;
  dualMonoShared?: number;
}

export function handleInputModeChanged(payload: InputModeChangedPayload): void {
  if (isInputModeName(payload.mode)) {
    currentInputMode = payload.mode;
  } else if (typeof payload.monoMode === "boolean") {
    currentInputMode = inputModeFromSettings(payload.monoMode, payload.inputChannel ?? 0, payload.dualMono === true);
  }

  const effective = payload.effectiveMode === "stereo" || payload.effectiveMode === "dualMono"
    ? payload.effectiveMode
    : "mono";
  inputCapabilities = {
    hostControlled: payload.hostControlled ?? inputCapabilities.hostControlled,
    inputChannels: typeof payload.inputChannels === "number" ? payload.inputChannels : inputCapabilities.inputChannels,
    outputChannels: typeof payload.outputChannels === "number" ? payload.outputChannels : inputCapabilities.outputChannels,
    effectiveMode: effective,
    dualMonoShared: typeof payload.dualMonoShared === "number" ? payload.dualMonoShared : 0,
  };

  renderInputModeControl();
}

