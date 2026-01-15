import type { DemoSample, GlobalSignalChainConfig, Preset, UiState } from "./types.js";

export const LOG_ENTRY_LIMIT = 200;

export const DEMO_AUDIO_SAMPLES: DemoSample[] = [
  {
    id: "di-riff-01",
    title: "Guitar Riff 01",
    path: "demo/guitar-riff-01.wav",
  },
  {
    id: "di-riff-02",
    title: "Guitar Riff 02",
    path: "demo/guitar-riff-02.wav",
  },
   {
    id: "di-riff-03",
    title: "Guitar Riff 03",
    path: "demo/DI_Guitar_L.wav",
  },
];

/**
 * Default global signal chain configuration.
 * Signal flow: Input → [Tuner tap] → Gate → Transpose → [Presets] → EQ → Doubler → Output
 */
export const DEFAULT_GLOBAL_SIGNAL_CHAIN: GlobalSignalChainConfig = {
  inputGain: 0.0,
  monoMode: false,
  inputChannel: 0,
  autoLevelInput: false,
  outputGain: 0.0,
  autoLevelOutput: false,
  limiterEnabled: false,
  preChain: {
    gateEnabled: false,
    gateThreshold: -40.0,
    gateAttack: 0.5,
    gateHold: 50.0,
    gateRelease: 100.0,
    transposeEnabled: false,
    transposeSemitones: 0,
  },
  postChain: {
    eqEnabled: false,
    eqLowGain: 0.0,
    eqLowFreq: 100.0,
    eqLowMidGain: 0.0,
    eqLowMidFreq: 400.0,
    eqLowMidQ: 1.0,
    eqHighMidGain: 0.0,
    eqHighMidFreq: 2000.0,
    eqHighMidQ: 1.0,
    eqHighGain: 0.0,
    eqHighFreq: 8000.0,
    doublerEnabled: false,
    doublerDelay: 20.0,
    doublerMix: 0.5,
    doublerDetune: 5.0,
  },
};

export const uiState: UiState = {
  presets: [],
  filteredPresets: [],
  activePresetId: null,
  presetCache: new Map<string, Preset>(),
  parameters: {
    values: [],
  },
  signalTest: null,
  demoAudioSelectedId: DEMO_AUDIO_SAMPLES.length ? DEMO_AUDIO_SAMPLES[0].id : null,
  demoAudioRepeat: false,
  logs: [],
  resourceLibrary: {},
  appSettings: {},
  tone3000Session: null,
  mixer: {
    activePresetIds: [],
    presets: {},
    masterGain: 1.0,
    limiterEnabled: false,
  },
  uiSettings: { zoom: 1 },
  dspPerformance: undefined,
  dspPerformanceHistory: [],
  globalSignalChain: { ...DEFAULT_GLOBAL_SIGNAL_CHAIN },
};

export function clonePreset<T extends Preset | null>(preset: T): T {
  return preset ? (JSON.parse(JSON.stringify(preset)) as T) : preset;
}
