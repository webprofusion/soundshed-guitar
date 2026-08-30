/**
 * Undo/redo and the A/B switch for the active signal chain.
 *
 * ## What gets recorded
 *
 * The chain the user is editing lives on `uiState.activePresetDraft` (or the
 * focused mixer slot — `getSignalPathPreset()` resolves which). Every edit,
 * whether it is a knob turn or a node being dragged to a new position, ends up
 * there. So rather than instrument the twenty-odd senders that can change it,
 * this listens for `presetDirtyChanged` — which every one of them raises via
 * `setPresetDirty()` — and, a beat later, compares the live graph against the
 * top of the stack. Nothing changed means nothing is recorded, so the broad
 * trigger costs nothing and cannot miss an edit path.
 *
 * The beat matters: topology edits (add/delete/reorder/replace) are applied by
 * the engine and echoed back, so the draft is only correct once that round trip
 * lands. The debounce also coalesces a knob drag into a single undo step.
 *
 * ## What restoring does
 *
 * Restoring by reloading the whole preset would rebuild the DSP graph and
 * reload every NAM model — an audible gap, for what is usually one knob. So a
 * restore whose topology matches the live chain is replayed as targeted
 * parameter/bypass/config messages instead, and only a structural difference
 * falls back to a full `loadPreset`. `historyModel.ts` decides which.
 *
 * ## A and B
 *
 * Two stacks, one live at a time, both seeded from the preset as loaded.
 * Switching applies the other slot's current state and leaves each cursor where
 * the user left it, so A/B is a comparison, not an undo step.
 */

import type { Preset, SignalGraph } from "../types.js";
import { clonePreset, getSignalPathPreset, setPresetDirty, uiState } from "../state.js";
import { findPresetScene, normalizePresetScenes } from "../presetScenes.js";
import { showNotification } from "../notifications.js";
import {
  sendSignalPathNodeBypassUpdate,
  sendSignalPathNodeConfigUpdate,
  sendSignalPathNodeParamUpdate,
} from "./commands.js";
import { pushScenePresetToBackend } from "./mixer.js";
import { getNodeDisplayName } from "./nodeLabels.js";
import { requestNodeParamsRefresh, requestSignalPathRender } from "./render.js";
import {
  ChainHistory,
  cloneGraph,
  createSnapshot,
  describeGraphChange,
  diffNodeStates,
  graphSignature,
  type ChainSlotId,
  type ChainSnapshot,
  type NodeStateChange,
} from "./historyModel.js";

/** How long an edit must stay still before it becomes an undo step. */
const COMMIT_DEBOUNCE_MS = 320;

/**
 * How long after a restore the engine's echo is still treated as part of that
 * restore rather than as a new edit. The engine normalises what it is sent
 * (resource refs, scene ids, migrations), so the graph that comes back is not
 * always byte-identical to the one that went out.
 */
const RESTORE_SETTLE_MS = 1500;

/** Long enough for the `presetLoaded` echo to have landed, short enough to stay inside the settle window. */
const DIRTY_RESYNC_MS = 700;

const history = new ChainHistory();

let initialized = false;
let commitTimer: ReturnType<typeof setTimeout> | null = null;
let restoreSettleUntil = 0;
/** `presetId::sceneId` of the chain the stacks describe; a change re-seeds them. */
let contextKey = "";

let historyRoot: HTMLElement | null = null;
let undoButton: HTMLButtonElement | null = null;
let redoButton: HTMLButtonElement | null = null;
let copyButton: HTMLButtonElement | null = null;
const slotButtons = new Map<ChainSlotId, HTMLButtonElement>();

// ── The chain being edited ─────────────────────────────────────────────────

interface ChainContext {
  preset: Preset;
  graph: SignalGraph;
  sceneId: string | null;
}

/**
 * The preset and graph the signal path is currently editing, or null when there
 * is nothing to track. Composite edit mode is excluded deliberately: it edits a
 * custom effect's inner graph, which never marks the preset dirty and has its
 * own enter/exit lifecycle.
 */
function readChainContext(): ChainContext | null {
  if (uiState.compositeEditMode) {
    return null;
  }
  const preset = getSignalPathPreset();
  if (!preset?.graph) {
    return null;
  }
  return { preset, graph: preset.graph, sceneId: uiState.activePresetSceneId ?? null };
}

/**
 * Re-seeds both stacks when the chain under the cursor becomes a different
 * preset or scene — B holding a chain from a preset the user has since left
 * would overwrite their work the moment they toggled to it.
 */
function syncChainContext(): ChainContext | null {
  const context = readChainContext();
  if (!context) {
    contextKey = "";
    return null;
  }

  const key = `${context.preset.id}::${context.sceneId ?? ""}`;
  if (key !== contextKey || !history.isSeeded()) {
    contextKey = key;
    history.reset(createSnapshot(context.graph, context.preset.id, context.sceneId, "Loaded"));
    refreshChainHistoryControls();
  }
  return context;
}

// ── Recording ──────────────────────────────────────────────────────────────

function scheduleCommit(): void {
  if (commitTimer !== null) {
    clearTimeout(commitTimer);
  }
  commitTimer = setTimeout(() => {
    commitTimer = null;
    commitSignalChainSnapshot();
  }, COMMIT_DEBOUNCE_MS);
}

/**
 * Fold whatever the chain looks like now into the active stack. Safe to call at
 * any time: an unchanged graph records nothing.
 */
export function commitSignalChainSnapshot(): void {
  if (commitTimer !== null) {
    clearTimeout(commitTimer);
    commitTimer = null;
  }

  const context = syncChainContext();
  if (!context) {
    return;
  }

  const current = history.getCurrent();
  if (current && current.signature === graphSignature(context.graph)) {
    return;
  }

  const snapshot = createSnapshot(
    context.graph,
    context.preset.id,
    context.sceneId,
    describeGraphChange(current?.graph, context.graph, getNodeDisplayName),
  );

  if (Date.now() < restoreSettleUntil) {
    history.amendCurrent(snapshot);
  } else {
    history.push(snapshot);
  }
  refreshChainHistoryControls();
}

// ── Restoring ──────────────────────────────────────────────────────────────

/**
 * Replays a same-topology difference as individual node messages, patching the
 * draft as it goes so the UI does not have to wait for the echo.
 */
function applyNodeStateChanges(preset: Preset, changes: NodeStateChange[]): void {
  const liveNodes = new Map((preset.graph?.nodes ?? []).map((node) => [node.id, node]));

  for (const change of changes) {
    const node = liveNodes.get(change.nodeId);
    if (!node) {
      continue;
    }
    for (const { key, value } of change.params) {
      node.params[key] = value;
      sendSignalPathNodeParamUpdate(change.nodeId, key, value);
    }
    for (const { key, value } of change.config) {
      node.config[key] = value;
      sendSignalPathNodeConfigUpdate(change.nodeId, key, value);
    }
    if (change.bypassed !== null) {
      const flags = node as unknown as { bypassed?: boolean; enabled?: boolean };
      flags.bypassed = change.bypassed;
      flags.enabled = !change.bypassed;
      sendSignalPathNodeBypassUpdate(change.nodeId, preset.id, change.bypassed);
    }
  }

  requestSignalPathRender();
  requestNodeParamsRefresh();
}

/** Structural restore: swap the whole graph into the active scene and reload it. */
function applyWholeGraph(preset: Preset, sceneId: string | null, graph: SignalGraph): void {
  const restored = clonePreset(preset);
  const resolvedSceneId = normalizePresetScenes(restored, sceneId ?? undefined);
  const graphCopy = cloneGraph(graph);
  const scene = findPresetScene(restored, resolvedSceneId);
  if (scene) {
    scene.graph = graphCopy;
  }
  restored.graph = graphCopy;

  // `activePresetSnapshot` is the app's record of the preset as last saved, and
  // it is what "are there unsaved changes?" is measured against. Reloading a
  // preset overwrites it — reasonable for a real load, wrong for a restore,
  // which would make stepping back through edits quietly look like a save and
  // drop the discard-changes prompt. Hold onto it and put it back.
  const savedBeforeRestore = uiState.activePresetSnapshot ?? null;
  pushScenePresetToBackend(restored);
  setTimeout(() => resyncDirtyFlagAfterRestore(savedBeforeRestore), DIRTY_RESYNC_MS);
}

function resyncDirtyFlagAfterRestore(saved: Preset | null): void {
  const context = readChainContext();
  if (!context || !saved || saved.id !== context.preset.id) {
    return;
  }
  uiState.activePresetSnapshot = saved;
  setPresetDirty(graphSignature(context.graph) !== graphSignature(saved.graph));
}

function applySnapshot(target: ChainSnapshot): void {
  const context = readChainContext();
  if (!context || context.preset.id !== target.presetId) {
    showNotification("Chain history unavailable", "The active preset changed");
    refreshChainHistoryControls();
    return;
  }

  if (graphSignature(context.graph) === target.signature) {
    refreshChainHistoryControls();
    return;
  }

  restoreSettleUntil = Date.now() + RESTORE_SETTLE_MS;
  const changes = diffNodeStates(context.graph, target.graph);
  if (changes) {
    applyNodeStateChanges(context.preset, changes);
  } else {
    applyWholeGraph(context.preset, context.sceneId, target.graph);
  }
  refreshChainHistoryControls();
}

// ── Public actions ─────────────────────────────────────────────────────────

/** Steps the active slot back one edit. Returns false when there is nothing to undo. */
export function undoSignalChain(): boolean {
  commitSignalChainSnapshot();
  const target = history.undo();
  if (!target) {
    return false;
  }
  applySnapshot(target);
  return true;
}

/** Steps the active slot forward one edit. Returns false when there is nothing to redo. */
export function redoSignalChain(): boolean {
  commitSignalChainSnapshot();
  const target = history.redo();
  if (!target) {
    return false;
  }
  applySnapshot(target);
  return true;
}

/** Makes slot A or B live, applying whatever chain it holds. */
export function setActiveChainSlot(slotId: ChainSlotId): void {
  commitSignalChainSnapshot();
  const target = history.setActiveSlot(slotId);
  if (!target) {
    refreshChainHistoryControls();
    return;
  }
  applySnapshot(target);
}

export function toggleActiveChainSlot(): void {
  setActiveChainSlot(history.getActiveSlotId() === "A" ? "B" : "A");
}

/** Forks the live chain into the other slot so it can be varied and compared. */
export function copyActiveChainSlotToOther(): void {
  commitSignalChainSnapshot();
  const source = history.getActiveSlotId();
  const target = history.copyActiveToOther();
  if (!target) {
    return;
  }
  showNotification(`Chain ${source} copied to ${target}`);
  refreshChainHistoryControls();
}

// ── Footer controls ────────────────────────────────────────────────────────

export function refreshChainHistoryControls(): void {
  const available = history.isSeeded() && !uiState.compositeEditMode;

  if (undoButton) {
    const label = history.peekUndoLabel();
    undoButton.disabled = !available || !history.canUndo();
    undoButton.title = label ? `Undo ${label} (Ctrl+Z)` : "Nothing to undo (Ctrl+Z)";
  }
  if (redoButton) {
    const label = history.peekRedoLabel();
    redoButton.disabled = !available || !history.canRedo();
    redoButton.title = label ? `Redo ${label} (Ctrl+Shift+Z)` : "Nothing to redo (Ctrl+Shift+Z)";
  }

  const activeSlot = history.getActiveSlotId();
  slotButtons.forEach((button, slotId) => {
    const isActive = slotId === activeSlot;
    button.classList.toggle("is-active", isActive);
    button.setAttribute("aria-pressed", String(isActive));
    button.disabled = !available;
    button.title = `Chain ${slotId} — ${history.describeSlot(slotId)}`;
  });

  if (copyButton) {
    copyButton.disabled = !available || history.slotsMatch();
    copyButton.title = `Copy chain ${activeSlot} to ${history.getOtherSlotId()}`;
  }

  historyRoot?.classList.toggle("is-unavailable", !available);
}

// ── Keyboard ───────────────────────────────────────────────────────────────

function isTextEntryTarget(element: Element | null): boolean {
  if (!(element instanceof HTMLElement)) {
    return false;
  }
  if (element.isContentEditable) {
    return true;
  }
  return Boolean(
    element.closest("input, textarea, select, [contenteditable=''], [contenteditable='true'], [role='textbox']"),
  );
}

/**
 * A modal owns its own keyboard while it is up — the layout designer has its
 * own undo stack on the same chord. `.modal` is `position: fixed`, so
 * `offsetParent` is always null and cannot be used to test visibility.
 */
function isModalOpen(): boolean {
  return Array.from(document.querySelectorAll<HTMLElement>(".modal")).some(
    (modal) => modal.getClientRects().length > 0,
  );
}

function handleChainHistoryKeyDown(event: KeyboardEvent): void {
  if (event.defaultPrevented || event.altKey || !(event.ctrlKey || event.metaKey)) {
    return;
  }

  const key = event.key.toLowerCase();
  const isUndo = key === "z" && !event.shiftKey;
  const isRedo = key === "y" || (key === "z" && event.shiftKey);
  if (!isUndo && !isRedo) {
    return;
  }

  if (uiState.compositeEditMode || isModalOpen()) {
    return;
  }
  if (isTextEntryTarget(event.target as Element | null) || isTextEntryTarget(document.activeElement)) {
    return;
  }

  if (isUndo ? undoSignalChain() : redoSignalChain()) {
    event.preventDefault();
  }
}

// ── Wiring ─────────────────────────────────────────────────────────────────

export function initSignalChainHistory(): void {
  if (initialized) {
    return;
  }
  initialized = true;

  historyRoot = document.getElementById("footer-chain-history");
  undoButton = document.getElementById("footer-chain-undo") as HTMLButtonElement | null;
  redoButton = document.getElementById("footer-chain-redo") as HTMLButtonElement | null;
  copyButton = document.getElementById("footer-chain-copy") as HTMLButtonElement | null;

  const slotA = document.getElementById("footer-chain-slot-a") as HTMLButtonElement | null;
  const slotB = document.getElementById("footer-chain-slot-b") as HTMLButtonElement | null;
  if (slotA) slotButtons.set("A", slotA);
  if (slotB) slotButtons.set("B", slotB);

  undoButton?.addEventListener("click", () => void undoSignalChain());
  redoButton?.addEventListener("click", () => void redoSignalChain());
  copyButton?.addEventListener("click", copyActiveChainSlotToOther);
  slotButtons.forEach((button, slotId) => {
    button.addEventListener("click", () => setActiveChainSlot(slotId));
  });

  document.addEventListener("keydown", handleChainHistoryKeyDown);
  document.addEventListener("presetDirtyChanged", scheduleCommit);

  syncChainContext();
  refreshChainHistoryControls();
}
