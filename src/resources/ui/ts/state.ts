import type { DemoSample, Preset, UiState } from "./types.js";

export const LOG_ENTRY_LIMIT = 200;

export const DEMO_AUDIO_SAMPLES: DemoSample[] = [
  {
    id: "guitar-riff-01",
    title: "Guitar Riff 01",
    path: "demo/guitar-riff-01.wav",
  },
  {
    id: "guitar-riff-02",
    title: "Guitar Riff 02",
    path: "demo/guitar-riff-02.wav",
  },
];

export const uiState: UiState = {
  presets: [],
  filteredPresets: [],
  activePresetId: null,
  presetCache: new Map<string, Preset>(),
  parameters: {
    values: [],
    gateEnabled: false,
    gateThreshold: null,
  },
  signalTest: null,
  demoAudioSelectedId: DEMO_AUDIO_SAMPLES.length ? DEMO_AUDIO_SAMPLES[0].id : null,
  demoAudioRepeat: false,
  logs: [],
  resourceLibrary: {},
  mixer: {
    activePresetIds: [],
    presets: {},
    masterGain: 1.0,
    limiterEnabled: false,
  },
};

export function clonePreset<T extends Preset | null>(preset: T): T {
  return preset ? (JSON.parse(JSON.stringify(preset)) as T) : preset;
}
