/**
 * Shared state for the signal-path feature.
 *
 * signalPath.ts was a single 7,900-line module, so all of this was just local
 * scope. Splitting it into focused modules means the handful of genuinely
 * shared things — the panel's DOM roots, and which node is selected — need one
 * owner. This is it.
 *
 * The DOM lookups run at import time, which is safe: `dist/main.js` is a module
 * script at the end of <body>, so the document is already parsed, and every one
 * of these elements is in the static markup assembled from ui-components/.
 *
 * Mutable values are exposed through accessors rather than as `export let`.
 * An ES module's live bindings are readable by importers but only the declaring
 * module may assign them, so a bare `export let` would compile and then fail at
 * the first write from another file.
 */

// ── DOM roots ──────────────────────────────────────────────────────────────

export const signalPathNodesElement = document.getElementById("signal-path-nodes");
export const nodeParamsPanelElement = document.getElementById("node-params-panel");
export const effectVisualizationElement = document.getElementById("effect-visualization");
export const signalPathAddMenu = document.getElementById("signal-path-add-menu");
export const signalPathAddMenuTrigger = document.getElementById("signal-path-add-menu-trigger") as HTMLButtonElement | null;
export const signalPathAddMenuOptions = document.querySelector<HTMLElement>("#signal-path-add-menu-options");
export const signalPathAddSceneButton = document.getElementById("signal-path-add-scene") as HTMLButtonElement | null;

// ── Selection ──────────────────────────────────────────────────────────────

let selectedNodeId: string | null = null;
let lastSelectedNodeType: string | null = null;
let lastSelectedNodeCategory: string | null = null;

/** The node whose parameters the panel is currently showing. */
export function getSelectedNodeId(): string | null {
  return selectedNodeId;
}

export function setSelectedNodeId(nodeId: string | null): void {
  selectedNodeId = nodeId;
}

/**
 * The type and category of the last selected node, remembered so that
 * re-rendering the chain can restore the selection to an equivalent node when
 * the previous one no longer exists.
 */
export function getLastSelectedNodeType(): string | null {
  return lastSelectedNodeType;
}

export function getLastSelectedNodeCategory(): string | null {
  return lastSelectedNodeCategory;
}

export function setLastSelectedNode(type: string | null, category: string | null): void {
  lastSelectedNodeType = type;
  lastSelectedNodeCategory = category;
}

// ── Per-node caches ────────────────────────────────────────────────────────

/** Rolling averages behind the DSP status readout, keyed by metric name. */
export const selectedNodeDspStatusAverages = new Map<string, number>();

/** Spectrogram scrollback for the input analyzer, keyed by node id. */
export const analyzerSpectrogramHistoryByNode = new Map<string, number[][]>();

export const ANALYZER_SPECTROGRAM_HISTORY_FRAMES = 160;

/** Smoothing factor for the DSP status averages; lower is slower. */
export const DSP_STATUS_AVERAGE_SMOOTHING = 0.01;

/** How often the smoothed DSP averages are redrawn, in milliseconds. */
export const DSP_STATUS_AVERAGE_RENDER_INTERVAL_MS = 500;

let selectedNodeDspStatusNodeId: string | null = null;

/** Which node the DSP status averages belong to, so they reset on selection change. */
export function getSelectedNodeDspStatusNodeId(): string | null {
  return selectedNodeDspStatusNodeId;
}

export function setSelectedNodeDspStatusNodeId(nodeId: string | null): void {
  selectedNodeDspStatusNodeId = nodeId;
}

// ── Mix tab ────────────────────────────────────────────────────────────────

let mixTabActive = false;

/**
 * Whether the chain area is showing the multi-preset mixer instead of the
 * signal chain. Read by the layout code, written by the mixer.
 */
export function isMixTabActive(): boolean {
  return mixTabActive;
}

export function setMixTabActive(active: boolean): void {
  mixTabActive = active;
}
