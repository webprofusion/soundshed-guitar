/**
 * Right-click (a long press on a touch screen, touchLongPress.ts) → "MIDI Learn…" on the controls a MIDI mapping can drive: the input
 * and output level knobs, effect parameters — the chain's Input and Output node gains
 * included — and the setlist pads and bank controls.
 *
 * It is the MIDI panel's Learn button, reached from the control. The learn is armed
 * on the slot that already drives the control's address — a default slot, or a
 * custom one — and an effect parameter nothing drives yet gets a custom slot made
 * for it first. Either way the captured mapping lands in the slot table the panel
 * edits. `midiMapping.ts` decides which address an element drives.
 *
 * "MIDI Learn for this preset…", offered on the signal chain's controls, learns into a
 * per-preset slot instead: live only while the active preset is selected, and taking its
 * MIDI control over from a global mapping then, so one pedal can do a different job in
 * each preset. A per-preset mapping on an effect names that node (`id:<nodeId>`), so two
 * effects of one type — two amps in parallel — each answer to their own control.
 *
 * An effect's on/off button learns too, as `node.<type>.enabled`: a footswitch's 127
 * turns the effect on and 0 turns it off, or with Toggle chosen in the MIDI panel each
 * press switches it. "MIDI Learn toggle for this preset…" learns straight into Toggle, for
 * that one effect in that one preset.
 */

import { armMidiLearn, cancelMidiLearn, getArmedMidiLearnSlotId, getMaxPresetSlots, onMidiLearnChange, presetDisplayName } from "./automationPanel.js";
import type { MidiLearnCapture } from "./automationPanel.js";
import { postMessage } from "./bridge.js";
import {
  buildMidiLearnMenuItems,
  countCustomSlots,
  countPresetSlots,
  describeMidiMap,
  findPresetSlotForAddress,
  findSlotForAddress,
  isBypassAddress,
  MIDI_MODE_ABSOLUTE,
  MIDI_MODE_TOGGLE,
  nextCustomSlotId,
  nextPresetSlotId,
  nodeSelectorFor,
  resolveMidiLearnTarget,
} from "./midiMapping.js";
import type { MidiLearnTarget } from "./midiMapping.js";
import { showNotification } from "./notifications.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { getNodeDisplayName } from "./signalPath/nodeLabels.js";
import { getActivePresetForRender, uiState } from "./state.js";
import type { AutomationSlot, GraphNode } from "./types.js";
import { initializeTouchLongPress } from "./touchLongPress.js";
import { escapeHtml } from "./utils.js";

/** How far inside the window the menu's edges are kept. */
const VIEWPORT_MARGIN_PX = 4;

let initialized = false;
let menuElement: HTMLElement | null = null;
let statusElement: HTMLElement | null = null;
/** The learn this menu armed, while it waits for a MIDI event. */
let listening: { slotId: string; label: string; presetName: string | null; toggle: boolean } | null = null;

export function initializeMidiLearnMenu(): void {
  if (initialized) return;
  initialized = true;

  document.addEventListener("contextmenu", handleContextMenu);
  initializeTouchLongPress();
  // Capture phase, so a control that stops the event still closes the menu.
  document.addEventListener("pointerdown", (event) => {
    if (menuElement && !menuElement.contains(event.target as Node | null)) {
      closeMenu();
    }
  }, true);
  document.addEventListener("keydown", handleKeydown);
  document.addEventListener("scroll", closeMenu, true);
  window.addEventListener("resize", closeMenu);
  window.addEventListener("blur", closeMenu);
  onMidiLearnChange(handleMidiLearnChange);
}

function handleContextMenu(event: MouseEvent): void {
  // A right-click on the menu itself keeps it, rather than swapping in the browser's menu.
  if (menuElement && event.target instanceof Node && menuElement.contains(event.target)) {
    event.preventDefault();
    return;
  }
  closeMenu();
  const automation = uiState.automation;
  // Until the engine has sent its slots there is nothing to arm, and no way to tell
  // which slot already drives a control.
  if (!automation?.registry.length) {
    return;
  }

  const target = resolveMidiLearnTarget(event.target instanceof Element ? event.target : null, {
    registry: automation.registry,
    findNode: findPlayingNode,
    // The chain's Input and Output routing nodes are defined only here (ROUTING_NODE_EFFECTS),
    // but the engine maps their gain onto the same dB range the knob draws, so they are
    // learnable like any other node parameter.
    getEffectInfo: (type) => EffectTypeRegistry.get(type),
    getNodeName: getNodeDisplayName,
  });
  if (!target) {
    return;
  }

  event.preventDefault();
  openMenu(target, event.clientX, event.clientY);
}

/**
 * The node with this id in the preset the engine is playing. A composite being edited
 * shows its inner graph instead, and a `node.*` address cannot reach those nodes.
 */
function findPlayingNode(nodeId: string): GraphNode | undefined {
  if (uiState.compositeEditMode) {
    return undefined;
  }
  const preset = getActivePresetForRender();
  const graphs = [preset?.graph, ...(preset?.scenes ?? []).map((scene) => scene.graph)];
  for (const graph of graphs) {
    const node = graph?.nodes.find((candidate) => candidate.id === nodeId);
    if (node) {
      return node;
    }
  }
  return undefined;
}

function openMenu(target: MidiLearnTarget, x: number, y: number): void {
  const slots = uiState.automation?.slots ?? [];
  const maxCustomSlots = uiState.automation?.maxCustomSlots ?? 16;
  const slot = findSlotForAddress(slots, target.address);
  const presetId = uiState.activePresetId || null;
  const presetSlot = presetId ? findPresetSlotForAddress(slots, presetId, target.address, target.nodeId) : undefined;
  const items = buildMidiLearnMenuItems({
    address: target.address,
    slot,
    presetSlot,
    presetId,
    armedSlotId: getArmedMidiLearnSlotId(),
    customSlotCount: countCustomSlots(slots),
    maxCustomSlots,
    presetSlotCount: presetId ? countPresetSlots(slots, presetId) : 0,
    maxPresetSlots: getMaxPresetSlots(),
  });

  const menu = document.createElement("div");
  menu.className = "midi-learn-menu";
  menu.setAttribute("role", "menu");
  menu.setAttribute("aria-label", "MIDI Learn");
  menu.innerHTML = `
    <div class="midi-learn-menu-title">${escapeHtml(slot?.label || presetSlot?.label || target.label)}</div>
    ${items.map((item) => `
      <button class="midi-learn-menu-item" type="button" role="menuitem" data-action="${item.action}"${item.disabled ? " disabled" : ""}>
        <span>${escapeHtml(item.label)}</span>
        ${item.hint ? `<span class="midi-learn-menu-hint">${escapeHtml(item.hint)}</span>` : ""}
      </button>
    `).join("")}
  `;

  menu.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest<HTMLButtonElement>(".midi-learn-menu-item") : null;
    if (!button || button.disabled) {
      return;
    }
    closeMenu();
    runMenuAction(button.dataset.action, target, slot, presetSlot, presetId);
  });

  // Measured in place, then pulled back inside the window if it would overhang.
  document.body.appendChild(menu);
  const { width, height } = menu.getBoundingClientRect();
  menu.style.left = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(x, window.innerWidth - width - VIEWPORT_MARGIN_PX))}px`;
  menu.style.top = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(y, window.innerHeight - height - VIEWPORT_MARGIN_PX))}px`;
  menuElement = menu;
  menu.querySelector<HTMLButtonElement>(".midi-learn-menu-item:not(:disabled)")?.focus();
}

function closeMenu(): void {
  menuElement?.remove();
  menuElement = null;
}

function runMenuAction(
  action: string | undefined,
  target: MidiLearnTarget,
  slot: AutomationSlot | undefined,
  presetSlot: AutomationSlot | undefined,
  presetId: string | null,
): void {
  if (action === "learn") {
    startMidiLearn(target, null);
  } else if (action === "learnPreset" && presetId) {
    // On an effect's on/off the item says latch, so it learns as one whatever the slot had.
    startMidiLearn(target, presetId, isBypassAddress(target.address) ? MIDI_MODE_ABSOLUTE : undefined);
  } else if (action === "learnPresetToggle" && presetId) {
    startMidiLearn(target, presetId, MIDI_MODE_TOGGLE);
  } else if (action === "cancel") {
    cancelMidiLearn();
  } else if (action === "clear") {
    // The active preset's own mapping goes first; the global one is left for the next clear.
    if (presetSlot?.midiMap) {
      clearPresetMapping(presetSlot);
    } else if (slot) {
      clearMidiMapping(slot);
    }
  }
}

/**
 * Removes the slot's MIDI mapping, as the MIDI panel's clear button does. The slot itself
 * stays, with any keyboard mapping and DAW automation, so a later learn on the same
 * control fills it again rather than taking another custom slot.
 */
function clearMidiMapping(slot: AutomationSlot): void {
  postMessage({ type: "setAutomationSlot", slotId: slot.slotId, midiMap: null });
  showNotification(`Cleared MIDI mapping for ${slot.label}`);
}

/**
 * Removes a per-preset mapping. A per-preset slot is nothing but its mapping — no DAW
 * parameter, no keys — so the slot goes with it.
 */
function clearPresetMapping(slot: AutomationSlot): void {
  // A slot removed while still listening would leave the engine a learn with nowhere to land.
  if (getArmedMidiLearnSlotId() === slot.slotId) {
    cancelMidiLearn();
  }
  postMessage({ type: "removeAutomationSlot", slotId: slot.slotId });
  showNotification(`Cleared MIDI mapping for ${slot.label} in ${presetDisplayName(slot.presetId ?? "")}`);
}

/**
 * Learns into the control's global slot, or with a `presetId` into that preset's own slot. The
 * mapping learns in `mode`; without one it keeps the mode a re-learned slot had (Absolute for a new one).
 */
function startMidiLearn(target: MidiLearnTarget, presetId: string | null, mode?: number): void {
  const slots = uiState.automation?.slots ?? [];
  const existing = presetId
    ? findPresetSlotForAddress(slots, presetId, target.address, target.nodeId)
    : findSlotForAddress(slots, target.address);
  let slotId = existing?.slotId;
  // A per-preset mapping drives the node it was learned on, not the first of its type.
  const nodeSelector = presetId && target.nodeId ? nodeSelectorFor(target.nodeId) : undefined;

  // The engine handles messages in order, so a slot created here exists by the time learn is armed on it.
  if (!slotId && presetId) {
    const maxPresetSlots = getMaxPresetSlots();
    if (countPresetSlots(slots, presetId) >= maxPresetSlots) {
      showNotification("MIDI Learn unavailable", `this preset already has ${maxPresetSlots} mappings`);
      return;
    }
    slotId = nextPresetSlotId(slots);
    const label = nodeSelector ? (target.nodeLabel ?? target.label) : target.label;
    postMessage({ type: "setAutomationSlot", slotId, label, address: target.address, presetId, ...(nodeSelector ? { nodeSelector } : {}) });
  } else if (slotId && nodeSelector && existing?.nodeSelector !== nodeSelector) {
    // A mapping from before mappings named their node: tie it to the one learned on.
    postMessage({ type: "setAutomationSlot", slotId, nodeSelector });
  } else if (!slotId) {
    const maxCustomSlots = uiState.automation?.maxCustomSlots ?? 16;
    if (countCustomSlots(slots) >= maxCustomSlots) {
      showNotification("MIDI Learn unavailable", `all ${maxCustomSlots} custom slots are in use`);
      return;
    }
    slotId = nextCustomSlotId(slots);
    postMessage({ type: "setAutomationSlot", slotId, label: target.label, address: target.address });
  }

  // Set before arming: arming notifies handleMidiLearnChange, which drops a learn it did not start.
  const learnedLabel = presetId && target.nodeLabel ? target.nodeLabel : target.label;
  const learnMode = mode ?? existing?.midiMap?.mode;
  const toggle = learnMode === MIDI_MODE_TOGGLE;
  listening = { slotId, label: existing?.label || learnedLabel, presetName: presetId ? presetDisplayName(presetId) : null, toggle };
  armMidiLearn(slotId, learnMode);
  showListeningStatus(listening);
}

function handleMidiLearnChange(armedSlotId: string | null, capture: MidiLearnCapture | null): void {
  if (!listening) {
    return;
  }
  if (capture?.slotId === listening.slotId) {
    const scope = listening.presetName ? ` for ${listening.presetName}` : "";
    const how = listening.toggle ? " as a toggle" : "";
    showNotification(`Mapped ${describeMidiMap(capture)} to ${listening.label}${scope}${how}`);
  }
  if (armedSlotId !== listening.slotId) {
    // Captured, cancelled, or replaced by a learn armed from the MIDI panel.
    listening = null;
    hideListeningStatus();
  }
}

function handleKeydown(event: KeyboardEvent): void {
  if (menuElement && (event.key === "ArrowDown" || event.key === "ArrowUp")) {
    const items = Array.from(menuElement.querySelectorAll<HTMLButtonElement>(".midi-learn-menu-item:not(:disabled)"));
    const step = event.key === "ArrowDown" ? 1 : -1;
    const current = items.findIndex((item) => item === document.activeElement);
    // With nothing focused yet, Down lands on the first item and Up on the last.
    const from = current >= 0 ? current : (step > 0 ? -1 : 0);
    items[(from + step + items.length) % items.length]?.focus();
    event.preventDefault();
    return;
  }
  if (event.key !== "Escape") {
    return;
  }
  if (menuElement) {
    closeMenu();
    event.preventDefault();
  } else if (listening) {
    cancelMidiLearn();
  }
}

function showListeningStatus(learn: { label: string; presetName: string | null; toggle: boolean }): void {
  if (!statusElement) {
    statusElement = document.createElement("div");
    statusElement.className = "midi-learn-status";
    statusElement.setAttribute("role", "status");
    // One listener for the element's lifetime; its contents are replaced for each learn.
    statusElement.addEventListener("click", (event) => {
      if (event.target instanceof Element && event.target.closest(".midi-learn-status-cancel")) {
        cancelMidiLearn();
      }
    });
    document.body.appendChild(statusElement);
  }
  statusElement.innerHTML = `
    <span class="midi-learn-status-dot" aria-hidden="true"></span>
    <span class="midi-learn-status-text">Move a MIDI control to ${learn.toggle ? "toggle" : "map"} <strong>${escapeHtml(learn.label)}</strong>${learn.presetName ? ` for <strong>${escapeHtml(learn.presetName)}</strong>` : ""}</span>
    <button class="midi-learn-status-cancel" type="button">Cancel</button>
  `;
  statusElement.hidden = false;
}

function hideListeningStatus(): void {
  if (statusElement) {
    statusElement.hidden = true;
  }
}
