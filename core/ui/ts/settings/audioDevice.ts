/**
 * Settings > Audio Device and MIDI Devices: the standalone app's own device controls.
 *
 * They replace JUCE's Audio/MIDI Settings dialog, a native window that on Android
 * cannot sit over the WebView and elsewhere looks nothing like the app. The engine
 * side is juce/source/StandaloneAudioSettings. This module keeps no device state of
 * its own: every request is answered with a whole `audioDeviceState` snapshot, and
 * the controls are redrawn from it.
 *
 * While the controls are on screen the input meter runs on a lease the module
 * renews every two seconds; the engine lets it lapse five seconds after the last
 * renewal, so a page that went away without saying so costs nothing.
 */

import { sendAudioDeviceRequest } from "../bridge.js";

export interface AudioDeviceChannelGroup {
  name: string;
  active: boolean;
}

export interface AudioDeviceMidiPort {
  identifier: string;
  name: string;
  enabled: boolean;
}

export interface AudioDeviceState {
  deviceTypes: string[];
  deviceType: string;
  /** False for drivers such as ASIO that open input and output as one device. */
  separateIO: boolean;
  inputDevices: string[];
  outputDevices: string[];
  /** Empty is "no device". */
  inputDevice: string;
  outputDevice: string;
  /** The output the operating system has chosen; empty when the driver has none to follow (ASIO, mobile). */
  defaultOutputDevice: string;
  /** "System default" is the output: it follows the operating system's choice. */
  outputFollowsDefault: boolean;
  deviceOpen: boolean;
  /** Channels in groups as wide as the plugin's bus; at most one group is on. */
  inputChannels: AudioDeviceChannelGroup[];
  outputChannels: AudioDeviceChannelGroup[];
  sampleRates: number[];
  bufferSizes: number[];
  /** What the device runs at, which is not always what was asked for. */
  sampleRate: number;
  bufferSize: number;
  inputLatency: number;
  outputLatency: number;
  hasControlPanel: boolean;
  canMuteInput: boolean;
  inputMuted: boolean;
  /** The input and output look like a built-in mic and speakers. */
  feedbackRisk: boolean;
  inputPermission: "granted" | "denied";
  midiInputs: AudioDeviceMidiPort[];
  midiOutputs: AudioDeviceMidiPort[];
  midiOutput: string;
  showMidiOutput: boolean;
}

export interface SelectOption {
  value: string;
  label: string;
}

/** How often the meter lease is renewed; the engine's lease lasts 5 s. */
export const AUDIO_DEVICE_LEVEL_RENEW_MS = 2000;

/** The bottom of the input meter. */
export const AUDIO_DEVICE_METER_FLOOR_DB = -60;

/** How long the Test button shows that the tone is playing. */
const TEST_TONE_FEEDBACK_MS = 1200;

let available = false;
let initialized = false;
let current: AudioDeviceState | null = null;
let lastError = "";
let latencyText = "";
let dropoutText = "";
let watching = false;
let renewTimer: ReturnType<typeof setInterval> | null = null;
let testToneTimer: ReturnType<typeof setTimeout> | null = null;
let observer: IntersectionObserver | null = null;

function el<T extends HTMLElement = HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

// ── Narrowing ────────────────────────────────────────────────────────────────

function strings(value: unknown): string[] {
  return Array.isArray(value) ? value.filter((item): item is string => typeof item === "string") : [];
}

function numbers(value: unknown): number[] {
  return Array.isArray(value) ? value.filter((item): item is number => typeof item === "number" && Number.isFinite(item)) : [];
}

function numberOr(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function channelGroups(value: unknown): AudioDeviceChannelGroup[] {
  if (!Array.isArray(value)) {
    return [];
  }
  return value
    .filter((item): item is Record<string, unknown> => typeof item === "object" && item !== null)
    .map((item) => ({ name: typeof item.name === "string" ? item.name : "", active: item.active === true }));
}

function midiPorts(value: unknown): AudioDeviceMidiPort[] {
  if (!Array.isArray(value)) {
    return [];
  }
  return value
    .filter((item): item is Record<string, unknown> & { identifier: string } => typeof item === "object" && item !== null && typeof item.identifier === "string")
    .map((item) => ({ identifier: item.identifier, name: typeof item.name === "string" ? item.name : item.identifier, enabled: item.enabled === true }));
}

/** The `state` of an `audioDeviceState` message, or null if it is not one. */
export function normalizeAudioDeviceState(raw: unknown): AudioDeviceState | null {
  if (typeof raw !== "object" || raw === null) {
    return null;
  }
  const s = raw as Record<string, unknown>;
  return {
    deviceTypes: strings(s.deviceTypes),
    deviceType: typeof s.deviceType === "string" ? s.deviceType : "",
    separateIO: s.separateIO !== false,
    inputDevices: strings(s.inputDevices),
    outputDevices: strings(s.outputDevices),
    inputDevice: typeof s.inputDevice === "string" ? s.inputDevice : "",
    outputDevice: typeof s.outputDevice === "string" ? s.outputDevice : "",
    defaultOutputDevice: typeof s.defaultOutputDevice === "string" ? s.defaultOutputDevice : "",
    outputFollowsDefault: s.outputFollowsDefault === true,
    deviceOpen: s.deviceOpen === true,
    inputChannels: channelGroups(s.inputChannels),
    outputChannels: channelGroups(s.outputChannels),
    sampleRates: numbers(s.sampleRates),
    bufferSizes: numbers(s.bufferSizes),
    sampleRate: numberOr(s.sampleRate, 0),
    bufferSize: numberOr(s.bufferSize, 0),
    inputLatency: numberOr(s.inputLatency, 0),
    outputLatency: numberOr(s.outputLatency, 0),
    hasControlPanel: s.hasControlPanel === true,
    canMuteInput: s.canMuteInput === true,
    inputMuted: s.inputMuted === true,
    feedbackRisk: s.feedbackRisk === true,
    inputPermission: s.inputPermission === "denied" ? "denied" : "granted",
    midiInputs: midiPorts(s.midiInputs),
    midiOutputs: midiPorts(s.midiOutputs),
    midiOutput: typeof s.midiOutput === "string" ? s.midiOutput : "",
    showMidiOutput: s.showMidiOutput === true,
  };
}

// ── Option lists ─────────────────────────────────────────────────────────────

/** Devices, then "none". A chosen device that has gone stays listed, so the picker shows what broke. */
export function deviceOptions(devices: string[], selected: string, noneLabel: string): SelectOption[] {
  const options = devices.map((name) => ({ value: name, label: name }));
  if (selected !== "" && !devices.includes(selected)) {
    options.unshift({ value: selected, label: `${selected} (disconnected)` });
  }
  options.push({ value: "", label: noneLabel });
  return options;
}

/** The output picker's value for "System default"; no device can be named this. */
export const SYSTEM_DEFAULT_OUTPUT = "\u0000system-default";

/** The output picker: "System default" first when the driver has one, then the devices. */
export function outputDeviceOptions(state: AudioDeviceState): SelectOption[] {
  const options = deviceOptions(state.outputDevices, state.outputFollowsDefault ? "" : state.outputDevice, state.separateIO ? "No output" : "No device");
  if (state.defaultOutputDevice !== "") {
    options.unshift({ value: SYSTEM_DEFAULT_OUTPUT, label: `System default (${state.defaultOutputDevice})` });
  }
  return options;
}

export function selectedOutputOption(state: AudioDeviceState): string {
  return state.outputFollowsDefault ? SYSTEM_DEFAULT_OUTPUT : state.outputDevice;
}

/** "None", then each channel group by index. */
export function channelOptions(groups: AudioDeviceChannelGroup[]): SelectOption[] {
  return [{ value: "-1", label: "None" }, ...groups.map((group, index) => ({ value: String(index), label: group.name }))];
}

export function activeChannelGroup(groups: AudioDeviceChannelGroup[]): number {
  return groups.findIndex((group) => group.active);
}

function withCurrent(values: number[], currentValue: number): number[] {
  return currentValue > 0 && !values.includes(currentValue) ? [...values, currentValue].sort((a, b) => a - b) : values;
}

export function sampleRateOptions(state: AudioDeviceState): SelectOption[] {
  return withCurrent(state.sampleRates, state.sampleRate).map((rate) => ({ value: String(rate), label: `${rate} Hz` }));
}

export function bufferSizeOptions(state: AudioDeviceState): SelectOption[] {
  return withCurrent(state.bufferSizes, state.bufferSize).map((size) => ({
    value: String(size),
    label: state.sampleRate > 0 ? `${size} samples (${formatMs(size, state.sampleRate)} ms)` : `${size} samples`,
  }));
}

function formatMs(samples: number, sampleRate: number): string {
  return ((samples * 1000) / sampleRate).toFixed(1);
}

// ── Messages about the setup ─────────────────────────────────────────────────

export interface AudioDeviceAlert {
  text: string;
  /** A button that fixes it: the label, and the request it sends. */
  action?: { label: string; request: string };
}

/** The one thing most worth saying about the setup, if anything. */
export function audioDeviceAlert(state: AudioDeviceState, error: string): AudioDeviceAlert | null {
  if (state.inputPermission === "denied") {
    return {
      text: "Microphone access is off, so the amp can't hear your instrument.",
      action: { label: "Allow Access", request: "requestInputPermission" },
    };
  }
  if (error) {
    return { text: error, action: { label: "Reset Device", request: "resetDevice" } };
  }
  if (state.inputDevice === "" && state.outputDevice === "") {
    return { text: "No audio device is selected." };
  }
  if (!state.deviceOpen) {
    return {
      text: "The audio device couldn't be opened. Plug it back in and retry, or choose another device.",
      action: { label: "Retry", request: "resetDevice" },
    };
  }
  if (state.outputDevice === "" || !state.outputChannels.some((group) => group.active)) {
    return { text: "No output is selected, so nothing will be heard." };
  }
  if (state.inputDevice === "" || !state.inputChannels.some((group) => group.active)) {
    return { text: "No input is selected, so the amp hears nothing." };
  }
  return null;
}

export function muteHint(state: AudioDeviceState): string {
  if (state.inputMuted && state.feedbackRisk) {
    return "Muted because the built-in microphone can hear the speakers. Plug in headphones or an interface, then turn this off.";
  }
  if (state.inputMuted) {
    return "The amp hears nothing while the input is muted.";
  }
  if (state.feedbackRisk) {
    return "The built-in microphone can hear the speakers, which can howl. Use headphones, or mute the input.";
  }
  return "Silences the input, to stop feedback when a microphone can hear the speakers.";
}

/** Why the sample rate cannot be changed, when the device offers only one. */
export function sampleRateHint(state: AudioDeviceState): string {
  if (state.sampleRates.length > 1) {
    return "";
  }
  if (state.deviceType === "Windows Audio") {
    return "Windows fixes the rate in shared mode. Choose Windows Audio (Exclusive Mode) as the driver for other rates.";
  }
  return "The audio driver sets the sample rate.";
}

/** Why the buffer size cannot be changed, when the device offers only one. */
export function bufferSizeHint(state: AudioDeviceState): string {
  if (state.bufferSizes.length > 1) {
    return "";
  }
  return state.hasControlPanel
    ? "This driver sets its own buffer size. Change it in the driver's Control Panel below."
    : "The audio driver sets the buffer size.";
}

export function latencySummary(state: AudioDeviceState): string {
  if (!state.deviceOpen || state.sampleRate <= 0 || (state.inputLatency <= 0 && state.outputLatency <= 0)) {
    return "";
  }
  const total = formatMs(state.inputLatency + state.outputLatency, state.sampleRate);
  const input = formatMs(state.inputLatency, state.sampleRate);
  const output = formatMs(state.outputLatency, state.sampleRate);
  return `Latency reported by the driver: ${total} ms (${input} ms in, ${output} ms out).`;
}

// ── Rendering ────────────────────────────────────────────────────────────────

/** Rebuilds the options only when they changed, so a redraw does not close an open picker. */
function fillSelect(select: HTMLSelectElement | null, options: SelectOption[], selected: string): void {
  if (!select) {
    return;
  }
  const signature = JSON.stringify(options);
  if (select.dataset.options !== signature) {
    select.replaceChildren(...options.map((option) => {
      const element = document.createElement("option");
      element.value = option.value;
      element.textContent = option.label;
      return element;
    }));
    select.dataset.options = signature;
  }
  select.value = selected;
}

function showRow(id: string, show: boolean): void {
  el(id)?.toggleAttribute("hidden", !show);
}

function showHint(id: string, text: string): void {
  const hint = el(id);
  if (hint) {
    hint.textContent = text;
    hint.hidden = text === "";
  }
}

function renderAlert(state: AudioDeviceState | null): void {
  const alert = state ? audioDeviceAlert(state, lastError) : { text: "Looking for audio devices…" };
  const container = el("audio-device-alert");
  const text = el("audio-device-alert-text");
  const button = el<HTMLButtonElement>("audio-device-alert-action");
  container?.toggleAttribute("hidden", !alert);
  if (text) {
    text.textContent = alert?.text ?? "";
  }
  if (button) {
    button.hidden = !alert?.action;
    button.textContent = alert?.action?.label ?? "";
    button.dataset.request = alert?.action?.request ?? "";
  }
}

function renderMidi(state: AudioDeviceState): void {
  const list = el("audio-device-midi-inputs");
  if (list) {
    const signature = JSON.stringify(state.midiInputs.map((port) => [port.identifier, port.name]));
    if (list.dataset.ports !== signature) {
      list.replaceChildren(...state.midiInputs.map((port, index) => {
        const row = document.createElement("div");
        row.className = "audio-device-midi-input";
        const id = `audio-device-midi-input-${index}`;
        const label = document.createElement("label");
        label.htmlFor = id;
        label.textContent = port.name;
        const toggle = document.createElement("label");
        toggle.className = "toggle-switch";
        const input = document.createElement("input");
        input.type = "checkbox";
        input.id = id;
        input.dataset.identifier = port.identifier;
        const slider = document.createElement("span");
        slider.className = "toggle-slider";
        toggle.append(input, slider);
        row.append(label, toggle);
        return row;
      }));
      if (state.midiInputs.length === 0) {
        // Sits where a port would, under the heading the markup keeps: the answer to
        // "which MIDI inputs?" is None, rather than the section looking unfinished.
        const empty = document.createElement("div");
        empty.className = "audio-device-midi-empty";
        empty.textContent = "None";
        list.append(empty);
      }
      list.dataset.ports = signature;
    }
    for (const port of state.midiInputs) {
      const input = list.querySelector<HTMLInputElement>(`input[data-identifier="${CSS.escape(port.identifier)}"]`);
      if (input) {
        input.checked = port.enabled;
      }
    }
  }

  showRow("audio-device-midi-output-row", state.showMidiOutput);
  showRow("audio-device-midi-output-hint", state.showMidiOutput);
  const outputs = [...state.midiOutputs.map((port) => ({ value: port.identifier, label: port.name })), { value: "", label: "None" }];
  if (state.midiOutput !== "" && !state.midiOutputs.some((port) => port.identifier === state.midiOutput)) {
    outputs.unshift({ value: state.midiOutput, label: "(disconnected)" });
  }
  fillSelect(el<HTMLSelectElement>("audio-device-midi-output"), outputs, state.midiOutput);
}

function renderLatency(): void {
  const hint = el("audio-device-latency");
  if (hint) {
    hint.textContent = [latencyText, dropoutText].filter(Boolean).join(" ");
    hint.hidden = hint.textContent === "";
  }
}

function render(): void {
  const container = el("audio-device-settings");
  if (!container) {
    return;
  }
  container.hidden = !available;
  el("midi-device-settings")?.toggleAttribute("hidden", !available || !current);
  container.dataset.state = current ? "ready" : "loading";
  renderAlert(current);

  const state = current;
  if (!state) {
    return;
  }

  showRow("audio-device-type-row", state.deviceTypes.length > 1);
  fillSelect(el<HTMLSelectElement>("audio-device-type"), state.deviceTypes.map((type) => ({ value: type, label: type })), state.deviceType);

  // One picker for drivers that open both sides as one device, named for that.
  showRow("audio-device-input-row", state.separateIO);
  fillSelect(el<HTMLSelectElement>("audio-device-input"), deviceOptions(state.inputDevices, state.inputDevice, "No input"), state.inputDevice);
  const outputLabel = el("audio-device-output-label");
  if (outputLabel) {
    outputLabel.textContent = state.separateIO ? "Output Device" : "Device";
  }
  fillSelect(el<HTMLSelectElement>("audio-device-output"), outputDeviceOptions(state), selectedOutputOption(state));
  const testButton = el<HTMLButtonElement>("audio-device-test");
  if (testButton) {
    testButton.disabled = !state.deviceOpen || state.outputDevice === "";
  }

  showRow("audio-device-input-channels-row", state.inputChannels.length > 0);
  fillSelect(el<HTMLSelectElement>("audio-device-input-channels"), channelOptions(state.inputChannels), String(activeChannelGroup(state.inputChannels)));

  // A two-channel output has nothing to choose, unless it has somehow been switched off.
  const outputGroup = activeChannelGroup(state.outputChannels);
  showRow("audio-device-output-channels-row", state.outputChannels.length > 1 || (state.outputChannels.length > 0 && outputGroup < 0));
  fillSelect(el<HTMLSelectElement>("audio-device-output-channels"), channelOptions(state.outputChannels), String(outputGroup));

  // A list of one is shown for what it reads, but cannot be opened, and says who sets it.
  const sampleRate = el<HTMLSelectElement>("audio-device-sample-rate");
  showRow("audio-device-sample-rate-row", state.deviceOpen);
  fillSelect(sampleRate, sampleRateOptions(state), String(state.sampleRate));
  if (sampleRate) {
    sampleRate.disabled = state.sampleRates.length <= 1;
  }
  showHint("audio-device-sample-rate-hint", state.deviceOpen ? sampleRateHint(state) : "");
  const bufferSize = el<HTMLSelectElement>("audio-device-buffer-size");
  showRow("audio-device-buffer-size-row", state.deviceOpen);
  fillSelect(bufferSize, bufferSizeOptions(state), String(state.bufferSize));
  if (bufferSize) {
    bufferSize.disabled = state.bufferSizes.length <= 1;
  }
  showHint("audio-device-buffer-size-hint", state.deviceOpen ? bufferSizeHint(state) : "");
  latencyText = latencySummary(state);
  renderLatency();

  showRow("audio-device-mute-row", state.canMuteInput);
  showRow("audio-device-mute-hint", state.canMuteInput);
  const mute = el<HTMLInputElement>("audio-device-mute-input");
  if (mute) {
    mute.checked = state.inputMuted;
  }
  const hint = el("audio-device-mute-hint");
  if (hint) {
    hint.textContent = muteHint(state);
    hint.classList.toggle("is-warning", state.inputMuted || state.feedbackRisk);
  }

  showRow("audio-device-control-panel", state.hasControlPanel);

  renderMidi(state);
}

/** The input meter and dropout count, from an `audioDeviceLevels` message. */
export function showAudioDeviceLevels(inputDb: number, xruns: number): void {
  const db = Number.isFinite(inputDb) ? inputDb : AUDIO_DEVICE_METER_FLOOR_DB;
  const clamped = Math.min(0, Math.max(AUDIO_DEVICE_METER_FLOOR_DB, db));
  const fraction = (clamped - AUDIO_DEVICE_METER_FLOOR_DB) / -AUDIO_DEVICE_METER_FLOOR_DB;
  const fill = el("audio-device-input-meter-fill");
  if (fill) {
    fill.style.width = `${(fraction * 100).toFixed(1)}%`;
    fill.classList.toggle("is-hot", db > -6);
    fill.classList.toggle("is-clipping", db > -0.5);
  }
  el("audio-device-input-meter")?.setAttribute("aria-valuenow", clamped.toFixed(0));

  const nextDropoutText = Number.isFinite(xruns) && xruns > 0 ? `Dropouts since the device opened: ${xruns}.` : "";
  if (nextDropoutText !== dropoutText) {
    dropoutText = nextDropoutText;
    renderLatency();
  }
}

/** A snapshot from the engine. An error belongs to the request that caused it and stays until the next one. */
export function showAudioDeviceState(state: AudioDeviceState, error = ""): void {
  current = state;
  if (error) {
    lastError = error;
  }
  render();
}

/** The engine says this build has no device settings of its own (a plugin format). */
export function markAudioDeviceSettingsUnavailable(): void {
  syncAudioDeviceSettingsAvailability(false);
}

// ── Requests ─────────────────────────────────────────────────────────────────

function request(action: string, args: Record<string, unknown> = {}): void {
  lastError = "";
  sendAudioDeviceRequest(action, args);
}

function renewLevels(): void {
  sendAudioDeviceRequest("watchLevels", { enabled: true });
}

/** Rescans the devices and runs the meter while the controls are on screen, and stops it when they are not. */
function setWatching(next: boolean): void {
  if (next === watching) {
    return;
  }
  watching = next;
  if (next) {
    sendAudioDeviceRequest("getState", { rescan: true });
    renewLevels();
    renewTimer = setInterval(renewLevels, AUDIO_DEVICE_LEVEL_RENEW_MS);
    return;
  }
  if (renewTimer !== null) {
    clearInterval(renewTimer);
    renewTimer = null;
  }
  sendAudioDeviceRequest("watchLevels", { enabled: false });
  showAudioDeviceLevels(AUDIO_DEVICE_METER_FLOOR_DB, 0);
}

/** Shows the controls where the engine can drive the devices, and fetches their state the first time. */
export function syncAudioDeviceSettingsAvailability(isAvailable: boolean): void {
  const becameAvailable = isAvailable && !available;
  available = isAvailable;
  if (!available) {
    setWatching(false);
  }
  render();
  if (becameAvailable && !current) {
    sendAudioDeviceRequest("getState");
  }
}

function onSelectChange(id: string, handler: (value: string) => void): void {
  el<HTMLSelectElement>(id)?.addEventListener("change", (event) => handler((event.target as HTMLSelectElement).value));
}

export function initAudioDeviceSettings(): void {
  if (initialized) {
    return;
  }
  initialized = true;

  onSelectChange("audio-device-type", (deviceType) => request("setDeviceType", { deviceType }));
  onSelectChange("audio-device-input", (name) => request("setDevice", { kind: "input", name }));
  onSelectChange("audio-device-output", (name) =>
    name === SYSTEM_DEFAULT_OUTPUT
      ? request("setDevice", { kind: "output", systemDefault: true })
      : request("setDevice", { kind: current?.separateIO === false ? "linked" : "output", name }));
  onSelectChange("audio-device-input-channels", (group) => request("setInputChannels", { group: Number(group) }));
  onSelectChange("audio-device-output-channels", (group) => request("setOutputChannels", { group: Number(group) }));
  onSelectChange("audio-device-sample-rate", (rate) => request("setSampleRate", { sampleRate: Number(rate) }));
  onSelectChange("audio-device-buffer-size", (size) => request("setBufferSize", { bufferSize: Number(size) }));
  onSelectChange("audio-device-midi-output", (identifier) => request("setMidiOutput", { identifier }));

  el<HTMLInputElement>("audio-device-mute-input")?.addEventListener("change", (event) => {
    request("setInputMuted", { muted: (event.target as HTMLInputElement).checked });
  });
  el("audio-device-midi-inputs")?.addEventListener("change", (event) => {
    const input = event.target as HTMLInputElement;
    if (input.dataset.identifier) {
      request("setMidiInputEnabled", { identifier: input.dataset.identifier, enabled: input.checked });
    }
  });

  el("audio-device-alert-action")?.addEventListener("click", (event) => {
    const action = (event.currentTarget as HTMLButtonElement).dataset.request;
    if (action) {
      request(action);
    }
  });
  el("audio-device-control-panel")?.addEventListener("click", () => request("showControlPanel"));
  el("audio-device-reset")?.addEventListener("click", () => request("resetDevice"));

  const testButton = el<HTMLButtonElement>("audio-device-test");
  testButton?.addEventListener("click", () => {
    request("playTestTone");
    testButton.classList.add("is-playing");
    if (testToneTimer !== null) {
      clearTimeout(testToneTimer);
    }
    testToneTimer = setTimeout(() => {
      testToneTimer = null;
      testButton.classList.remove("is-playing");
    }, TEST_TONE_FEEDBACK_MS);
  });

  const container = el("audio-device-settings");
  if (container && typeof IntersectionObserver !== "undefined") {
    observer = new IntersectionObserver((entries) => {
      const entry = entries[entries.length - 1];
      setWatching(Boolean(entry?.isIntersecting) && available);
    });
    observer.observe(container);
  }
}
