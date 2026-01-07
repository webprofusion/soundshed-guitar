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
  gateEnabled: boolean;
  gateThreshold: number | null;
  modelPath: string;
  irPath: string;
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

// V2 Preset Format Types
export interface GlobalSettings {
  inputTrim: number;
  outputTrim: number;
  masterVolume: number;
}

export interface ResourceRef {
  type: string;
  id: string;
  filePath?: string;
  embeddedId?: string;
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

export interface UiState {
  presets: Preset[];
  filteredPresets: Preset[];
  activePresetId: string | null;
  presetCache: Map<string, Preset>;
  parameters: ParametersState;
  signalTest: SignalTestResult | null;
  demoAudioSelectedId: string | null;
  demoAudioRepeat: boolean;
  logs: LogEntry[];
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
