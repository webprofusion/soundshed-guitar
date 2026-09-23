import { updateAppSetting } from "./appSettingsStore.js";
import { postMessage } from "./bridge.js";
import { showNotification } from "./notifications.js";
import { showConfirm } from "./dialogs.js";
import {
  assignPresetToActiveSetlistSlot,
  clearActiveSetlistSlot,
  createSetlist,
  deleteActiveSetlist,
  isOnlyPlayingPreset,
  updateActiveSetlistDetails,
} from "./presets.js";
import { clonePreset, getActivePresetForRender, uiState } from "./state.js";
import { setPresetLoadingId } from "./presetLibraryStore.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "./featureFlags.js";
import type { AppSettingValue, GraphNode, Preset, Setlist } from "./types.js";
import { escapeHtml } from "./utils.js";
import {
  applySignalPathNodeBypassState,
  isNodeBypassed,
  isToggleableSignalPathNode,
  renderSignalPathBar,
} from "./signalPath.js";

type PerformancePadMode = "setlist" | "effects";
type PerformancePadCount = 4 | 6 | 8;

interface PerformancePadAssignment {
  padIndex: number;
  nodeId: string;
  effectType: string;
  label: string;
}

type PerformancePadAssignments = Record<string, PerformancePadAssignment[]>;

const SETTINGS = {
  mode: "performancePads.mode",
  padCount: "performancePads.padCount",
  assignments: "performancePads.assignments",
} as const;

const VALID_PAD_COUNTS: PerformancePadCount[] = [4, 6, 8];

let rootElement: HTMLElement | null = null;
let mode: PerformancePadMode = "setlist";
let padCount: PerformancePadCount = 8;
let assignments: PerformancePadAssignments = {};
let draftPadIndex = 0;
let draftEffectType = "";
let draftNodeId = "";
let editingSetlistId: string | null = null;
let longPressTimer: ReturnType<typeof setTimeout> | null = null;

function isPerformancePadMode(value: unknown): value is PerformancePadMode {
  return value === "setlist" || value === "effects";
}

function normalizePadCount(value: unknown): PerformancePadCount {
  return VALID_PAD_COUNTS.includes(value as PerformancePadCount) ? value as PerformancePadCount : 8;
}

function normalizeAssignments(value: unknown): PerformancePadAssignments {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return {};
  }

  const next: PerformancePadAssignments = {};
  Object.entries(value as Record<string, unknown>).forEach(([presetId, entries]) => {
    if (!Array.isArray(entries)) {
      return;
    }
    const normalized = entries
      .map((entry): PerformancePadAssignment | null => {
        if (!entry || typeof entry !== "object") {
          return null;
        }
        const raw = entry as Record<string, unknown>;
        const rawPadIndex = raw.padIndex;
        const nodeId = typeof raw.nodeId === "string" ? raw.nodeId.trim() : "";
        const effectType = typeof raw.effectType === "string" ? raw.effectType.trim() : "";
        const label = typeof raw.label === "string" ? raw.label.trim() : "";
        if (typeof rawPadIndex !== "number" || !Number.isInteger(rawPadIndex) || rawPadIndex < 0 || !nodeId) {
          return null;
        }
        return {
          padIndex: rawPadIndex,
          nodeId,
          effectType,
          label,
        };
      })
      .filter((entry): entry is PerformancePadAssignment => Boolean(entry));
    if (normalized.length) {
      next[presetId] = normalized;
    }
  });
  return next;
}

function writeSetting(key: string, value: AppSettingValue): void {
  updateAppSetting(key, value);
}

function persistMode(): void {
  writeSetting(SETTINGS.mode, mode);
}

function persistPadCount(): void {
  writeSetting(SETTINGS.padCount, padCount);
}

function persistAssignments(): void {
  writeSetting(SETTINGS.assignments, assignments as unknown as AppSettingValue);
}

function getActiveSetlist(): Setlist | null {
  const activeId = uiState.activeSetlistId;
  const setlists = uiState.setlists ?? [];
  return setlists.find((setlist) => setlist.id === activeId) ?? setlists[0] ?? null;
}

function findPresetName(presetId: string): string {
  return uiState.presetCache.get(presetId)?.name
    ?? uiState.presets.find((preset) => preset.id === presetId)?.name
    ?? presetId;
}

function getSetlistBankLabel(setlist: Setlist | null): string {
  if (!setlist) {
    return "No bank";
  }
  return typeof setlist.bank === "number" ? `Bank ${setlist.bank}` : "Unassigned bank";
}

function isEditingCurrentSetlist(setlist: Setlist | null): boolean {
  return Boolean(setlist && editingSetlistId === setlist.id);
}

function getNextSuggestedBank(setlists: Setlist[]): number {
  const usedBanks = setlists
    .map((setlist) => setlist.bank)
    .filter((bank): bank is number => typeof bank === "number" && Number.isFinite(bank));
  if (!usedBanks.length) {
    return 1;
  }
  return Math.max(...usedBanks) + 1;
}

function buildDefaultSetlistName(bank: number): string {
  return `Bank ${bank}`;
}

function areEffectsPerformancePadsEnabled(): boolean {
  return isFeatureEnabled(Features.EffectsPerformancePads);
}

function setMode(nextMode: PerformancePadMode): void {
  if (nextMode === "effects" && !areEffectsPerformancePadsEnabled()) {
    nextMode = "setlist";
  }
  if (mode === nextMode) {
    return;
  }
  mode = nextMode;
  persistMode();
  renderPerformancePads();
}

function setPadCount(nextCount: PerformancePadCount): void {
  if (padCount === nextCount) {
    return;
  }
  padCount = nextCount;
  if (draftPadIndex >= padCount) {
    draftPadIndex = 0;
  }
  persistPadCount();
  renderPerformancePads();
}

function setActiveSetlist(setlist: Setlist): void {
  uiState.activeSetlistId = setlist.id;
  uiState.setlistCursorIndex = 0;
  editingSetlistId = null;
  // The engine stores the choice and answers with "setlistCursorChanged"; the setlists
  // themselves are unchanged, so they are not re-sent.
  postMessage({ type: "selectSetlist", setlistId: setlist.id });
  renderPerformancePads();
}

function changeSetlistBank(delta: number): void {
  const setlists = uiState.setlists ?? [];
  if (!setlists.length) {
    showNotification("No setlists", "Create a setlist before using bank controls.");
    return;
  }

  const active = getActiveSetlist();
  const currentIndex = Math.max(0, setlists.findIndex((setlist) => setlist.id === active?.id));
  const nextIndex = Math.max(0, Math.min(setlists.length - 1, currentIndex + delta));
  if (nextIndex === currentIndex) {
    showNotification(delta > 0 ? "Last bank" : "First bank");
    return;
  }
  setActiveSetlist(setlists[nextIndex]);
}

function startEditingSetlist(): void {
  const setlist = getActiveSetlist();
  if (!setlist) {
    showNotification("No setlist selected", "Create a setlist before editing it.");
    return;
  }
  editingSetlistId = setlist.id;
  renderPerformancePads();
}

function cancelEditingSetlist(): void {
  if (!editingSetlistId) {
    return;
  }
  editingSetlistId = null;
  renderPerformancePads();
}

function saveEditedSetlist(): void {
  if (!rootElement) {
    return;
  }
  const nameInput = rootElement.querySelector<HTMLInputElement>("#performance-setlist-name");
  const name = nameInput?.value ?? "";
  // Bank number is auto-assigned; preserve the existing one when editing
  const setlist = getActiveSetlist();
  const existingBank = setlist?.bank ?? null;
  const setlists = uiState.setlists ?? [];
  const bank = existingBank !== null
    ? existingBank
    : getNextSuggestedBank(setlists.filter((s) => s.id !== setlist?.id));
  if (!updateActiveSetlistDetails(name, bank)) {
    return;
  }
  editingSetlistId = null;
  renderPerformancePads();
}

async function deleteCurrentSetlistWithConfirm(): Promise<void> {
  const setlist = getActiveSetlist();
  if (!setlist) {
    showNotification("No setlist selected", "Create a setlist before deleting it.");
    return;
  }
  const confirmed = await showConfirm(`Delete "${setlist.name}"?`, "Delete setlist");
  if (!confirmed) {
    return;
  }
  if (!deleteActiveSetlist()) {
    return;
  }
  editingSetlistId = null;
  renderPerformancePads();
}

function addNewSetlist(): void {
  const setlists = uiState.setlists ?? [];
  const nextBank = getNextSuggestedBank(setlists);
  const created = createSetlist(buildDefaultSetlistName(nextBank), nextBank);
  if (!created) {
    return;
  }
  editingSetlistId = created.id;
  renderPerformancePads();
}

function selectSetlistSlot(index: number): void {
  const setlist = getActiveSetlist();
  if (!setlist) {
    showNotification("No setlist selected", "Create or select a setlist first.");
    return;
  }
  // An index outside the setlist has no preset either.
  const presetId = setlist.slots[index]?.presetId ?? "";
  if (!presetId) {
    showNotification("Empty path", "Assign a preset to this setlist slot first.");
    return;
  }

  uiState.setlistCursorIndex = index;
  // The backend switches the preset off this one message and reports it with "presetLoaded",
  // which re-renders the pads. Loading it from here as well rebuilt the whole DSP graph a
  // second time and doubled the switch time. A pad for the preset already playing loads nothing.
  if (!isOnlyPlayingPreset(presetId)) {
    setPresetLoadingId(presetId);
  }
  postMessage({ type: "setSetlistCursor", cursorIndex: index });
  renderPerformancePads();
}

/** Assign a preset to a pad of the active setlist, asking first when it would replace a different one. */
export async function assignSetlistSlot(index: number, presetId: string): Promise<void> {
  const setlist = getActiveSetlist();
  if (!setlist) {
    showNotification("No setlist selected", "Create or select a setlist first.");
    return;
  }
  if (!presetId) {
    showNotification("No preset selected", "Choose a preset in the main preset chooser first.");
    return;
  }

  const existingPresetId = setlist.slots[index]?.presetId?.trim() ?? "";
  if (existingPresetId === presetId) {
    showNotification("Path unchanged", `Pad ${index + 1} already uses ${findPresetName(presetId)}.`);
    return;
  }

  if (existingPresetId) {
    const confirmed = await showConfirm(
      `Replace pad ${index + 1} from "${findPresetName(existingPresetId)}" to "${findPresetName(presetId)}"?`,
      "Replace setlist path",
    );
    if (!confirmed) {
      return;
    }
  }

  const assigned = assignPresetToActiveSetlistSlot(index, presetId);
  if (!assigned) {
    showNotification("Assign failed", "Could not update the active setlist slot.");
    return;
  }

  showNotification("Path saved", `Pad ${index + 1}: ${findPresetName(presetId)}`);
  renderPerformancePads();
}

async function clearSelectedSetlistSlotWithConfirm(): Promise<void> {
  if (mode !== "setlist") {
    return;
  }
  const setlist = getActiveSetlist();
  if (!setlist) {
    return;
  }
  const slotIndex = uiState.setlistCursorIndex ?? 0;
  if (slotIndex < 0 || slotIndex >= setlist.slots.length) {
    return;
  }
  const existingPresetId = setlist.slots[slotIndex]?.presetId?.trim() ?? "";
  if (!existingPresetId) {
    return;
  }
  const confirmed = await showConfirm(
    `Clear pad ${slotIndex + 1} assignment for "${findPresetName(existingPresetId)}"?`,
    "Clear pad assignment",
  );
  if (!confirmed) {
    return;
  }
  const cleared = clearActiveSetlistSlot(slotIndex);
  if (!cleared) {
    showNotification("Clear failed", "Could not clear the selected pad assignment.");
    return;
  }
  showNotification("Assignment cleared", `Pad ${slotIndex + 1} is now empty.`);
  renderPerformancePads();
}

function getPresetAssignments(presetId: string): PerformancePadAssignment[] {
  return assignments[presetId] ?? [];
}

function setPresetAssignments(presetId: string, nextAssignments: PerformancePadAssignment[]): void {
  assignments = {
    ...assignments,
    [presetId]: nextAssignments.sort((left, right) => left.padIndex - right.padIndex),
  };
  persistAssignments();
}

function getNodeEffectType(node: GraphNode): string {
  return EffectTypeRegistry.resolve(node.type);
}

function getNodeTypeLabel(node: GraphNode): string {
  const typeInfo = EffectTypeRegistry.get(node.type);
  return typeInfo?.displayName || node.displayName || node.type;
}

function getNodeLabel(node: GraphNode): string {
  const typeLabel = getNodeTypeLabel(node);
  if (node.displayName && node.displayName !== typeLabel) {
    return `${node.displayName} · ${typeLabel}`;
  }
  return typeLabel;
}

function getAssignableNodes(preset: Preset | null): GraphNode[] {
  return (preset?.graph?.nodes ?? []).filter((node) => isToggleableSignalPathNode(node));
}

function getAssignmentForPad(presetId: string, padIndex: number): PerformancePadAssignment | null {
  return getPresetAssignments(presetId).find((assignment) => assignment.padIndex === padIndex) ?? null;
}

function getAssignedNode(preset: Preset, assignment: PerformancePadAssignment): GraphNode | null {
  return preset.graph?.nodes.find((node) => node.id === assignment.nodeId) ?? null;
}

function toggleEffectPad(padIndex: number): void {
  if (!areEffectsPerformancePadsEnabled()) {
    return;
  }
  const preset = getActivePresetForRender();
  if (!preset?.id) {
    showNotification("No active preset", "Load a preset before using effect pads.");
    return;
  }
  const assignment = getAssignmentForPad(preset.id, padIndex);
  if (!assignment) {
    draftPadIndex = padIndex;
    showNotification("Pad is unassigned", "Choose an effect below and press Assign.");
    renderPerformancePads();
    return;
  }

  const node = getAssignedNode(preset, assignment);
  if (!node) {
    showNotification("Assigned effect missing", "Reassign this pad to a node in the current chain.");
    renderPerformancePads();
    return;
  }

  applySignalPathNodeBypassState(node, preset, !isNodeBypassed(node));
  renderPerformancePads();
}

function assignDraftPad(): void {
  if (!areEffectsPerformancePadsEnabled()) {
    return;
  }
  const preset = getActivePresetForRender();
  if (!preset?.id) {
    showNotification("No active preset", "Load a preset before assigning effect pads.");
    return;
  }
  const nodes = getAssignableNodes(preset);
  const node = nodes.find((candidate) => candidate.id === draftNodeId)
    ?? nodes.find((candidate) => getNodeEffectType(candidate) === draftEffectType)
    ?? nodes[0]
    ?? null;
  if (!node) {
    showNotification("No assignable effects", "Add a toggleable effect to the signal chain first.");
    return;
  }

  const nextAssignment: PerformancePadAssignment = {
    padIndex: draftPadIndex,
    nodeId: node.id,
    effectType: getNodeEffectType(node),
    label: getNodeLabel(node),
  };
  const next = getPresetAssignments(preset.id)
    .filter((assignment) => assignment.padIndex !== draftPadIndex);
  next.push(nextAssignment);
  setPresetAssignments(preset.id, next);
  showNotification("Pad assigned", `Pad ${draftPadIndex + 1}: ${nextAssignment.label}`);
  renderPerformancePads();
}

function clearDraftPad(): void {
  if (!areEffectsPerformancePadsEnabled()) {
    return;
  }
  const preset = getActivePresetForRender();
  if (!preset?.id) {
    return;
  }
  const next = getPresetAssignments(preset.id)
    .filter((assignment) => assignment.padIndex !== draftPadIndex);
  setPresetAssignments(preset.id, next);
  renderPerformancePads();
}

function renderModeButton(nextMode: PerformancePadMode, label: string): string {
  const active = mode === nextMode;
  return `
    <button
      class="performance-mode-btn${active ? " is-active" : ""}"
      type="button"
      data-performance-action="mode"
      data-mode="${nextMode}"
      aria-pressed="${active}"
    >${label}</button>
  `;
}

function renderHeaderActionButton(action: string, label: string): string {
  return `
    <button
      class="performance-header-action"
      type="button"
      data-performance-action="${action}"
    >${label}</button>
  `;
}

function renderHeader(): string {
  const setlists = uiState.setlists ?? [];
  const activeSetlist = getActiveSetlist();
  const activeIndex = activeSetlist ? setlists.findIndex((candidate) => candidate.id === activeSetlist.id) : -1;
  const showSetlistActions = mode === "setlist";
  const showAddAction = showSetlistActions && (activeIndex < 0 || activeIndex === setlists.length - 1);
  return `
    <header class="performance-pad-header">
      ${showSetlistActions ? `
        <div class="performance-header-actions" aria-label="Setlist actions">
          ${showAddAction ? renderHeaderActionButton("add-setlist", "Add bank") : ""}
        </div>
      ` : '<div class="performance-header-actions" aria-hidden="true"></div>'}
      <div class="performance-pad-controls" aria-label="Performance pad controls">
        <div class="performance-mode-switch" role="group" aria-label="Pad mode">
          ${renderModeButton("setlist", "Setlist")}
          ${areEffectsPerformancePadsEnabled() ? renderModeButton("effects", "Effects") : ""}
        </div>
        <label class="performance-pad-count">
          <span>Pads</span>
          <select id="performance-pad-count-select" class="themed-select">
            ${VALID_PAD_COUNTS.map((count) => `<option value="${count}"${count === padCount ? " selected" : ""}>${count}</option>`).join("")}
          </select>
        </label>
      </div>
    </header>
  `;
}

function renderSetlistMode(): string {
  const setlist = getActiveSetlist();
  const setlists = uiState.setlists ?? [];
  const activeIndex = setlist ? setlists.findIndex((candidate) => candidate.id === setlist.id) : -1;
  const activeCursor = uiState.setlistCursorIndex ?? 0;
  const slots = setlist?.slots ?? [];
  const editing = isEditingCurrentSetlist(setlist);
  const visibleSlots = Array.from({ length: padCount }, (_, padIndex) => {
    const slot = slots[padIndex] ?? null;
    const presetName = slot?.presetId ? findPresetName(slot.presetId) : "";
    const active = padIndex === activeCursor;
    const disabled = !slot?.presetId;
    return `
      <div class="performance-setlist-pad-wrap" data-slot-index="${padIndex}">
        <button
          class="performance-pad performance-setlist-pad${active ? " is-active" : ""}${disabled ? " is-empty" : ""}"
          type="button"
          data-performance-action="setlist-slot"
          data-slot-index="${padIndex}"
          aria-pressed="${active}"
          ${disabled ? "aria-disabled=\"true\"" : ""}
        >
          <span class="performance-pad-number">${padIndex + 1}</span>
          <span class="performance-pad-main">${disabled ? "Empty Path" : escapeHtml(presetName)}</span>
          <span class="performance-pad-sub">${active ? "Current" : disabled ? "Assign in setlist" : "Tap to load"}</span>
        </button>
        <button
          class="performance-pad-assign-btn"
          type="button"
          data-performance-action="assign-setlist-slot"
          data-slot-index="${padIndex}"
          aria-label="Save current preset to pad ${padIndex + 1}"
          title="Save current preset"
        >
          <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
            <path d="M6 4h9l3 3v13H6zM9 4v5h6M9 16h6" />
          </svg>
        </button>
      </div>
    `;
  }).join("");

  const currentBankPanel = setlist ? (editing ? `
      <div class="performance-bank-current performance-bank-editor">
        <div class="performance-bank-editor-fields">
          <label class="performance-bank-editor-field">
            <span>Name</span>
            <input id="performance-setlist-name" type="text" value="${escapeHtml(setlist.name)}" />
          </label>
        </div>
        <div class="performance-bank-editor-actions">
          <button class="performance-bank-inline-btn is-primary" type="button" data-performance-action="save-setlist-edit">Save</button>
          <button class="performance-bank-inline-btn" type="button" data-performance-action="cancel-setlist-edit">Cancel</button>
          <button class="performance-bank-inline-btn is-danger" type="button" data-performance-action="delete-setlist">Delete</button>
        </div>
      </div>
    ` : `
      <div class="performance-bank-current">
        <button
          class="performance-bank-inline-action"
          type="button"
          data-performance-action="edit-setlist"
          aria-label="Edit current bank"
          title="Edit bank"
        >Edit</button>
        <span>${escapeHtml(getSetlistBankLabel(setlist))}</span>
        <strong>${escapeHtml(setlist.name)}</strong>
        <small>${slots.length ? `${Math.min(padCount, slots.length)} of ${slots.length} paths visible` : "No paths assigned"}</small>
      </div>
    `) : `
      <div class="performance-bank-current">
        <span>No bank</span>
        <strong>No setlist selected</strong>
        <small>Create a bank to start assigning paths.</small>
      </div>
    `;

  return `
    <section class="performance-pad-mode performance-pad-mode-setlist" aria-label="Setlist performance pads">
      <div class="performance-bank-strip">
        <button class="performance-bank-btn performance-bank-nav-btn" type="button" data-performance-action="bank-down" aria-label="Previous bank" ${activeIndex <= 0 ? "disabled" : ""}>
          <svg class="performance-bank-arrow" viewBox="0 0 24 24" aria-hidden="true" focusable="false"><polygon points="12,18 2,6 22,6"/></svg>
          <small>${activeIndex > 0 ? escapeHtml(setlists[activeIndex - 1]?.name ?? "") : "First"}</small>
        </button>
        ${currentBankPanel}
        <button class="performance-bank-btn performance-bank-nav-btn" type="button" data-performance-action="bank-up" aria-label="Next bank" ${activeIndex < 0 || activeIndex >= setlists.length - 1 ? "disabled" : ""}>
          <svg class="performance-bank-arrow" viewBox="0 0 24 24" aria-hidden="true" focusable="false"><polygon points="12,6 22,18 2,18"/></svg>
          <small>${activeIndex >= 0 && activeIndex < setlists.length - 1 ? escapeHtml(setlists[activeIndex + 1]?.name ?? "") : "Last"}</small>
        </button>
      </div>
      ${setlist ? `<div class="performance-pad-grid performance-pad-grid-${padCount}">${visibleSlots}</div>` : `
        <div class="performance-pad-empty">
          <strong>No setlist selected</strong>
          <span>Create a setlist in the preset library, then return here for live path selection.</span>
        </div>
      `}
    </section>
  `;
}

function ensureDraftSelection(preset: Preset | null): void {
  const nodes = getAssignableNodes(preset);
  if (!nodes.length) {
    draftEffectType = "";
    draftNodeId = "";
    return;
  }

  const hasType = nodes.some((node) => getNodeEffectType(node) === draftEffectType);
  if (!hasType) {
    draftEffectType = getNodeEffectType(nodes[0]);
  }

  const nodesForType = nodes.filter((node) => getNodeEffectType(node) === draftEffectType);
  if (!nodesForType.some((node) => node.id === draftNodeId)) {
    draftNodeId = nodesForType[0]?.id ?? nodes[0].id;
  }
}

function renderEffectAssignmentPanel(preset: Preset | null): string {
  ensureDraftSelection(preset);
  const nodes = getAssignableNodes(preset);
  const typeOptions = Array.from(new Map(nodes.map((node) => [getNodeEffectType(node), getNodeTypeLabel(node)])));
  const nodeOptions = nodes.filter((node) => !draftEffectType || getNodeEffectType(node) === draftEffectType);

  if (!preset) {
    return `
      <div class="performance-assignment-panel is-empty">
        Load a preset to assign effects to pads.
      </div>
    `;
  }

  if (!nodes.length) {
    return `
      <div class="performance-assignment-panel is-empty">
        Add a toggleable effect to the active chain before assigning pads.
      </div>
    `;
  }

  return `
    <div class="performance-assignment-panel" aria-label="Effect pad assignment">
      <label>
        <span>Pad</span>
        <select id="performance-assign-pad" class="themed-select">
          ${Array.from({ length: padCount }, (_, index) => `<option value="${index}"${index === draftPadIndex ? " selected" : ""}>Pad ${index + 1}</option>`).join("")}
        </select>
      </label>
      <label>
        <span>Effect type</span>
        <select id="performance-assign-effect-type" class="themed-select">
          ${typeOptions.map(([type, label]) => `<option value="${escapeHtml(type)}"${type === draftEffectType ? " selected" : ""}>${escapeHtml(label)}</option>`).join("")}
        </select>
      </label>
      <label>
        <span>Node</span>
        <select id="performance-assign-node" class="themed-select">
          ${nodeOptions.map((node) => `<option value="${escapeHtml(node.id)}"${node.id === draftNodeId ? " selected" : ""}>${escapeHtml(getNodeLabel(node))}</option>`).join("")}
        </select>
      </label>
      <div class="performance-assignment-actions">
        <button class="performance-assign-btn" type="button" data-performance-action="assign-effect">Assign</button>
        <button class="performance-clear-btn" type="button" data-performance-action="clear-effect">Clear Pad</button>
      </div>
    </div>
  `;
}

function renderEffectsMode(): string {
  const preset = getActivePresetForRender();
  const presetId = preset?.id ?? "";
  const presetName = preset?.name ?? "No preset loaded";
  const padHtml = Array.from({ length: padCount }, (_, padIndex) => {
    const assignment = presetId ? getAssignmentForPad(presetId, padIndex) : null;
    const node = preset && assignment ? getAssignedNode(preset, assignment) : null;
    const missing = Boolean(assignment && !node);
    const on = Boolean(node && !isNodeBypassed(node));
    const assignedLabel = node ? getNodeLabel(node) : assignment?.label || "Unassigned";
    return `
      <button
        class="performance-pad performance-effect-pad${assignment ? " is-assigned" : " is-empty"}${on ? " is-on" : " is-off"}${missing ? " is-missing" : ""}${padIndex === draftPadIndex ? " is-editing" : ""}"
        type="button"
        data-performance-action="effect-pad"
        data-pad-index="${padIndex}"
        aria-pressed="${on}"
      >
        <span class="performance-pad-number">${padIndex + 1}</span>
        <span class="performance-pad-main">${escapeHtml(assignedLabel)}</span>
        <span class="performance-pad-sub">${missing ? "Missing" : assignment ? (on ? "On" : "Off") : "Tap to assign"}</span>
      </button>
    `;
  }).join("");

  return `
    <section class="performance-pad-mode performance-pad-mode-effects" aria-label="Effect bypass pads">
      <div class="performance-effect-context">
        <span>Active preset</span>
        <strong>${escapeHtml(presetName)}</strong>
        <small>Tap assigned pads to toggle bypass. Use assignment controls for the selected pad.</small>
      </div>
      <div class="performance-pad-grid performance-pad-grid-${padCount}">${padHtml}</div>
      ${renderEffectAssignmentPanel(preset ? clonePreset(preset) : null)}
    </section>
  `;
}

function renderPerformancePads(): void {
  if (!rootElement) {
    return;
  }

  rootElement.innerHTML = `
    ${renderHeader()}
    <div class="performance-pad-body">
      ${mode === "setlist" ? renderSetlistMode() : renderEffectsMode()}
    </div>
  `;
}

function handleRootClick(event: MouseEvent): void {
  const target = (event.target as HTMLElement | null)?.closest<HTMLElement>("[data-performance-action]");
  if (!target || !rootElement?.contains(target)) {
    return;
  }

  const action = target.dataset.performanceAction ?? "";
  if (action === "mode") {
    const nextMode = target.dataset.mode;
    if (isPerformancePadMode(nextMode)) {
      setMode(nextMode);
    }
    return;
  }
  if (action === "bank-down") {
    changeSetlistBank(-1);
    return;
  }
  if (action === "bank-up") {
    changeSetlistBank(1);
    return;
  }
  if (action === "edit-setlist") {
    startEditingSetlist();
    return;
  }
  if (action === "save-setlist-edit") {
    saveEditedSetlist();
    return;
  }
  if (action === "cancel-setlist-edit") {
    cancelEditingSetlist();
    return;
  }
  if (action === "delete-setlist") {
    void deleteCurrentSetlistWithConfirm();
    return;
  }
  if (action === "add-setlist") {
    addNewSetlist();
    return;
  }
  if (action === "setlist-slot") {
    selectSetlistSlot(Number.parseInt(target.dataset.slotIndex ?? "-1", 10));
    return;
  }
  if (action === "effect-pad") {
    toggleEffectPad(Number.parseInt(target.dataset.padIndex ?? "-1", 10));
    return;
  }
  if (action === "assign-effect") {
    assignDraftPad();
    return;
  }
  if (action === "clear-effect") {
    clearDraftPad();
  }
}

function handleRootChange(event: Event): void {
  const target = event.target as HTMLSelectElement | null;
  if (!target || !rootElement?.contains(target)) {
    return;
  }

  if (target.id === "performance-pad-count-select") {
    setPadCount(normalizePadCount(Number.parseInt(target.value, 10)));
    return;
  }
  if (target.id === "performance-assign-pad") {
    draftPadIndex = Math.max(0, Math.min(padCount - 1, Number.parseInt(target.value, 10)));
    renderPerformancePads();
    return;
  }
  if (target.id === "performance-assign-effect-type") {
    draftEffectType = target.value;
    draftNodeId = "";
    renderPerformancePads();
    return;
  }
  if (target.id === "performance-assign-node") {
    draftNodeId = target.value;
    renderPerformancePads();
  }
}

const LONG_PRESS_MS = 500;

function cancelLongPress(): void {
  if (longPressTimer !== null) {
    clearTimeout(longPressTimer);
    longPressTimer = null;
  }
}

function isEditableKeyboardTarget(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) {
    return false;
  }
  if (target instanceof HTMLInputElement || target instanceof HTMLTextAreaElement || target instanceof HTMLSelectElement) {
    return true;
  }
  return target.isContentEditable;
}

function handleAssignPointerDown(event: PointerEvent): void {
  const target = (event.target as HTMLElement | null)?.closest<HTMLElement>("[data-performance-action='assign-setlist-slot']");
  if (!target || !rootElement?.contains(target)) {
    return;
  }
  // Prevent the normal click from firing for this element
  event.preventDefault();
  cancelLongPress();

  const slotIndex = Number.parseInt(target.dataset.slotIndex ?? "-1", 10);
  target.setPointerCapture(event.pointerId);

  longPressTimer = setTimeout(() => {
    longPressTimer = null;
    void assignSetlistSlot(slotIndex, uiState.activePresetId?.trim() ?? "");
  }, LONG_PRESS_MS);
}

function handleAssignPointerUp(event: PointerEvent): void {
  const target = (event.target as HTMLElement | null)?.closest<HTMLElement>("[data-performance-action='assign-setlist-slot']");
  if (!target || !rootElement?.contains(target)) {
    cancelLongPress();
    return;
  }

  if (longPressTimer !== null) {
    // Short tap — cancel long press, show hint, and load existing preset if any
    cancelLongPress();
    const slotIndex = Number.parseInt(target.dataset.slotIndex ?? "-1", 10);
    const setlist = getActiveSetlist();
    const existingPresetId = setlist?.slots[slotIndex]?.presetId?.trim() ?? "";
    if (existingPresetId) {
      selectSetlistSlot(slotIndex);
    }
    showNotification("Hold to assign", "Press and hold the save button to assign the current preset to this pad.");
  }
}

function handlePerformancePadsKeydown(event: KeyboardEvent): void {
  if (event.key !== "Delete") {
    return;
  }
  if (!rootElement || mode !== "setlist") {
    return;
  }
  if (rootElement.offsetParent === null || isEditableKeyboardTarget(event.target)) {
    return;
  }
  event.preventDefault();
  void clearSelectedSetlistSlotWithConfirm();
}

export function applyPerformancePadAppSettings(settings = uiState.appSettings): void {
  const storedMode = settings?.[SETTINGS.mode];
  mode = isPerformancePadMode(storedMode) && (storedMode !== "effects" || areEffectsPerformancePadsEnabled())
    ? storedMode
    : "setlist";
  padCount = normalizePadCount(settings?.[SETTINGS.padCount]);
  assignments = normalizeAssignments(settings?.[SETTINGS.assignments]);
  editingSetlistId = null;
  if (draftPadIndex >= padCount) {
    draftPadIndex = 0;
  }
  renderPerformancePads();
}

export function refreshPerformancePads(): void {
  if (!rootElement) {
    return;
  }
  renderPerformancePads();
  renderSignalPathBar();
}

export function initializePerformancePads(): void {
  rootElement = document.getElementById("panel-performance");
  if (!rootElement || rootElement.dataset.bound === "true") {
    return;
  }

  rootElement.dataset.bound = "true";
  rootElement.addEventListener("click", handleRootClick);
  rootElement.addEventListener("change", handleRootChange);
  rootElement.addEventListener("pointerdown", handleAssignPointerDown);
  rootElement.addEventListener("pointerup", handleAssignPointerUp);
  rootElement.addEventListener("pointercancel", cancelLongPress);
  document.addEventListener("keydown", handlePerformancePadsKeydown);
  document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => {
    if (!areEffectsPerformancePadsEnabled() && mode === "effects") {
      setMode("setlist");
      return;
    }
    renderPerformancePads();
  });

  applyPerformancePadAppSettings();
}
