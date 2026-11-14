/**
 * automationPanel.ts — MIDI & Automation mapping modal.
 *
 * Opened from the MIDI button in the footer bar. Contains two tabs:
 *  - Mappings: automation slot configuration (MIDI/keyboard mapping), and the
 *    switch for sending the preset name to the controller's display
 *  - MIDI Log: real-time diagnostic log of incoming MIDI events
 */

import { postMessage } from "./bridge.js";
import { updateAppSetting } from "./appSettingsStore.js";
import { getNodeDisplayName } from "./signalPath/nodeLabels.js";
import { getActivePresetForRender, uiState } from "./state.js";
import type { AutomationSlot, AutomationRegistryEntry } from "./types.js";
import { buildTargetOptions, deriveLabel } from "./automationTargets.js";
import { getXMarkSvg } from "./iconAssets.js";
import {
  MIDI_MODE_OPTIONS,
  describeMidiMap,
  formatMidiChannel,
  isBypassAddress,
  nodeIdFromSelector,
  withCurrentTargetOption,
} from "./midiMapping.js";
import { escapeHtml } from "./utils.js";
import { syncSetlistMidiOutControls, wireSetlistMidiOutControls } from "./midiSetlistOut.js";

let modalInitialized = false;
let pendingLearnSlotId: string | null = null;
let pendingKeyLearnSlotId: string | null = null;
let editingSlotId: string | null = null;
let midiLogEnabled = false;
const midiLogEntries: { time: string; type: string; data: string }[] = [];
const MAX_LOG_ENTRIES = 500;

/**
 * App-settings key for sending the active preset's name to the MIDI controller's
 * display as SysEx (the engine's ControllerDisplayFeed). Absent means on.
 */
const CONTROLLER_DISPLAY_PRESET_NAME_SETTING = "midi.controllerDisplay.presetName";

/**
 * `event.key` for the spacebar. Space is never claimed as a mapping: it is the
 * transport key in every DAW, and the plugin has no business swallowing it.
 */
const SPACEBAR_KEY = " ";

/**
 * Keys are compared case-insensitively for printable characters, and verbatim
 * for named keys ("ArrowUp", "Escape", …) where case is meaningful.
 */
function normalizeMappedKey(key: string): string {
  return key.length === 1 ? key.toLowerCase() : key;
}

export interface AutomationState {
  slots: AutomationSlot[];
  registry: AutomationRegistryEntry[];
  maxCustomSlots: number;
  /** How many per-preset MIDI mappings one preset may hold. */
  maxPresetSlots?: number;
  /** The slot the engine's key learn is waiting on (armKeyLearn), or "" when none is. */
  keyLearnSlotId?: string;
}

let maxPresetSlots = 16;

export function applyAutomationState(next: Partial<AutomationState>): void {
  uiState.automation = {
    slots: next.slots ?? uiState.automation?.slots ?? [],
    registry: next.registry ?? uiState.automation?.registry ?? [],
    maxCustomSlots: next.maxCustomSlots ?? uiState.automation?.maxCustomSlots ?? 16,
  };
  if (typeof next.maxPresetSlots === "number") {
    maxPresetSlots = next.maxPresetSlots;
  }
  // The engine runs key learn, and says which slot it is waiting on with every slot table.
  if (typeof next.keyLearnSlotId === "string") {
    pendingKeyLearnSlotId = next.keyLearnSlotId || null;
  }
  renderAutomationPanel();
  renderKeyboardPanel();
}

export function getMaxPresetSlots(): number {
  return maxPresetSlots;
}

/** A preset's name for display, or its id when the library has not loaded it. */
export function presetDisplayName(presetId: string): string {
  return uiState.presetCache.get(presetId)?.name
    ?? uiState.presets.find((preset) => preset.id === presetId)?.name
    ?? presetId;
}

// ── MIDI learn ────────────────────────────────────────────────────────────
// One slot listens at a time, whether it was armed here or from a control's
// right-click menu (midiLearnMenu.ts), so the panel always shows the truth.

/** The event a learn captured, as the engine reports it in `midiLearnCapture`. */
export interface MidiLearnCapture {
  slotId: string;
  eventType: number;
  channel: number;
  controller: number;
}

/**
 * Told of every change to the armed learn: `armedSlotId` is the slot now listening
 * (null when none is), and `capture` is set when the change is a learn completing.
 */
export type MidiLearnListener = (armedSlotId: string | null, capture: MidiLearnCapture | null) => void;

const midiLearnListeners = new Set<MidiLearnListener>();

export function onMidiLearnChange(listener: MidiLearnListener): void {
  midiLearnListeners.add(listener);
}

function notifyMidiLearnChange(capture: MidiLearnCapture | null): void {
  midiLearnListeners.forEach((listener) => listener(pendingLearnSlotId, capture));
}

export function getArmedMidiLearnSlotId(): string | null {
  return pendingLearnSlotId;
}

/** Listen for the next MIDI event on a slot, replacing any learn already armed. */
export function armMidiLearn(slotId: string, mode?: number): void {
  pendingLearnSlotId = slotId;
  postMessage({ type: "armMidiLearn", slotId, ...(mode === undefined ? {} : { mode }) });
  renderAutomationPanel();
  notifyMidiLearnChange(null);
}

export function cancelMidiLearn(): void {
  if (!pendingLearnSlotId) return;
  pendingLearnSlotId = null;
  postMessage({ type: "cancelMidiLearn" });
  renderAutomationPanel();
  notifyMidiLearnChange(null);
}

export function handleMidiLearnCapture(capture: MidiLearnCapture): void {
  pendingLearnSlotId = null;
  renderAutomationPanel();
  notifyMidiLearnChange(capture);
}

export function initializeAutomationPanel(): void {
  if (modalInitialized) return;
  modalInitialized = true;

  wireModal();
  wireTabs();
  wireControllerDisplayToggle();
  wireSetlistMidiOutControls();
  renderAutomationPanel();
  renderKeyboardPanel();
  renderMidiLog();
}

// ── Modal open/close ─────────────────────────────────────────────────────

function wireModal(): void {
  const footerBtn = document.getElementById("footer-midi-btn");
  const modal = document.getElementById("midi-modal");
  const closeBtn = document.getElementById("midi-close-btn");

  if (footerBtn) {
    footerBtn.addEventListener("click", () => {
      if (modal?.style.display === "flex") {
        closeMidiModal();
      } else {
        openMidiModal();
      }
    });
  }

  if (closeBtn) {
    closeBtn.addEventListener("click", closeMidiModal);
  }

  if (modal) {
    modal.addEventListener("mousedown", (event) => {
      if (event.target === modal) closeMidiModal();
    });
  }

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && modal?.style.display === "flex") {
      closeMidiModal();
    }
  });
}

function openMidiModal(): void {
  const modal = document.getElementById("midi-modal");
  if (!modal) return;
  modal.style.display = "flex";
  // Sync backend MIDI-log forwarding with the current toggle state.
  postMessage({ type: "setMidiLogEnabled", enabled: midiLogEnabled });
  // Settings can change from another instance while the panel is closed.
  syncControllerDisplayToggle();
  syncSetlistMidiOutControls();
  requestAutomationState();
  renderAutomationPanel();
  renderKeyboardPanel();
  // Steal focus so keyboard events aren't consumed by buttons/inputs
  if (document.activeElement instanceof HTMLElement) {
    document.activeElement.blur();
  }
  modal.focus();
}

function closeMidiModal(): void {
  const modal = document.getElementById("midi-modal");
  if (!modal) return;
  modal.style.display = "none";
  // Stop backend MIDI-log forwarding while the panel is closed.
  postMessage({ type: "setMidiLogEnabled", enabled: false });
  cancelMidiLearn();
  armKeyLearn(null);
  editingSlotId = null;
  // Nothing to redraw: both lists render only while the modal is open, and again when it opens.
}

// ── Tabs ──────────────────────────────────────────────────────────────────

function wireTabs(): void {
  const tabBtns = document.querySelectorAll<HTMLButtonElement>(".midi-tab-btn");
  tabBtns.forEach((btn) => {
    btn.addEventListener("click", () => {
      const targetTab = btn.dataset.midiTab;
      tabBtns.forEach((b) => b.classList.toggle("active", b === btn));

      document.querySelectorAll<HTMLDivElement>(".midi-tab-panel").forEach((panel) => {
        panel.classList.toggle("active", panel.id === `midi-tab-${targetTab}`);
      });

      // Steal focus so keyboard events aren't consumed by the tab button
      blurActive();
    });
  });

  // MIDI log controls
  const logToggle = document.getElementById("midi-log-enabled");
  if (logToggle) {
    logToggle.addEventListener("change", () => {
      midiLogEnabled = (logToggle as HTMLInputElement).checked;
      postMessage({ type: "setMidiLogEnabled", enabled: midiLogEnabled });
      renderMidiLog();
    });
  }

  const clearBtn = document.getElementById("midi-log-clear");
  if (clearBtn) {
    clearBtn.addEventListener("click", () => {
      midiLogEntries.length = 0;
      renderMidiLog();
    });
  }
}

// ── Controller display ────────────────────────────────────────────────────

function getControllerDisplayToggle(): HTMLInputElement | null {
  return document.getElementById("midi-controller-display-preset-name") as HTMLInputElement | null;
}

function syncControllerDisplayToggle(): void {
  const toggle = getControllerDisplayToggle();
  if (toggle) {
    toggle.checked = uiState.appSettings?.[CONTROLLER_DISPLAY_PRESET_NAME_SETTING] !== false;
  }
}

function wireControllerDisplayToggle(): void {
  const toggle = getControllerDisplayToggle();
  if (!toggle) return;

  syncControllerDisplayToggle();
  toggle.addEventListener("change", () => {
    updateAppSetting(CONTROLLER_DISPLAY_PRESET_NAME_SETTING, toggle.checked);
    // blurActive() only releases buttons; a focused checkbox would hold keys the
    // keyboard mappings are listening for.
    toggle.blur();
  });
}

// ── State requests ────────────────────────────────────────────────────────

/**
 * Ask the engine for the slot table and parameter registry. main.ts sends this at
 * startup once IPlugReceiveData is registered: sent from initializeAutomationPanel,
 * which runs earlier, the reply was lost, and the registry stayed empty (and the
 * right-click MIDI Learn menu absent) until the MIDI panel was first opened.
 */
export function requestAutomationState(): void {
  postMessage({ type: "getAutomation" });
}

// ── Mappings rendering ────────────────────────────────────────────────────

function isTextEntryElement(element: HTMLElement | null): boolean {
  if (!element) return false;
  if (element.isContentEditable) return true;
  return Boolean(element.closest("input, textarea, select, [contenteditable=''], [contenteditable='true'], [role='textbox']"));
}

function renderAutomationPanel(): void {
  const container = document.getElementById("automation-slots-list");
  if (!container || document.getElementById("midi-modal")?.style.display !== "flex") return;

  const state = uiState.automation;
  if (!state) {
    container.innerHTML = "<p>Loading automation…</p>";
    return;
  }

  let html = "";

  // Default slots
  html += '<div class="automation-section"><h3>Default Slots</h3>';
  for (const slot of state.slots.filter((s) => s.isDefault)) {
    html += renderSlotRow(slot, state.registry);
  }
  html += "</div>";

  // Custom slots
  const customSlots = state.slots.filter((s) => !s.isDefault && !s.presetId);
  html += `<div class="automation-section"><h3>Custom Slots (${customSlots.length}/${state.maxCustomSlots})</h3>`;
  for (const slot of customSlots) {
    html += renderSlotRow(slot, state.registry);
    if (editingSlotId === slot.slotId) {
      html += renderTargetEditor(slot, state.registry);
    }
  }

  if (customSlots.length < state.maxCustomSlots) {
    html += `<button class="automation-add-slot" id="automation-add-slot">+ Add Custom Slot</button>`;
  }
  html += "</div>";

  // Per-preset MIDI mappings, made with "MIDI Learn for this preset…" on a control
  const presetSlots = state.slots.filter((s) => s.presetId);
  if (presetSlots.length > 0) {
    html += '<div class="automation-section"><h3>Preset Mappings</h3>';
    for (const slot of presetSlots) {
      html += renderSlotRow(slot, state.registry);
    }
    html += "</div>";
  }

  container.innerHTML = html;
  wireSlotEvents(container);
}

function renderSlotRow(slot: AutomationSlot, registry: AutomationRegistryEntry[]): string {
  const entry = registry.find((e) => e.address === slot.address);
  const rangeText = entry ? `(${entry.min}..${entry.max}${entry.unit ? " " + entry.unit : ""})` : "";
  const nodeText = describeSelectedNode(slot);
  const addressText = slot.presetId
    ? `${presetDisplayName(slot.presetId)} · ${slot.address}${nodeText ? ` · ${nodeText}` : ""}`
    : slot.address;
  const isLearn = pendingLearnSlotId === slot.slotId;
  const showTestButton = Boolean(entry?.isTrigger) || !slot.isDefault;

  // The mode has a picker of its own, so the text leaves it out.
  const midiText = slot.midiMap
    ? describeMidiMap({ eventType: slot.midiMap.eventType, channel: slot.midiMap.channel, controller: slot.midiMap.controller })
    : "—";
  const modePicker = slot.midiMap ? renderMidiModePicker(slot) : "";

  let keyText = "—";
  if (slot.keyMap && slot.keyMap.length > 0) {
    keyText = slot.keyMap.map((k) => {
      const mode = k.mode === 0 ? "trig" : `=${k.value.toFixed(2)}`;
      return `${k.key}(${mode})`;
    }).join(", ");
  }

  return `
    <div class="automation-slot-row" data-slot-id="${escapeHtml(slot.slotId)}">
      <span class="automation-slot-label">${escapeHtml(slot.label)}</span>
      <span class="automation-slot-address" title="${escapeHtml(addressText)}">${escapeHtml(addressText)} ${escapeHtml(rangeText)}</span>
      <span class="automation-slot-midi">${escapeHtml(midiText)}${modePicker}${slot.midiMap ? ` <button class="automation-clear-btn automation-clear-midi" data-slot-id="${escapeHtml(slot.slotId)}" title="Clear MIDI mapping">${getXMarkSvg()}</button>` : ""}</span>
      <span class="automation-slot-key">${escapeHtml(keyText)}${(slot.keyMap && slot.keyMap.length > 0) ? ` <button class="automation-clear-btn automation-clear-key" data-slot-id="${escapeHtml(slot.slotId)}" title="Clear keyboard mapping">${getXMarkSvg()}</button>` : ""}</span>
      <div>
        <button class="automation-learn-btn ${isLearn ? "active" : ""}" data-slot-id="${escapeHtml(slot.slotId)}">
          ${isLearn ? "Listening…" : "Learn"}
        </button>
        ${showTestButton ? `<button class="automation-test-btn" data-slot-id="${escapeHtml(slot.slotId)}" title="Fire this mapping now">Test</button>` : ""}
        ${!slot.isDefault && !slot.presetId ? `<button class="automation-target-btn" data-slot-id="${escapeHtml(slot.slotId)}" title="Edit target parameter">${editingSlotId === slot.slotId ? "Close" : "Target"}</button>` : ""}
        ${!slot.isDefault ? `<button class="automation-remove-btn" data-slot-id="${escapeHtml(slot.slotId)}">${getXMarkSvg()}</button>` : ""}
      </div>
    </div>
  `;
}

function wireSlotEvents(container: HTMLElement): void {
  container.querySelectorAll<HTMLButtonElement>(".automation-learn-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const slotId = btn.dataset.slotId || "";
      if (pendingLearnSlotId === slotId) {
        cancelMidiLearn();
      } else {
        // Learning again changes the control, not how the mapping acts: the mode chosen stays.
        armMidiLearn(slotId, uiState.automation?.slots.find((slot) => slot.slotId === slotId)?.midiMap?.mode);
      }
      blurActive();
    });
  });

  container.querySelectorAll<HTMLButtonElement>(".automation-target-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const slotId = btn.dataset.slotId || "";
      editingSlotId = editingSlotId === slotId ? null : slotId;
      renderAutomationPanel();
      blurActive();
    });
  });

  container.querySelectorAll<HTMLButtonElement>(".automation-test-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const slotId = btn.dataset.slotId || "";
      const slot = uiState.automation?.slots.find((s) => s.slotId === slotId);
      const entry = uiState.automation?.registry.find((e) => e.address === (slot?.address ?? ""));

      let value = 1.0;
      if (slot && (isBypassAddress(slot.address) || slot.midiMap?.mode === 2)) {
        value = slot.value < 0.5 ? 1.0 : 0.0;
      } else if (!entry?.isTrigger && slot) {
        value = slot.value < 0.99 ? 1.0 : 0.0;
      }

      // An effect's on/off switches from the state the effect is in, which the engine reads:
      // the slot's value is only what it last set, and the editor or a preset load moves it.
      const toggle = slot ? isBypassAddress(slot.address) : false;
      postMessage({ type: "setAutomationValue", slotId, value, source: "ui", ...(toggle ? { toggle: true } : {}) });
      blurActive();
    });
  });

  container.querySelectorAll<HTMLButtonElement>(".automation-remove-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const slotId = btn.dataset.slotId || "";
      postMessage({ type: "removeAutomationSlot", slotId });
      blurActive();
    });
  });

  container.querySelectorAll<HTMLButtonElement>(".automation-clear-midi").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const slotId = btn.dataset.slotId || "";
      postMessage({ type: "setAutomationSlot", slotId, midiMap: null });
      blurActive();
    });
  });

  container.querySelectorAll<HTMLButtonElement>(".automation-clear-key").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const slotId = btn.dataset.slotId || "";
      postMessage({ type: "setAutomationSlot", slotId, keyMap: null });
      blurActive();
    });
  });

  container.querySelectorAll<HTMLSelectElement>(".automation-midi-mode").forEach((select) => {
    select.addEventListener("change", () => {
      const slotId = select.dataset.slotId || "";
      const slot = uiState.automation?.slots.find((s) => s.slotId === slotId);
      if (!slot?.midiMap) {
        return;
      }
      postMessage({ type: "setAutomationSlot", slotId, midiMap: { ...slot.midiMap, mode: Number(select.value) } });
      blurActive();
    });
  });

  const addBtn = container.querySelector<HTMLButtonElement>("#automation-add-slot");
  if (addBtn) {
    addBtn.addEventListener("click", () => {
      const slotNum = Date.now() % 100000;
      const slotId = `custom.${slotNum}`;
      postMessage({ type: "setAutomationSlot", slotId, label: "New Slot", address: "" });
      blurActive();
    });
  }

  wireTargetEditor(container);
}

// ── Target picker (custom slot address selector) ───────────────────────────

/**
 * The node a per-preset mapping drives, when it names one (`id:<nodeId>`): the name the node
 * has in the active preset, or its id while another preset is playing.
 */
function describeSelectedNode(slot: AutomationSlot): string {
  const nodeId = nodeIdFromSelector(slot.nodeSelector);
  if (!nodeId) {
    return "";
  }
  const preset = slot.presetId === uiState.activePresetId ? getActivePresetForRender() : null;
  const graphs = preset ? [preset.graph, ...(preset.scenes ?? []).map((scene) => scene.graph)] : [];
  const node = graphs.map((graph) => graph?.nodes.find((candidate) => candidate.id === nodeId)).find(Boolean);
  return node ? getNodeDisplayName(node) : `node ${nodeId}`;
}

/** The mode picker beside a MIDI mapping. */
function renderMidiModePicker(slot: AutomationSlot): string {
  const current = slot.midiMap?.mode ?? 0;
  const options = MIDI_MODE_OPTIONS.map((option) =>
    `<option value="${option.mode}" title="${escapeHtml(option.title)}"${option.mode === current ? " selected" : ""}>${escapeHtml(option.label)}</option>`).join("");
  return ` <select class="automation-midi-mode" data-slot-id="${escapeHtml(slot.slotId)}" aria-label="MIDI mode for ${escapeHtml(slot.label)}" title="How the MIDI control drives the target">${options}</select>`;
}

function renderTargetEditor(slot: AutomationSlot, registry: AutomationRegistryEntry[]): string {
  const derivedLabel = deriveLabel(slot.address, registry);
  const options = withCurrentTargetOption(buildTargetOptions(registry), slot.address, derivedLabel);

  const groups: string[] = [];
  const seen = new Set<string>();
  for (const o of options) {
    if (!seen.has(o.group)) {
      seen.add(o.group);
      groups.push(o.group);
    }
  }

  let optgroups = "";
  for (const g of groups) {
    let inner = "";
    for (const o of options) {
      if (o.group !== g) continue;
      const sel = o.address === slot.address ? " selected" : "";
      inner += `<option value="${escapeHtml(o.address)}"${sel}>${escapeHtml(o.label)}</option>`;
    }
    optgroups += `<optgroup label="${escapeHtml(g)}">${inner}</optgroup>`;
  }

  const labelValue = slot.label || derivedLabel;

  return `
    <div class="automation-target-editor" data-slot-id="${escapeHtml(slot.slotId)}">
      <label class="automation-target-field">
        <span>Label</span>
        <input type="text" class="automation-target-label-input" value="${escapeHtml(labelValue)}" />
      </label>
      <label class="automation-target-field">
        <span>Target parameter</span>
        <select class="automation-target-select">${optgroups}</select>
      </label>
      <div class="automation-target-actions">
        <button class="automation-target-save" data-slot-id="${escapeHtml(slot.slotId)}">Apply</button>
        <button class="automation-target-cancel" data-slot-id="${escapeHtml(slot.slotId)}">Cancel</button>
      </div>
    </div>
  `;
}

function wireTargetEditor(container: HTMLElement): void {
  container.querySelectorAll<HTMLButtonElement>(".automation-target-save").forEach((btn) => {
    btn.addEventListener("click", () => {
      const slotId = btn.dataset.slotId || "";
      const editor = container.querySelector<HTMLElement>(`.automation-target-editor[data-slot-id="${cssEscape(slotId)}"]`);
      if (!editor) return;
      const labelInput = editor.querySelector<HTMLInputElement>(".automation-target-label-input");
      const select = editor.querySelector<HTMLSelectElement>(".automation-target-select");
      const label = labelInput?.value.trim() || deriveLabel(select?.value || "", uiState.automation?.registry ?? []);
      const address = select?.value || "";
      postMessage({ type: "setAutomationSlot", slotId, label, address });
      editingSlotId = null;
      blurActive();
    });
  });

  container.querySelectorAll<HTMLButtonElement>(".automation-target-cancel").forEach((btn) => {
    btn.addEventListener("click", () => {
      editingSlotId = null;
      renderAutomationPanel();
      blurActive();
    });
  });
}

function cssEscape(s: string): string {
  if (window.CSS && typeof window.CSS.escape === "function") return window.CSS.escape(s);
  return s.replace(/["\\]/g, "\\$&");
}

function blurActive(): void {
  if (document.activeElement instanceof HTMLElement && document.activeElement.tagName === "BUTTON") {
    document.activeElement.blur();
  }
}

// ── Keyboard mapping tab ───────────────────────────────────────────────────

/** Space would otherwise render as an invisible blank in the mapping list. */
function describeMappedKey(key: string): string {
  return key === SPACEBAR_KEY ? "Space" : key;
}

function renderKeyboardPanel(): void {
  const container = document.getElementById("keyboard-slots-list");
  if (!container || document.getElementById("midi-modal")?.style.display !== "flex") return;

  const state = uiState.automation;
  if (!state) {
    container.innerHTML = "<p>Loading automation…</p>";
    return;
  }

  // Space can no longer be mapped, but an older build could have stored one.
  // Such a mapping never fires, so flag it rather than leaving it silently dead.
  const hasStaleSpaceMapping = state.slots.some(
    (slot) => slot.keyMap?.some((k) => normalizeMappedKey(k.key) === SPACEBAR_KEY));

  let html = hasStaleSpaceMapping
    ? `<p class="midi-help">A slot is mapped to Space. The spacebar is reserved for your DAW's transport, so that mapping never fires — use Clear to remove it.</p>`
    : "";

  // Keys are global, so per-preset MIDI mappings take none.
  for (const slot of state.slots.filter((s) => !s.presetId)) {
    const isKeyLearn = pendingKeyLearnSlotId === slot.slotId;

    let keyText = "—";
    if (slot.keyMap && slot.keyMap.length > 0) {
      keyText = slot.keyMap.map((k) => {
        const mode = k.mode === 0 ? "trig" : `=${k.value.toFixed(2)}`;
        return `${describeMappedKey(k.key)}(${mode})`;
      }).join(", ");
    }

    html += `
      <div class="automation-slot-row keyboard-slot-row" data-slot-id="${escapeHtml(slot.slotId)}">
        <span class="automation-slot-label">${escapeHtml(slot.label)}</span>
        <span class="automation-slot-key-display">${escapeHtml(keyText)}</span>
        <div class="keyboard-slot-actions">
          <button class="automation-key-learn-btn ${isKeyLearn ? "active" : ""}" data-slot-id="${escapeHtml(slot.slotId)}">
            ${isKeyLearn ? "Press a key…" : "Learn"}
          </button>
          ${(slot.keyMap && slot.keyMap.length > 0) ? `<button class="automation-key-clear-btn" data-slot-id="${escapeHtml(slot.slotId)}">Clear</button>` : ""}
        </div>
      </div>
    `;
  }

  container.innerHTML = html;
  wireKeyboardPanelEvents(container);
}

function wireKeyboardPanelEvents(container: HTMLElement): void {
  container.querySelectorAll<HTMLButtonElement>(".automation-key-learn-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const slotId = btn.dataset.slotId || "";
      armKeyLearn(pendingKeyLearnSlotId === slotId ? null : slotId);
      blurActive();
    });
  });

  container.querySelectorAll<HTMLButtonElement>(".automation-key-clear-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const slotId = btn.dataset.slotId || "";
      postMessage({ type: "setAutomationSlot", slotId, keyMap: null });
      blurActive();
    });
  });
}

// ── MIDI Log ──────────────────────────────────────────────────────────────

export function handleMidiLogEntry(entry: { type: string; channel: number; data1: number; data2: number }): void {
  if (!midiLogEnabled) return;

  const now = new Date();
  const time = `${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}:${String(now.getSeconds()).padStart(2, "0")}.${String(now.getMilliseconds()).padStart(3, "0")}`;
  const data = `ch=${formatMidiChannel(entry.channel)} d1=${entry.data1} d2=${entry.data2}`;

  midiLogEntries.push({ time, type: entry.type, data });
  if (midiLogEntries.length > MAX_LOG_ENTRIES) {
    midiLogEntries.shift();
  }

  renderMidiLog();
}

function renderMidiLog(): void {
  const output = document.getElementById("midi-log-output");
  if (!output) return;

  if (!midiLogEnabled) {
    output.innerHTML = '<p class="midi-log-empty">MIDI logging is disabled. Enable it to see incoming MIDI events.</p>';
    return;
  }

  if (midiLogEntries.length === 0) {
    output.innerHTML = '<p class="midi-log-empty">No MIDI events received yet.</p>';
    return;
  }

  // Render last 200 entries in reverse (newest first)
  const entries = midiLogEntries.slice(-200).reverse();
  let html = "";
  for (const entry of entries) {
    html += `<div class="midi-log-entry"><span class="midi-log-time">${entry.time}</span><span class="midi-log-type">${escapeHtml(entry.type)}</span><span class="midi-log-data">${escapeHtml(entry.data)}</span></div>`;
  }
  output.innerHTML = html;
}

// ── Keyboard handling ─────────────────────────────────────────────────────
// The engine runs the keyboard mappings (automationKey): which slot a key fires, the value it
// sets, and what a key learn stores, the same for every UI. This listener only decides which
// keys to pass on. A text field keeps its keys, Space is the DAW's transport and a chord is a
// shortcut, so none of those go; a key no slot maps goes nowhere either. Mapped keys fire
// wherever the UI has focus; a key learn is confined to the MIDI panel, the one place a
// keystroke should be swallowed to assign it rather than acted on.

/** Asks the engine to map the next key to `slotId`, or with null to stop waiting for one. */
function armKeyLearn(slotId: string | null): void {
  if (!slotId && !pendingKeyLearnSlotId) return;
  pendingKeyLearnSlotId = slotId;
  postMessage({ type: "armKeyLearn", slotId: slotId ?? "" });
  renderKeyboardPanel();
}

/** Whether any slot maps `key`, compared as the engine compares it, so the key can be held back from the page. */
function isMappedKey(slots: readonly AutomationSlot[], key: string): boolean {
  const pressed = normalizeMappedKey(key);
  return slots.some((slot) => slot.keyMap?.some((mapping) => normalizeMappedKey(mapping.key) === pressed) ?? false);
}

document.addEventListener("keydown", (event: KeyboardEvent) => {
  const active = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  if (isTextEntryElement(active)) return;

  const modal = document.getElementById("midi-modal");
  const isMidiPanelOpen = modal?.style.display === "flex";

  // A key learn takes every key, so none of them scroll or re-trigger the Learn button. The
  // engine maps the key, backs out on Escape, and waits through Space and a bare modifier.
  if (isMidiPanelOpen && pendingKeyLearnSlotId) {
    postMessage({ type: "automationKey", key: event.key, repeat: event.repeat });
    event.preventDefault();
    return;
  }

  const state = uiState.automation;
  if (!state || event.key === SPACEBAR_KEY || event.ctrlKey || event.metaKey || event.altKey) return;
  if (!isMappedKey(state.slots, event.key)) return;

  postMessage({ type: "automationKey", key: event.key, repeat: event.repeat });
  event.preventDefault();
}, true);
