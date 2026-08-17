/**
 * automationPanel.ts — MIDI & Automation mapping modal.
 *
 * Opened from the MIDI button in the footer bar. Contains two tabs:
 *  - Mappings: automation slot configuration (MIDI/keyboard mapping)
 *  - MIDI Log: real-time diagnostic log of incoming MIDI events
 */

import { postMessage, setAppSetting } from "./bridge.js";
import { uiState } from "./state.js";
import type { AutomationSlot, AutomationRegistryEntry } from "./types.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { getXMarkSvg } from "./iconAssets.js";

let modalInitialized = false;
let pendingLearnSlotId: string | null = null;
let pendingKeyLearnSlotId: string | null = null;
let editingSlotId: string | null = null;
let midiLogEnabled = false;
const midiLogEntries: { time: string; type: string; data: string }[] = [];
const MAX_LOG_ENTRIES = 500;

/** `event.key` for the spacebar. */
const SPACEBAR_KEY = " ";
const CAPTURE_SPACEBAR_SETTING = "automation.captureSpacebar";

/**
 * Keys are compared case-insensitively for printable characters, and verbatim
 * for named keys ("ArrowUp", "Escape", …) where case is meaningful.
 */
function normalizeMappedKey(key: string): string {
  return key.length === 1 ? key.toLowerCase() : key;
}

/**
 * Whether a Space mapping is allowed to fire. Off by default: Space is the
 * transport key in every DAW, and a plugin update that silently stopped play/stop
 * from working would be far worse than a mapping the user has to switch on.
 */
export function isSpacebarCaptureEnabled(): boolean {
  return uiState.appSettings?.[CAPTURE_SPACEBAR_SETTING] === true;
}

function setSpacebarCaptureEnabled(enabled: boolean): void {
  uiState.appSettings[CAPTURE_SPACEBAR_SETTING] = enabled;
  setAppSetting(CAPTURE_SPACEBAR_SETTING, enabled);
}

export interface AutomationState {
  slots: AutomationSlot[];
  registry: AutomationRegistryEntry[];
  maxCustomSlots: number;
}

export function applyAutomationState(next: Partial<AutomationState>): void {
  uiState.automation = {
    slots: next.slots ?? uiState.automation?.slots ?? [],
    registry: next.registry ?? uiState.automation?.registry ?? [],
    maxCustomSlots: next.maxCustomSlots ?? uiState.automation?.maxCustomSlots ?? 16,
  };
  renderAutomationPanel();
  renderKeyboardPanel();
}

export function handleMidiLearnCapture(slotId: string): void {
  pendingLearnSlotId = null;
  renderAutomationPanel();
}

export function initializeAutomationPanel(): void {
  if (modalInitialized) return;
  modalInitialized = true;

  wireModal();
  wireTabs();
  requestAutomationState();
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
  requestAutomationState();
  const spacebarToggle = document.getElementById("automation-capture-spacebar") as HTMLInputElement | null;
  if (spacebarToggle) {
    spacebarToggle.checked = isSpacebarCaptureEnabled();
  }
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
  if (pendingLearnSlotId) {
    pendingLearnSlotId = null;
    postMessage({ type: "cancelMidiLearn" });
  }
  pendingKeyLearnSlotId = null;
  editingSlotId = null;
  renderAutomationPanel();
  renderKeyboardPanel();
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

  const spacebarToggle = document.getElementById("automation-capture-spacebar") as HTMLInputElement | null;
  if (spacebarToggle) {
    spacebarToggle.addEventListener("change", () => {
      setSpacebarCaptureEnabled(spacebarToggle.checked);
      renderKeyboardPanel(); // refresh the "mapped to Space but inert" hint
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

// ── State requests ────────────────────────────────────────────────────────

function requestAutomationState(): void {
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
  if (!container) return;

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
  const customSlots = state.slots.filter((s) => !s.isDefault);
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

  container.innerHTML = html;
  wireSlotEvents(container);
}

/**
 * MIDI channels are stored 0-15 on the wire (and -1 for "any"), but musicians
 * and hardware label them 1-16 — always display the 1-based number.
 */
function formatMidiChannel(channel: number): string {
  return channel < 0 ? "any" : String(channel + 1);
}

function renderSlotRow(slot: AutomationSlot, registry: AutomationRegistryEntry[]): string {
  const entry = registry.find((e) => e.address === slot.address);
  const rangeText = entry ? `(${entry.min}..${entry.max}${entry.unit ? " " + entry.unit : ""})` : "";
  const isLearn = pendingLearnSlotId === slot.slotId;
  const showTestButton = Boolean(entry?.isTrigger) || !slot.isDefault;

  let midiText = "—";
  if (slot.midiMap) {
    const eventType = ["CC", "PC", "NoteOn", "NoteOff", "PBend"][slot.midiMap.eventType] || "CC";
    const mode = ["Abs", "Rel", "Toggle", "Pickup"][slot.midiMap.mode] || "Abs";
    midiText = `${eventType} ${slot.midiMap.controller} ch${formatMidiChannel(slot.midiMap.channel)} ${mode}`;
  }

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
      <span class="automation-slot-address" title="${escapeHtml(slot.address)}">${escapeHtml(slot.address)} ${escapeHtml(rangeText)}</span>
      <span class="automation-slot-midi">${escapeHtml(midiText)}${slot.midiMap ? ` <button class="automation-clear-btn automation-clear-midi" data-slot-id="${escapeHtml(slot.slotId)}" title="Clear MIDI mapping">${getXMarkSvg()}</button>` : ""}</span>
      <span class="automation-slot-key">${escapeHtml(keyText)}${(slot.keyMap && slot.keyMap.length > 0) ? ` <button class="automation-clear-btn automation-clear-key" data-slot-id="${escapeHtml(slot.slotId)}" title="Clear keyboard mapping">${getXMarkSvg()}</button>` : ""}</span>
      <div>
        <button class="automation-learn-btn ${isLearn ? "active" : ""}" data-slot-id="${escapeHtml(slot.slotId)}">
          ${isLearn ? "Listening…" : "Learn"}
        </button>
        ${showTestButton ? `<button class="automation-test-btn" data-slot-id="${escapeHtml(slot.slotId)}" title="Fire this mapping now">Test</button>` : ""}
        ${!slot.isDefault ? `<button class="automation-target-btn" data-slot-id="${escapeHtml(slot.slotId)}" title="Edit target parameter">${editingSlotId === slot.slotId ? "Close" : "Target"}</button>` : ""}
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
        pendingLearnSlotId = null;
        postMessage({ type: "cancelMidiLearn" });
      } else {
        pendingLearnSlotId = slotId;
        postMessage({ type: "armMidiLearn", slotId });
      }
      renderAutomationPanel();
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

      postMessage({ type: "setAutomationValue", slotId, value, source: "ui" });
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

interface TargetOption {
  address: string;
  label: string;
  group: string;
}

function buildTargetOptions(registry: AutomationRegistryEntry[]): TargetOption[] {
  const opts: TargetOption[] = [];

  for (const e of registry) {
    let group = "Other";
    if (e.address.startsWith("global.")) group = "Global";
    else if (e.address.startsWith("setlist.")) group = "Setlist";
    opts.push({ address: e.address, label: e.label, group });
  }

  const effects = EffectTypeRegistry.getAll()
    .filter((t) => t.catalogHidden !== true && t.parameters.length > 0);

  effects.sort((a, b) => {
    const c = (a.category || "").localeCompare(b.category || "");
    return c !== 0 ? c : (a.displayName || a.type).localeCompare(b.displayName || b.type);
  });

  for (const eff of effects) {
    const group = eff.category || "Effects";
    const effLabel = eff.displayName || eff.type;
    opts.push({ address: `node.${eff.type}.bypassed`, label: `${effLabel}: Bypass`, group });
    for (const p of eff.parameters) {
      const address = `node.${eff.type}.${p.key}`;
      const paramLabel = p.name || p.key;
      opts.push({ address, label: `${effLabel}: ${paramLabel}`, group });
    }
  }

  return opts;
}

function deriveLabel(address: string, registry: AutomationRegistryEntry[]): string {
  const entry = registry.find((e) => e.address === address);
  if (entry) return entry.label;

  const nodeMatch = address.match(/^node\.([^.]+)\.(.+)$/);
  if (nodeMatch) {
    const eff = EffectTypeRegistry.get(nodeMatch[1]);
    const effName = eff?.displayName || nodeMatch[1];
    if (nodeMatch[2] === "bypassed" || nodeMatch[2] === "bypass" || nodeMatch[2] === "enabled") {
      return `${effName}: Bypass`;
    }
    const param = eff?.parameters.find((p) => p.key === nodeMatch[2]);
    const paramName = param?.name || nodeMatch[2];
    return `${effName}: ${paramName}`;
  }
  return "Custom";
}

function isBypassAddress(address: string): boolean {
  return /^node\.[^.]+\.(bypassed|bypass|enabled)$/.test(address);
}

function renderTargetEditor(slot: AutomationSlot, registry: AutomationRegistryEntry[]): string {
  const options = buildTargetOptions(registry);

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

  const labelValue = slot.label || deriveLabel(slot.address, registry);

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

function escapeHtml(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

// ── Keyboard mapping tab ───────────────────────────────────────────────────

/** Space would otherwise render as an invisible blank in the mapping list. */
function describeMappedKey(key: string): string {
  return key === SPACEBAR_KEY ? "Space" : key;
}

function renderKeyboardPanel(): void {
  const container = document.getElementById("keyboard-slots-list");
  if (!container) return;

  const state = uiState.automation;
  if (!state) {
    container.innerHTML = "<p>Loading automation…</p>";
    return;
  }

  // A Space mapping does nothing while capture is off, and the failure is silent.
  // Say so rather than letting the user conclude the mapping is broken.
  const hasInertSpaceMapping = !isSpacebarCaptureEnabled()
    && state.slots.some((slot) => slot.keyMap?.some((k) => normalizeMappedKey(k.key) === SPACEBAR_KEY));

  let html = hasInertSpaceMapping
    ? `<p class="midi-help">A slot is mapped to Space, which is currently passed through to the host. Enable "Capture spacebar" above for it to fire.</p>`
    : "";

  for (const slot of state.slots) {
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
      pendingKeyLearnSlotId = pendingKeyLearnSlotId === slotId ? null : slotId;
      renderKeyboardPanel();
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
// Mapped keys fire wherever the UI has focus. Key *learn* is confined to the
// MIDI panel, since that is the only place a keystroke should be swallowed to
// assign it rather than acted on.

document.addEventListener("keydown", (event: KeyboardEvent) => {
  const active = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  if (isTextEntryElement(active)) return;

  const modal = document.getElementById("midi-modal");
  const isMidiPanelOpen = modal?.style.display === "flex";

  // Keyboard learn capture — intercept the next key and assign it to the slot
  if (isMidiPanelOpen && pendingKeyLearnSlotId) {
    const key = normalizeMappedKey(event.key);
    const state = uiState.automation;
    if (!state) return;
    const slot = state.slots.find((s) => s.slotId === pendingKeyLearnSlotId);
    if (!slot) {
      pendingKeyLearnSlotId = null;
      return;
    }

    // Build new keyMap array: replace any existing mapping for this key, add new entry.
    // Default to Trigger mode (mode=0) for trigger addresses, SetValue (mode=1) for continuous.
    const isTrigger = slot.address.includes("bankUp")
      || slot.address.includes("bankDown")
      || /setlist\.preset\d+$/.test(slot.address)
      || /scene\.select\d+$/.test(slot.address);
    const mode = isTrigger ? 0 : 1;
    const value = isTrigger ? 1.0 : 0.5;
    const newKeyMap = (slot.keyMap ?? []).filter((k) => k.key !== key);
    newKeyMap.push({ key, mode, value });

    postMessage({ type: "setAutomationSlot", slotId: pendingKeyLearnSlotId, keyMap: newKeyMap });
    pendingKeyLearnSlotId = null;
    event.preventDefault();
    return;
  }

  const state = uiState.automation;
  if (!state) return;

  const pressedKey = normalizeMappedKey(event.key);

  // Space is the transport key in every DAW, so claiming it is opt-in. Left off,
  // a Space mapping stays configured but inert and the keystroke is passed on
  // untouched rather than being swallowed here.
  if (pressedKey === SPACEBAR_KEY && !isSpacebarCaptureEnabled()) return;

  for (const slot of state.slots) {
    if (!slot.keyMap) continue;
    for (const km of slot.keyMap) {
      if (pressedKey !== normalizeMappedKey(km.key)) continue;

      if (km.mode === 0 && event.repeat) continue;

      const value = km.mode === 0 ? 1.0 : km.value;
      postMessage({ type: "setAutomationValue", slotId: slot.slotId, value, source: "keyboard" });
      event.preventDefault();
      return;
    }
  }
}, true);
