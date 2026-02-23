import type { DemoSample, GlobalSignalChainConfig, Preset, SignalGraph, UiState } from "./types.js";
import type { CompositeEffectDefinition } from "./compositeTypes.js";
import { createEmptyLayoutLibrary } from "./layoutTypes.js";

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
    path: "demo/DI_Guitar_L.wav"
     },
   {
    id: "di-audiocheck-whitenoise",
    title: "White Noise (Gaussian)",
    path: "demo/audiocheck.net_whitenoisegaussian.wav",
  },
    {
    id: "di-audiocheck-sweep20-20klog",
    title: "Sweep 20-20kHz (Logarithmic)",
    path: "demo/audiocheck.net_sweep20-20klog.wav",
  },
];

const DEFAULT_PRE_CHAIN_GRAPH: SignalGraph = {
  nodes: [
    {
      id: "__input__",
      type: "input",
      displayName: "Input",
      category: "utility",
      bypassed: false,
      params: {},
      config: {},
    },
    {
      id: "global_gate",
      type: "dynamics_gate",
      displayName: "Noise Gate",
      category: "dynamics",
      bypassed: true,
      params: {
        threshold: -40.0,
        attack: 0.5,
        hold: 50.0,
        release: 100.0,
      },
      config: {},
    },
    {
      id: "global_transpose",
      type: "transpose",
      displayName: "Transpose",
      category: "modulation",
      bypassed: true,
      params: {
        semitones: 0.0,
      },
      config: {},
    },
    {
      id: "__output__",
      type: "output",
      displayName: "Output",
      category: "utility",
      bypassed: false,
      params: {},
      config: {},
    },
  ],
  edges: [
    { from: "__input__", to: "global_gate", fromPort: 0, toPort: 0, gain: 1 },
    { from: "global_gate", to: "global_transpose", fromPort: 0, toPort: 0, gain: 1 },
    { from: "global_transpose", to: "__output__", fromPort: 0, toPort: 0, gain: 1 },
  ],
};

const DEFAULT_POST_CHAIN_GRAPH: SignalGraph = {
  nodes: [
    {
      id: "__input__",
      type: "input",
      displayName: "Input",
      category: "utility",
      bypassed: false,
      params: {},
      config: {},
    },
    {
      id: "global_eq",
      type: "eq_parametric",
      displayName: "Global EQ",
      category: "eq",
      bypassed: true,
      params: {
        lowGain: 0.0,
        lowFreq: 100.0,
        lowMidGain: 0.0,
        lowMidFreq: 400.0,
        lowMidQ: 1.0,
        highMidGain: 0.0,
        highMidFreq: 2000.0,
        highMidQ: 1.0,
        highGain: 0.0,
        highFreq: 8000.0,
      },
      config: {},
    },
    {
      id: "global_doubler",
      type: "delay_doubler",
      displayName: "Doubler",
      category: "modulation",
      bypassed: true,
      params: {
        time: 20.0,
        mix: 0.5,
        detune: 5.0,
      },
      config: {},
    },
    {
      id: "__output__",
      type: "output",
      displayName: "Output",
      category: "utility",
      bypassed: false,
      params: {},
      config: {},
    },
  ],
  edges: [
    { from: "__input__", to: "global_eq", fromPort: 0, toPort: 0, gain: 1 },
    { from: "global_eq", to: "global_doubler", fromPort: 0, toPort: 0, gain: 1 },
    { from: "global_doubler", to: "__output__", fromPort: 0, toPort: 0, gain: 1 },
  ],
};

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
  preChainGraph: DEFAULT_PRE_CHAIN_GRAPH,
  postChainGraph: DEFAULT_POST_CHAIN_GRAPH,
};

export const uiState: UiState = {
  presets: [],
  filteredPresets: [],
  activePresetId: null,
  presetCache: new Map<string, Preset>(),
  activePresetSnapshot: null,
  activePresetDraft: null,
  presetDirty: false,
  presetFolders: [],
  activePresetFolderId: "__all__",
  presetFavorites: new Set<string>(),
  presetRatings: {},
  setlists: [],
  activeSetlistId: null,
  parameters: {
    values: [],
  },
  signalTest: null,
  demoAudioSelectedId: DEMO_AUDIO_SAMPLES.length ? DEMO_AUDIO_SAMPLES[0].id : null,
  demoAudioRepeat: false,
  logs: [],
  resourceLibrary: {},
  blendLibrary: [],
  appSettings: {
    "audio.interfaceCalibration.enabled": true,
    "audio.interfaceCalibration.referenceDbu": 12.0,
    "metronome.clickConfig": [
      {
        id: "drum",
        label: "Drum",
        lowPath: "metronome/kit1/low.wav",
        highPath: "metronome/kit1/high.wav",
      }
    ],
  },
  tone3000Session: null,
  mixer: {
    activePresetIds: [],
    presets: {},
    masterGain: 1.0,
    limiterEnabled: false,
  },
  uiSettings: { zoom: 1 },
  uiViewState: {
    mainPanel: "visualizer",
    presetTab: "details",
    settings: {
      equipmentTab: "settings",
      libraryTab: "tone3000",
      advancedTab: "composites",
    },
  },
  dspPerformance: undefined,
  dspPerformanceHistory: [],
  globalSignalChain: { ...DEFAULT_GLOBAL_SIGNAL_CHAIN },
  signalDiagnostics: null,
  signalPeakHold: null,
  environment: { standalone: false },
  namCalibrationStatus: {},
  missingNodeResources: [],
  layoutLibrary: createEmptyLayoutLibrary(),
  metronome: {
    bpm: 120,
    enabled: false,
    editable: true,
    source: "app",
    volumeDb: -12,
    pan: 0,
    clickType: "click",
    clickTypes: [
      { id: "click", label: "Click" },
      { id: "drum", label: "Drum" },
      { id: "electronic", label: "Electronic" },
    ],
  },
  riffLibrary: {
    path: "",
    riffs: [],
  },
  riffCapture: {
    active: false,
    complete: false,
    takeId: "",
    bars: 1,
    tempoBpm: 120,
    timeSigNum: 4,
    timeSigDen: 4,
    capturedSamples: 0,
    sampleRate: 0,
    hasAudio: false,
    waveformPeaks: [],
  },
  compositeEditMode: false,
  compositeEditDefinition: null,
  compositeEditPreset: null,
};

export function clonePreset<T extends Preset | null>(preset: T): T {
  return preset ? (JSON.parse(JSON.stringify(preset)) as T) : preset;
}

export function getActivePresetForRender(): Preset | null {
  // In composite edit mode, render the composite's inner graph as a synthetic preset
  if (uiState.compositeEditMode && uiState.compositeEditPreset) {
    return uiState.compositeEditPreset;
  }
  if (uiState.activePresetDraft) {
    return uiState.activePresetDraft;
  }
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    return null;
  }
  return uiState.presetCache.get(activePresetId) ?? uiState.presets.find((preset) => preset.id === activePresetId) ?? null;
}

export function setActivePresetSnapshot(preset: Preset | null): void {
  uiState.activePresetSnapshot = preset ? clonePreset(preset) : null;
}

export function setActivePresetDraft(preset: Preset | null): void {
  uiState.activePresetDraft = preset ? clonePreset(preset) : null;
}

export function setPresetDirty(isDirty: boolean): void {
  // Don't mark the preset dirty when editing a composite's inner graph
  if (uiState.compositeEditMode) return;
  uiState.presetDirty = isDirty;
  if (typeof document !== "undefined") {
    document.dispatchEvent(new CustomEvent("presetDirtyChanged"));
  }
}

// ─────────────────────────────────────────────────────────────
// Composite Edit Mode Helpers
// ─────────────────────────────────────────────────────────────

export function isCompositeEditMode(): boolean {
  return !!uiState.compositeEditMode;
}

const ADVANCED_OPTIONS_SETTING = "ui.advancedOptionsEnabled";

export function isAdvancedOptionsEnabled(): boolean {
  return Boolean(uiState.appSettings?.[ADVANCED_OPTIONS_SETTING]);
}

const EXPERIMENTAL_FEATURES_SETTING = "ui.experimentalFeaturesEnabled";

export function isExperimentalFeaturesEnabled(): boolean {
  return Boolean(uiState.appSettings?.[EXPERIMENTAL_FEATURES_SETTING]);
}

export function getCompositeEditDefinition(): CompositeEffectDefinition | null {
  return uiState.compositeEditDefinition ?? null;
}

/**
 * Build a synthetic Preset from a composite's inner graph so the
 * signal path renderer can display it unchanged.
 */
function buildCompositeEditPreset(def: CompositeEffectDefinition): Preset {
  return {
    id: `__composite_edit_${def.id}__`,
    name: def.name,
    category: def.category,
    graph: def.innerGraph,
  };
}

/**
 * Enter composite edit mode. Called when the C++ side sends
 * compositeEditState for the first time (or on explicit enter).
 */
export function enterCompositeEditState(def: CompositeEffectDefinition): void {
  uiState.compositeEditMode = true;
  uiState.compositeEditDefinition = def;
  uiState.compositeEditPreset = buildCompositeEditPreset(def);
}

/**
 * Update the composite's inner graph during editing (called when C++
 * broadcasts compositeEditState after a graph mutation).
 */
export function updateCompositeEditState(def: CompositeEffectDefinition): void {
  uiState.compositeEditDefinition = def;
  uiState.compositeEditPreset = buildCompositeEditPreset(def);
}

/**
 * Exit composite edit mode. Restores the normal preset view.
 */
export function exitCompositeEditState(): void {
  uiState.compositeEditMode = false;
  uiState.compositeEditDefinition = null;
  uiState.compositeEditPreset = null;
}
