/**
 * The input mode control: what the chains are fed (docs/signal-chain.md, "Channel Layout").
 *
 * It is a quiet dropdown under the IN knob's readout: a trigger showing the current mode's name,
 * and a flyout listing the modes with their routing icons. In the standalone app the
 * player picks it and it is stored in app settings; in a DAW the track's bus decides between mono
 * and stereo, and only dual mono, a per-instance choice for a stereo track, is left to pick. The
 * engine reports what its input and output can carry (inputModeChanged); a mode they cannot
 * deliver stays listed, greyed, with what it needs.
 */
import { appendLog } from "./logging.js";
import { postMessage } from "./bridge.js";
import { updateAppSetting } from "./appSettingsStore.js";
import { uiState } from "./state.js";
import { inputModeIcon, type InputModeIconName } from "./inputModeIcons.js";
import { getUiZoom } from "./pointerDrag.js";

type InputModeName = "mono1" | "mono2" | "monoSum" | "stereo" | "dualMono";
type EffectiveMode = "mono" | "stereo" | "dualMono";

interface InputCapabilities {
  hostControlled: boolean;
  inputChannels: number;
  outputChannels: number;
  effectiveMode: EffectiveMode;
  /** In dual mono, effects that still hear both sides: hosted plugins. */
  dualMonoShared: number;
}

/**
 * How a mode is shown: the trigger has its name, and the flyout a row of icon, name and
 * description. One name for both, so what was picked reads the same once chosen.
 */
interface ModePresentation {
  icon: InputModeIconName;
  name: string;
  description: string;
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

const CHECK_ICON =
  '<svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M3.5 8.5 6.5 11.5 12.5 4.5" ' +
  'fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"/></svg>';

const VIEWPORT_MARGIN = 8;
const FLYOUT_GAP = 6;

let controlsInitialized = false;
let repositionFrame = 0;

const triggerEl = (): HTMLButtonElement | null => document.getElementById("input-mode-trigger") as HTMLButtonElement | null;
const flyoutEl = (): HTMLDivElement | null => document.getElementById("input-mode-flyout") as HTMLDivElement | null;

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

// ── What can be had ──────────────────────────────────────────────────────────

/** The modes worth listing: in a DAW the track stands in for the three mono modes and stereo. */
function listedModes(): InputModeName[] {
  return inputCapabilities.hostControlled ? ["stereo", "dualMono"] : [...INPUT_MODE_NAMES];
}

/** Why `mode` cannot be had with the input and output the engine reports, or null if it can. */
function unavailableReason(mode: InputModeName): string | null {
  const twoInputs = inputCapabilities.inputChannels >= 2;
  const twoOutputs = inputCapabilities.outputChannels >= 2;

  if (inputCapabilities.hostControlled) {
    if (mode === "dualMono" && !(twoInputs && twoOutputs)) {
      return twoInputs ? "Needs a stereo output" : "Needs a stereo track";
    }
    return null;
  }

  if (mode === "mono1") {
    return null;
  }

  if (mode === "dualMono" && !(twoInputs && twoOutputs)) {
    return twoInputs ? "Needs two outputs" : "Needs two inputs and two outputs";
  }

  return twoInputs ? null : "Needs a second input";
}

/** The mode the trigger shows: in a DAW anything but dual mono is "as the track is". */
function displayedMode(): InputModeName {
  return inputCapabilities.hostControlled && currentInputMode !== "dualMono" ? "stereo" : currentInputMode;
}

function presentMode(mode: InputModeName): ModePresentation {
  switch (mode) {
    case "mono1":
      return { icon: "mono1", name: "Mono In 1", description: "Input 1 on both sides" };
    case "mono2":
      return { icon: "mono2", name: "Mono In 2", description: "Input 2 on both sides" };
    case "monoSum":
      return { icon: "monoSum", name: "Mono 1+2", description: "Inputs 1 and 2 summed" };
    case "dualMono":
      return { icon: "dualMono", name: "Dual mono", description: "A separate rig per input" };
    case "stereo":
      if (inputCapabilities.hostControlled) {
        return inputCapabilities.inputChannels >= 2
          ? { icon: "stereo", name: "Track", description: "Stereo, as the track is" }
          : { icon: "mono", name: "Track", description: "Mono, as the track is" };
      }
      return { icon: "stereo", name: "Stereo", description: "Input 1 left, input 2 right" };
  }
}

/** What `mode` runs as when nothing gets in its way. */
function intendedEffectiveMode(mode: InputModeName): EffectiveMode {
  if (mode === "dualMono") {
    return "dualMono";
  }
  if (mode === "stereo") {
    return inputCapabilities.hostControlled && inputCapabilities.inputChannels < 2 ? "mono" : "stereo";
  }
  return "mono";
}

function effectiveModeName(mode: EffectiveMode): string {
  switch (mode) {
    case "stereo": return "Stereo";
    case "dualMono": return "Dual mono";
    default: return "Mono";
  }
}

function sharedPluginCount(): number {
  return inputCapabilities.effectiveMode === "dualMono" ? inputCapabilities.dualMonoShared : 0;
}

/** The one thing worth saying about the input as it stands, or "" when the mode speaks for itself. */
function statusNote(): string {
  const shared = sharedPluginCount();
  if (shared > 0) {
    return `${shared} hosted plugin${shared === 1 ? "" : "s"} hear${shared === 1 ? "s" : ""} both inputs: a plugin cannot be doubled up for dual mono.`;
  }

  if (inputCapabilities.hostControlled) {
    return "Your DAW track decides mono or stereo. Dual mono runs a stereo track as two separate rigs.";
  }

  if (inputCapabilities.inputChannels < 2) {
    return "The audio device has one input, so the chain runs in mono.";
  }

  if (currentInputMode === "dualMono" && inputCapabilities.outputChannels < 2) {
    return "Dual mono needs two outputs, so both inputs are summed to mono.";
  }

  return "";
}

// ── Trigger ──────────────────────────────────────────────────────────────────

function renderTrigger(): void {
  const trigger = triggerEl();
  if (!trigger) {
    return;
  }

  const mode = displayedMode();
  const presentation = presentMode(mode);
  const fallback = intendedEffectiveMode(mode) !== inputCapabilities.effectiveMode;

  const label = trigger.querySelector<HTMLElement>(".input-mode-trigger-label");
  if (label) {
    label.textContent = presentation.name;
  }

  trigger.classList.toggle("is-fallback", fallback);
  trigger.classList.toggle("has-shared-plugins", sharedPluginCount() > 0);

  const running = fallback ? ` Running in ${effectiveModeName(inputCapabilities.effectiveMode).toLowerCase()}.` : "";
  const note = statusNote();
  trigger.title = `Input: ${presentation.name}. ${presentation.description}.${running}${note ? ` ${note}` : ""}`;
  trigger.setAttribute("aria-label", `Input mode: ${presentation.name}`);
}

// ── Flyout ───────────────────────────────────────────────────────────────────

function isFlyoutOpen(): boolean {
  const flyout = flyoutEl();
  return Boolean(flyout && !flyout.hidden);
}

function buildOption(mode: InputModeName, selected: InputModeName): HTMLButtonElement {
  const presentation = presentMode(mode);
  const reason = unavailableReason(mode);
  const checked = mode === selected;

  const option = document.createElement("button");
  option.type = "button";
  option.className = "input-mode-option";
  option.dataset.mode = mode;
  option.setAttribute("role", "menuitemradio");
  option.setAttribute("aria-checked", String(checked));
  option.disabled = reason !== null;
  option.tabIndex = -1;

  const icon = document.createElement("span");
  icon.className = "input-mode-option-icon";
  icon.innerHTML = inputModeIcon(presentation.icon);

  const text = document.createElement("span");
  text.className = "input-mode-option-text";
  const name = document.createElement("span");
  name.className = "input-mode-option-name";
  name.textContent = presentation.name;
  const description = document.createElement("span");
  description.className = "input-mode-option-description";
  description.textContent = reason ?? presentation.description;
  text.append(name, description);

  const check = document.createElement("span");
  check.className = "input-mode-option-check";
  check.innerHTML = checked ? CHECK_ICON : "";

  option.append(icon, text, check);
  return option;
}

function renderFlyout(): void {
  const flyout = flyoutEl();
  if (!flyout) {
    return;
  }

  const header = document.createElement("div");
  header.className = "input-mode-flyout-header";
  const title = document.createElement("span");
  title.className = "input-mode-flyout-title";
  title.textContent = "Input mode";
  header.append(title);

  // Said only when it is not what the ticked mode would run: Stereo on a one-input device.
  if (intendedEffectiveMode(displayedMode()) !== inputCapabilities.effectiveMode) {
    const running = document.createElement("span");
    running.className = "input-mode-flyout-running";
    running.textContent = `Running ${effectiveModeName(inputCapabilities.effectiveMode).toLowerCase()}`;
    header.append(running);
  }

  const options = document.createElement("div");
  options.className = "input-mode-options";
  const selected = displayedMode();
  for (const mode of listedModes()) {
    options.append(buildOption(mode, selected));
  }

  const parts: HTMLElement[] = [header, options];
  const noteText = statusNote();
  if (noteText) {
    const note = document.createElement("p");
    note.className = "input-mode-note";
    note.textContent = noteText;
    parts.push(note);
  }

  // A redraw while the list has focus (the engine answering a choice) keeps it on the same mode.
  const focusedMode = (document.activeElement as HTMLElement | null)?.closest<HTMLElement>(".input-mode-option")?.dataset.mode;
  flyout.replaceChildren(...parts);
  if (focusedMode) {
    Array.from(flyout.querySelectorAll<HTMLButtonElement>(".input-mode-option"))
      .find((option) => option.dataset.mode === focusedMode)
      ?.focus();
  }
}

/**
 * Fixed to the viewport and hung from the body (bindFlyoutEvents), so neither the bar's own
 * clipping nor the scrolling sheet it becomes at compact density cuts it off. Centred under the
 * trigger, kept inside the window, and above the trigger when there is more room there. The
 * app's zoom is CSS zoom on the body, which scales a fixed element's left and top, so the
 * trigger's client rect and the window are brought into the body's own pixels first.
 */
function positionFlyout(): void {
  const flyout = flyoutEl();
  const trigger = triggerEl();
  if (!flyout || !trigger || flyout.hidden) {
    return;
  }

  const zoom = getUiZoom();
  const client = trigger.getBoundingClientRect();
  const rect = { left: client.left / zoom, top: client.top / zoom, bottom: client.bottom / zoom, width: client.width / zoom };
  const viewportWidth = window.innerWidth / zoom;
  const viewportHeight = window.innerHeight / zoom;
  const width = flyout.offsetWidth;
  const height = flyout.offsetHeight;
  const centred = rect.left + rect.width / 2 - width / 2;
  const left = Math.min(Math.max(centred, VIEWPORT_MARGIN), Math.max(VIEWPORT_MARGIN, viewportWidth - VIEWPORT_MARGIN - width));

  const spaceBelow = viewportHeight - rect.bottom - FLYOUT_GAP - VIEWPORT_MARGIN;
  const spaceAbove = rect.top - FLYOUT_GAP - VIEWPORT_MARGIN;
  const above = height > spaceBelow && spaceAbove > spaceBelow;
  const top = above ? Math.max(VIEWPORT_MARGIN, rect.top - FLYOUT_GAP - height) : rect.bottom + FLYOUT_GAP;

  flyout.style.left = `${Math.round(left)}px`;
  flyout.style.top = `${Math.round(top)}px`;
  flyout.style.maxHeight = `${Math.max(120, Math.floor(above ? spaceAbove : spaceBelow))}px`;
  flyout.classList.toggle("opens-above", above);
}

function schedulePosition(): void {
  if (repositionFrame || !isFlyoutOpen()) {
    return;
  }
  repositionFrame = requestAnimationFrame(() => {
    repositionFrame = 0;
    positionFlyout();
  });
}

function focusOption(target: "selected" | "first" | "last" | 1 | -1): void {
  const flyout = flyoutEl();
  if (!flyout) {
    return;
  }

  const options = Array.from(flyout.querySelectorAll<HTMLButtonElement>(".input-mode-option:not(:disabled)"));
  if (options.length === 0) {
    return;
  }

  let next: HTMLButtonElement | undefined;
  if (target === "first") {
    next = options[0];
  } else if (target === "last") {
    next = options[options.length - 1];
  } else if (target === "selected") {
    next = options.find((option) => option.getAttribute("aria-checked") === "true") ?? options[0];
  } else {
    const current = options.indexOf(document.activeElement as HTMLButtonElement);
    next = options[(current + target + options.length) % options.length];
  }
  next?.focus();
}

function closeInputModeFlyout(returnFocus = false): void {
  const flyout = flyoutEl();
  if (flyout) {
    flyout.hidden = true;
  }

  const trigger = triggerEl();
  trigger?.setAttribute("aria-expanded", "false");
  if (returnFocus) {
    trigger?.focus();
  }
}

function openInputModeFlyout(focus: boolean): void {
  const flyout = flyoutEl();
  if (!flyout) {
    return;
  }

  renderFlyout();
  flyout.hidden = false;
  positionFlyout();

  triggerEl()?.setAttribute("aria-expanded", "true");

  if (focus) {
    focusOption("selected");
  }
}

// ── Choosing ─────────────────────────────────────────────────────────────────

function renderInputModeControl(): void {
  renderTrigger();
  if (isFlyoutOpen()) {
    renderFlyout();
    positionFlyout();
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

function chooseInputMode(mode: InputModeName): void {
  if (unavailableReason(mode) !== null) {
    return;
  }

  // In a DAW "stereo" is the track's own layout, which mono and stereo alike reach by not
  // asking for dual mono.
  currentInputMode = mode;

  // In a DAW the choice belongs to this instance and is saved with the project, not in app
  // settings shared by every instance.
  if (isStandaloneUi()) {
    persistInputMode(currentInputMode);
  }

  renderInputModeControl();
  sendInputModeToPlugin();
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

function bindFlyoutEvents(): void {
  const trigger = triggerEl();
  const flyout = flyoutEl();
  if (!trigger || !flyout) {
    return;
  }

  // Moved to the body, as the add-effect chooser is. Inside the bar, a theme giving the bar a
  // filter or a backdrop (docs/theme-system.md) would make it the box a fixed element is laid
  // out in, and clip it.
  document.body.append(flyout);

  // detail is 0 for a click made with the keyboard, which then puts focus in the list.
  trigger.addEventListener("click", (event) => {
    if (isFlyoutOpen()) {
      closeInputModeFlyout();
    } else {
      openInputModeFlyout(event.detail === 0);
    }
  });

  trigger.addEventListener("keydown", (event) => {
    if ((event.key === "ArrowDown" || event.key === "ArrowUp") && !isFlyoutOpen()) {
      event.preventDefault();
      openInputModeFlyout(true);
    }
  });

  flyout.addEventListener("click", (event) => {
    const option = (event.target as HTMLElement | null)?.closest<HTMLButtonElement>(".input-mode-option");
    if (!option || option.disabled || !isInputModeName(option.dataset.mode)) {
      return;
    }
    // Closed first, so choosing does not redraw the list and detach the row this click is still
    // bubbling from: the compact sheet's outside-click check walks up from it.
    closeInputModeFlyout(true);
    chooseInputMode(option.dataset.mode);
  });

  flyout.addEventListener("keydown", (event) => {
    switch (event.key) {
      case "ArrowDown": focusOption(1); break;
      case "ArrowUp": focusOption(-1); break;
      case "Home": focusOption("first"); break;
      case "End": focusOption("last"); break;
      case "Escape": closeInputModeFlyout(true); event.stopPropagation(); break;
      case "Tab": closeInputModeFlyout(); return;
      default: return;
    }
    event.preventDefault();
  });

  // Document-wide, so these look the elements up rather than holding the ones bound above.
  document.addEventListener("pointerdown", (event) => {
    if (!isFlyoutOpen() || !(event.target instanceof Node)) {
      return;
    }
    if (flyoutEl()?.contains(event.target) || triggerEl()?.contains(event.target)) {
      return;
    }
    closeInputModeFlyout();
  }, true);

  // The trigger can move under an open flyout: the window resizes, or the compact sheet scrolls.
  window.addEventListener("resize", schedulePosition);
  document.addEventListener("scroll", (event) => {
    if (!(event.target instanceof Node) || !flyoutEl()?.contains(event.target)) {
      schedulePosition();
    }
  }, true);
}

export function initializeInputModeControls(): void {
  if (!controlsInitialized) {
    controlsInitialized = true;
    bindFlyoutEvents();
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
