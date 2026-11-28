import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as AudioDeviceModule from "../ts/settings/audioDevice.js";
import type { AudioDeviceState } from "../ts/settings/audioDevice.js";

type AudioDevice = typeof AudioDeviceModule;
type Sent = { type: string; action?: string; [key: string]: unknown };

const AUDIO_MIDI_PANEL = readFileSync(join(__dirname, "..", "ui-components", "panels", "settings-audio-midi.html"), "utf8");

let audioDevice: AudioDevice;
let sent: Sent[];
let observed: Array<(entries: Array<{ isIntersecting: boolean }>) => void>;

function state(overrides: Partial<AudioDeviceState> = {}): AudioDeviceState {
  return {
    deviceTypes: ["Windows Audio", "ASIO"],
    deviceType: "Windows Audio",
    separateIO: true,
    inputDevices: ["Interface In", "Microphone (Realtek)"],
    outputDevices: ["Interface Out", "Speakers (Realtek)"],
    inputDevice: "Interface In",
    outputDevice: "Interface Out",
    defaultOutputDevice: "Speakers (Realtek)",
    outputFollowsDefault: false,
    deviceOpen: true,
    inputChannels: [{ name: "Input 1 + 2", active: false }, { name: "Input 3 + 4", active: true }],
    outputChannels: [{ name: "Output 1 + 2", active: true }],
    sampleRates: [44100, 48000],
    bufferSizes: [64, 128, 256],
    sampleRate: 48000,
    bufferSize: 128,
    inputLatency: 96,
    outputLatency: 192,
    hasControlPanel: false,
    canMuteInput: true,
    inputMuted: false,
    feedbackRisk: false,
    inputPermission: "granted",
    midiInputs: [{ identifier: "midi-1", name: "Pedalboard", enabled: true }],
    midiOutputs: [{ identifier: "midi-out-1", name: "Pedalboard", enabled: false }],
    midiOutput: "",
    showMidiOutput: true,
    ...overrides,
  };
}

function requests(): Sent[] {
  return sent.filter((message) => message.type === "audioDevice");
}

function select(id: string): HTMLSelectElement {
  return document.getElementById(id) as HTMLSelectElement;
}

function optionLabels(id: string): string[] {
  return Array.from(select(id).options).map((option) => option.textContent ?? "");
}

function change(id: string, value: string): void {
  const element = select(id);
  element.value = value;
  element.dispatchEvent(new Event("change", { bubbles: true }));
}

beforeEach(async () => {
  vi.useFakeTimers();
  document.body.innerHTML = AUDIO_MIDI_PANEL;
  sent = [];
  observed = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as Sent);
  };
  // jsdom has no IntersectionObserver; hand the test the callback instead.
  vi.stubGlobal("IntersectionObserver", class {
    constructor(callback: (entries: Array<{ isIntersecting: boolean }>) => void) {
      observed.push(callback);
    }
    observe(): void {}
    disconnect(): void {}
  });
  // CSS.escape is missing from jsdom too.
  vi.stubGlobal("CSS", { escape: (value: string) => value.replace(/["\\]/g, "\\$&") });
  vi.resetModules();
  audioDevice = await import("../ts/settings/audioDevice.js");
  audioDevice.initAudioDeviceSettings();
});

afterEach(() => {
  delete window.IPlugSendMsg;
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe("narrowing a snapshot", () => {
  it("drops what is not the right type", () => {
    const narrowed = audioDevice.normalizeAudioDeviceState({
      deviceTypes: ["ASIO", 3],
      sampleRates: [48000, "96000", null],
      inputChannels: [{ name: "In 1", active: true }, "junk"],
      midiInputs: [{ identifier: "a", name: 5 }, { name: "no id" }],
      inputPermission: "maybe",
    });
    expect(narrowed?.deviceTypes).toEqual(["ASIO"]);
    expect(narrowed?.sampleRates).toEqual([48000]);
    expect(narrowed?.inputChannels).toEqual([{ name: "In 1", active: true }]);
    expect(narrowed?.midiInputs).toEqual([{ identifier: "a", name: "a", enabled: false }]);
    expect(narrowed?.inputPermission).toBe("granted");
    expect(narrowed?.separateIO).toBe(true);
    expect(audioDevice.normalizeAudioDeviceState("nope")).toBeNull();
  });
});

describe("device options", () => {
  it("keeps a chosen device that has gone, and ends with none", () => {
    expect(audioDevice.deviceOptions(["A", "B"], "Gone", "No input")).toEqual([
      { value: "Gone", label: "Gone (disconnected)" },
      { value: "A", label: "A" },
      { value: "B", label: "B" },
      { value: "", label: "No input" },
    ]);
  });

  it("lists a running buffer size the driver did not offer", () => {
    const labels = audioDevice.bufferSizeOptions(state({ bufferSizes: [64, 256], bufferSize: 192 })).map((option) => option.label);
    expect(labels).toEqual(["64 samples (1.3 ms)", "192 samples (4.0 ms)", "256 samples (5.3 ms)"]);
  });
});

describe("the alert", () => {
  it("puts a refused microphone first", () => {
    const alert = audioDevice.audioDeviceAlert(state({ inputPermission: "denied", deviceOpen: false }), "boom");
    expect(alert?.action?.request).toBe("requestInputPermission");
  });

  it("then a failed request, then a device that would not open", () => {
    expect(audioDevice.audioDeviceAlert(state({ deviceOpen: false }), "Device in use")?.text).toBe("Device in use");
    expect(audioDevice.audioDeviceAlert(state({ deviceOpen: false }), "")?.action?.request).toBe("resetDevice");
  });

  it("says when nothing can be heard", () => {
    expect(audioDevice.audioDeviceAlert(state({ outputDevice: "" }), "")?.text).toMatch(/No output/);
    expect(audioDevice.audioDeviceAlert(state({ inputChannels: [{ name: "In", active: false }] }), "")?.text).toMatch(/No input/);
    expect(audioDevice.audioDeviceAlert(state(), "")).toBeNull();
  });
});

describe("the controls", () => {
  it("stay hidden until the engine says it drives the devices, then ask for the state", () => {
    const container = document.getElementById("audio-device-settings")!;
    expect(container.hidden).toBe(true);
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    expect(container.hidden).toBe(false);
    expect(container.dataset.state).toBe("loading");
    expect(requests()).toEqual([{ type: "audioDevice", action: "getState" }]);
  });

  it("draw a snapshot", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state());

    expect(document.getElementById("audio-device-settings")!.dataset.state).toBe("ready");
    expect(select("audio-device-type").value).toBe("Windows Audio");
    expect(select("audio-device-input").value).toBe("Interface In");
    expect(optionLabels("audio-device-input-channels")).toEqual(["None", "Input 1 + 2", "Input 3 + 4"]);
    expect(select("audio-device-input-channels").value).toBe("1");
    // A single output pair has nothing to choose.
    expect(document.getElementById("audio-device-output-channels-row")!.hidden).toBe(true);
    expect(select("audio-device-buffer-size").value).toBe("128");
    expect(document.getElementById("audio-device-latency")!.textContent).toMatch(/6\.0 ms/);
    expect(document.getElementById("audio-device-control-panel")!.hidden).toBe(true);
    expect((document.getElementById("audio-device-midi-input-0") as HTMLInputElement).checked).toBe(true);
    expect(document.getElementById("midi-device-settings")!.hidden).toBe(false);
  });

  it("answers None under the MIDI Inputs heading when there is no input port", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state({ midiInputs: [] }));

    // The heading is markup, not rendered, so it stays whatever the engine reports.
    expect(document.querySelector(".audio-device-midi-label")!.textContent).toBe("MIDI Inputs");
    expect(document.getElementById("audio-device-midi-inputs")!.textContent).toBe("None");
    expect(document.getElementById("audio-device-midi-input-0")).toBeNull();
    expect(document.getElementById("midi-device-settings")!.hidden).toBe(false);

    // And a port arriving later replaces it.
    audioDevice.showAudioDeviceState(state());
    expect(document.querySelector(".audio-device-midi-empty")).toBeNull();
    expect((document.getElementById("audio-device-midi-input-0") as HTMLInputElement).checked).toBe(true);
  });

  it("show one device picker for a driver that opens both sides together", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state({ deviceTypes: ["ASIO"], deviceType: "ASIO", separateIO: false, hasControlPanel: true }));
    expect(document.getElementById("audio-device-type-row")!.hidden).toBe(true);
    expect(document.getElementById("audio-device-input-row")!.hidden).toBe(true);
    expect(document.getElementById("audio-device-output-label")!.textContent).toBe("Device");
    expect(document.getElementById("audio-device-control-panel")!.hidden).toBe(false);

    change("audio-device-output", "Interface Out");
    expect(requests().at(-1)).toEqual({ type: "audioDevice", action: "setDevice", kind: "linked", name: "Interface Out" });
  });

  it("offer the system default output, and follow it when chosen", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state());
    expect(optionLabels("audio-device-output")).toEqual(["System default (Speakers (Realtek))", "Interface Out", "Speakers (Realtek)", "No output"]);
    expect(select("audio-device-output").value).toBe("Interface Out");

    change("audio-device-output", audioDevice.SYSTEM_DEFAULT_OUTPUT);
    expect(requests().at(-1)).toEqual({ type: "audioDevice", action: "setDevice", kind: "output", systemDefault: true });

    // Following, the open device is the default, and is not shown as chosen by name.
    audioDevice.showAudioDeviceState(state({ outputDevice: "Speakers (Realtek)", outputFollowsDefault: true }));
    expect(select("audio-device-output").value).toBe(audioDevice.SYSTEM_DEFAULT_OUTPUT);

    change("audio-device-output", "Speakers (Realtek)");
    expect(requests().at(-1)).toEqual({ type: "audioDevice", action: "setDevice", kind: "output", name: "Speakers (Realtek)" });
  });

  it("offer no system default output when the driver has none", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state({ separateIO: false, defaultOutputDevice: "" }));
    expect(optionLabels("audio-device-output")).not.toContain(expect.stringMatching(/System default/));
  });

  it("send what each control changes", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state());
    sent = [];

    change("audio-device-input", "Microphone (Realtek)");
    change("audio-device-input-channels", "0");
    change("audio-device-sample-rate", "44100");
    change("audio-device-buffer-size", "64");
    change("audio-device-midi-output", "midi-out-1");
    const mute = document.getElementById("audio-device-mute-input") as HTMLInputElement;
    mute.checked = true;
    mute.dispatchEvent(new Event("change", { bubbles: true }));
    const midiInput = document.getElementById("audio-device-midi-input-0") as HTMLInputElement;
    midiInput.checked = false;
    midiInput.dispatchEvent(new Event("change", { bubbles: true }));

    expect(requests()).toEqual([
      { type: "audioDevice", action: "setDevice", kind: "input", name: "Microphone (Realtek)" },
      { type: "audioDevice", action: "setInputChannels", group: 0 },
      { type: "audioDevice", action: "setSampleRate", sampleRate: 44100 },
      { type: "audioDevice", action: "setBufferSize", bufferSize: 64 },
      { type: "audioDevice", action: "setMidiOutput", identifier: "midi-out-1" },
      { type: "audioDevice", action: "setInputMuted", muted: true },
      { type: "audioDevice", action: "setMidiInputEnabled", identifier: "midi-1", enabled: false },
    ]);
  });

  it("keep a request's error until the next request", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state(), "Device in use");
    // The device manager's own change message follows, without the error.
    audioDevice.showAudioDeviceState(state());
    expect(document.getElementById("audio-device-alert-text")!.textContent).toBe("Device in use");

    document.getElementById("audio-device-alert-action")!.click();
    expect(requests().at(-1)).toEqual({ type: "audioDevice", action: "resetDevice" });
    audioDevice.showAudioDeviceState(state());
    expect(document.getElementById("audio-device-alert")!.hidden).toBe(true);
  });

  it("lock a list of one and say who sets it", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state({ deviceType: "ASIO", bufferSizes: [64], bufferSize: 64, hasControlPanel: true }));
    expect(select("audio-device-buffer-size").disabled).toBe(true);
    expect(document.getElementById("audio-device-buffer-size-hint")!.textContent).toMatch(/Control Panel/);
    expect(select("audio-device-sample-rate").disabled).toBe(false);
    expect(document.getElementById("audio-device-sample-rate-hint")!.hidden).toBe(true);

    audioDevice.showAudioDeviceState(state({ sampleRates: [48000] }));
    expect(select("audio-device-sample-rate").disabled).toBe(true);
    expect(document.getElementById("audio-device-sample-rate-hint")!.textContent).toMatch(/Exclusive Mode/);
    expect(select("audio-device-buffer-size").disabled).toBe(false);
  });

  it("explain a mute the feedback check chose", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state({ inputMuted: true, feedbackRisk: true }));
    const hint = document.getElementById("audio-device-mute-hint")!;
    expect(hint.textContent).toMatch(/built-in microphone can hear the speakers/);
    expect(hint.classList.contains("is-warning")).toBe(true);
    expect((document.getElementById("audio-device-mute-input") as HTMLInputElement).checked).toBe(true);
  });
});

describe("the meter lease", () => {
  it("rescans and meters while on screen, and lets go when not", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    sent = [];

    observed[0]([{ isIntersecting: true }]);
    expect(requests()).toEqual([
      { type: "audioDevice", action: "getState", rescan: true },
      { type: "audioDevice", action: "watchLevels", enabled: true },
    ]);

    vi.advanceTimersByTime(audioDevice.AUDIO_DEVICE_LEVEL_RENEW_MS);
    expect(requests().at(-1)).toEqual({ type: "audioDevice", action: "watchLevels", enabled: true });

    observed[0]([{ isIntersecting: false }]);
    expect(requests().at(-1)).toEqual({ type: "audioDevice", action: "watchLevels", enabled: false });
    const count = requests().length;
    vi.advanceTimersByTime(audioDevice.AUDIO_DEVICE_LEVEL_RENEW_MS * 3);
    expect(requests()).toHaveLength(count);
  });

  it("draws the level and the dropouts", () => {
    audioDevice.syncAudioDeviceSettingsAvailability(true);
    audioDevice.showAudioDeviceState(state());
    audioDevice.showAudioDeviceLevels(-30, 4);
    expect(document.getElementById("audio-device-input-meter-fill")!.style.width).toBe("50%");
    expect(document.getElementById("audio-device-latency")!.textContent).toMatch(/Dropouts since the device opened: 4\./);
  });
});
