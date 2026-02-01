export type AttachmentType = "audiofx" | "ir" | string;

export interface Attachment {
  type: AttachmentType;
  id?: string;
  filePath?: string;
  hash?: string;
  data?: string;
  downloadUrl?: string;
  url?: string;
  href?: string;
  path?: string;
  customModelPath?: string | null;
  customIrPath?: string | null;
}

export interface AudioFxModelEntry {
  id: string;
  filePath: string;
  hash: string;
}

export interface IrLibraryEntry {
  id: string;
  filePath: string;
  hash: string;
}

export interface Parameter {
  id: string;
  value: number | boolean | string;
  label?: string;
}

export interface ParametersState {
  values: Parameter[];
}

export interface SignalTestResult {
  frequency: number;
  duration: number;
  elapsed: number;
  sampleRate: number;
  inputRMS: number;
  outputLeft: number;
  outputRight: number;
  passed: boolean;
  message: string;
}

export interface Preset {
  id: string;
  name: string;
  category?: string;
  description?: string;
  attachments?: Attachment[];
  fxChain?: string[];
  audioFxModelId?: string | null;
  irId?: string | null;
  customModelPath?: string | null;
  customIrPath?: string | null;
  formatVersion?: number;
  graph?: SignalGraph;
  globals?: GlobalSettings;
  embeddedResources?: EmbeddedResource[];
  [key: string]: unknown;
}

export interface PresetFolder {
  id: string;
  name: string;
  children: PresetFolder[];
  presetIds: string[];
}

export interface SetlistSlot {
  presetId: string;
}

export interface Setlist {
  id: string;
  name: string;
  bank?: number | null;
  slots: SetlistSlot[];
}

// V2 Preset Format Types
export interface GlobalSettings {
  inputTrim: number;
  outputTrim: number;
  masterVolume: number;
  autoLevelInput: boolean;
  autoLevelOutput: boolean;
}

export interface UiSettings {
  zoom: number;
  bounds?: {
    x: number;
    y: number;
    width: number;
    height: number;
  };
}

export interface ResourceRef {
  type?: string;
  id?: string;
  resourceType?: string;
  resourceId?: string;
  filePath?: string;
  embeddedId?: string;
  parameterId?: string;
  parameterValue?: number;
  parameters?: Record<string, number>;
}

export interface GraphNode {
  id: string;
  type: string;
  displayName: string;
  category: string;
  bypassed: boolean;
  params: Record<string, number>;
  config: Record<string, string>;
  resource?: ResourceRef;
  resources?: ResourceRef[];
}

export interface GraphEdge {
  from: string;
  to: string;
  fromPort: number;
  toPort: number;
  gain: number;
}

export interface SignalGraph {
  nodes: GraphNode[];
  edges: GraphEdge[];
}

export interface EmbeddedResource {
  id: string;
  type: string;
  name: string;
  hash: string;
  data?: string;
  originalPath?: string;
}

export interface LogEntry {
  timestamp: Date | string;
  message: string;
}

export interface DemoSample {
  id: string;
  title: string;
  path: string;
}

export type AppSettingPrimitive = string | number | boolean | null;
export type AppSettingValue = AppSettingPrimitive | AppSettingObject | AppSettingArray;
export interface AppSettingObject {
  [key: string]: AppSettingValue;
}
export type AppSettingArray = AppSettingValue[];

export type AppSettings = Record<string, AppSettingValue>;

export interface Tone3000Session {
  accessToken: string;
  refreshToken: string;
  expiresAt: number;
}

export interface LibraryResource {
  id: string;
  name: string;
  category: string;
  description: string;
  filePath: string;
  hash?: string;
  metadata?: Record<string, string>;
  fileMissing?: boolean;
}

export interface ResourceLibrary {
  nam?: LibraryResource[];
  ir?: LibraryResource[];
  [key: string]: LibraryResource[] | undefined;
}

export type BlendMode = "snap" | "interpolate";

export interface BlendModelMapping {
  id: string;
  parameterId?: string;
  parameterValue?: number;
  parameters?: Record<string, number>;
}

export interface BlendDefinition {
  id: string;
  name: string;
  category: string;
  models: string[];
  blendMode?: BlendMode;
  controlMap?: Record<string, number>;
  modelMappings?: BlendModelMapping[];
  parameters?: string[];
}

export type BlendLibrary = BlendDefinition[];

export interface MixerPresetState {
  id: string;
  mix: number; // 0..1
  pan: number; // -1..1 (L..R)
  mute: boolean;
  solo: boolean;
}

// Global Signal Chain Configuration
export interface GlobalPreChainConfig {
  gateEnabled: boolean;
  gateThreshold: number;  // dB
  gateAttack: number;     // ms
  gateHold: number;       // ms
  gateRelease: number;    // ms
  transposeEnabled: boolean;
  transposeSemitones: number;  // -36 to +12
}

export interface GlobalPostChainConfig {
  eqEnabled: boolean;
  eqLowGain: number;      // dB
  eqLowFreq: number;      // Hz
  eqLowMidGain: number;   // dB
  eqLowMidFreq: number;   // Hz
  eqLowMidQ: number;
  eqHighMidGain: number;  // dB
  eqHighMidFreq: number;  // Hz
  eqHighMidQ: number;
  eqHighGain: number;     // dB
  eqHighFreq: number;     // Hz
  doublerEnabled: boolean;
  doublerDelay: number;   // ms
  doublerMix: number;     // 0-1
  doublerDetune: number;  // cents
}

export interface GlobalSignalChainConfig {
  inputGain: number;      // dB
  monoMode: boolean;
  inputChannel: number;   // 0=left, 1=right
  autoLevelInput: boolean;
  outputGain: number;     // dB (master volume)
  autoLevelOutput: boolean;
  limiterEnabled: boolean;
  preChain: GlobalPreChainConfig;
  postChain: GlobalPostChainConfig;
}

export interface MixerState {
  activePresetIds: string[];
  presets: Record<string, MixerPresetState>;
  masterGain: number; // linear
  limiterEnabled: boolean;
}

export interface DSPPerformanceStats {
  totalProcessingTimeUs: number;
  realTimeUs: number;
  dspLoadPercent: number;
  nodeProcessingTimesUs: Record<string, number>;
}

export interface SignalLevelMetrics {
  peak: number;
  rms: number;
  peakDbfs: number;
  rmsDbfs: number;
  headroomDb: number;
  clipped: boolean;
  clipCount: number;
}

export interface SignalLevelNodeMetrics {
  scope: "pre" | "post" | "preset" | string;
  presetId?: string;
  nodeId: string;
  nodeType: string;
  levels: SignalLevelMetrics;
}

export interface SignalLevelDiagnostics {
  input: SignalLevelMetrics;
  output: SignalLevelMetrics;
  nodes: SignalLevelNodeMetrics[];
  timestamp?: number;
}

export interface EnvironmentState {
  standalone: boolean;
}

export interface MetronomeClickTypeOption {
  id: string;
  label?: string;
  lowPath?: string;
  highPath?: string;
}

export interface MetronomeState {
  bpm: number;
  enabled: boolean;
  editable: boolean;
  source: "app" | "host";
  volumeDb: number;
  pan: number;
  clickType: string;
  clickTypes?: MetronomeClickTypeOption[];
}

export interface UiState {
  presets: Preset[];
  filteredPresets: Preset[];
  activePresetId: string | null;
  presetCache: Map<string, Preset>;
  activePresetSnapshot?: Preset | null;
  activePresetDraft?: Preset | null;
  presetDirty?: boolean;
  presetFolders?: PresetFolder[];
  activePresetFolderId?: string | null;
  setlists?: Setlist[];
  activeSetlistId?: string | null;
  parameters: ParametersState;
  signalTest: SignalTestResult | null;
  demoAudioSelectedId: string | null;
  demoAudioRepeat: boolean;
  logs: LogEntry[];
  resourceLibrary: ResourceLibrary;
  blendLibrary?: BlendLibrary;
  appSettings: AppSettings;
  tone3000Session?: Tone3000Session | null;
  mixer?: MixerState;
  uiSettings?: UiSettings;
  dspPerformance?: DSPPerformanceStats;
  dspPerformanceHistory: DSPPerformanceStats[];
  globalSignalChain?: GlobalSignalChainConfig;
  signalDiagnostics?: SignalLevelDiagnostics | null;
  environment?: EnvironmentState;
  metronome?: MetronomeState;
  namCalibrationStatus?: Record<string, "calibrating" | "ready" | "failed">;
  missingNodeResources?: Array<{ nodeId: string; resourceType?: string; resourceId?: string; filePath?: string }>;
}

declare global {
  interface Window {
    NAMBridge: { postMessage: (payload: unknown) => void };
    IPlugSendMsg?: (payload: unknown) => void;
    IPlugReceiveData?: (message: string) => void;
    AUDIOFX_REMOTE_BASE_URL?: string;
  }
}

export {};
