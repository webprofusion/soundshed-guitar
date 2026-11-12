/**
 * Setlists: banked slots of presets for live use, the editor behind them, and
 * the cursor the host moves through them. Each edit is made by the engine, asked for by name
 * (createSetlist, updateSetlist, deleteSetlist, addSetlistSlot, setSetlistSlot, moveSetlistSlot,
 * removeSetlistSlot); the local copy changes at once and the engine's setlists reply confirms it.
 */

import { generateResourceId } from "../archiveUtils.js";
import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { TOUCH_DRAG_HOLD_MS, beginPointerDrag, type PointerDragTarget } from "../pointerDrag.js";
import { setPresetDirty, uiState } from "../state.js";
import { setPresetLoadingId } from "../presetLibraryStore.js";
import type { Setlist } from "../types.js";
import { setlistCollapsible, setlistEditorHeader, setlistListElement, setlistPanel, setlistSlotsElement, setlistToggle } from "./dom.js";
import { renderActivePreset } from "./filter.js";
import { syncSetlistAddCurrentButton } from "./libraryTabs.js";
import { escapeHtml } from "../utils.js";

export function normalizeSetlistName(name: string): string {
  return name.trim();
}

export function ensureSetlists(): void {
  const stored = uiState.setlists ?? [];
  uiState.activeSetlistId = uiState.activeSetlistId || (stored[0]?.id ?? null);
}

/**
 * Makes a setlist the active one, its cursor on the first slot, as a bank select does. The
 * engine stores the choice (`selectSetlist`) and answers with "setlistCursorChanged"; the
 * setlists themselves are not re-sent.
 */
export function setActiveSetlist(id: string): void {
  uiState.activeSetlistId = id;
  uiState.setlistCursorIndex = 0;
  postMessage({ type: "selectSetlist", setlistId: id });
  renderSetlistPanel();
}

export function applySetlistsFromBackend(setlists: Setlist[], activeSetlistId?: string | null): void {
  uiState.setlists = Array.isArray(setlists) ? setlists : [];
  uiState.activeSetlistId = activeSetlistId ?? (uiState.setlists[0]?.id ?? null);
  renderSetlistPanel();
}

export function applySetlistCursorFromBackend(cursorIndex: number, presetId?: string, activeSetlistId?: string): void {
  if (typeof activeSetlistId === "string" && activeSetlistId && activeSetlistId !== uiState.activeSetlistId) {
    uiState.activeSetlistId = activeSetlistId;
  }
  uiState.setlistCursorIndex = cursorIndex;
  renderSetlistPanel();
  // The backend applies the slot's preset itself and reports it via "presetLoaded";
  // loading it again from here would race that swap and leave both presets in the mixer.
  // A step onto the preset already playing loads nothing, so no "presetLoaded" follows to
  // end the loading state; the cursor report is the last word.
  if (presetId && uiState.presetLoadingId === presetId && uiState.activePresetId === presetId) {
    setPresetLoadingId(null);
    renderActivePreset();
  }
}

/**
 * Mirrors the backend: a setlist step onto the preset already playing on its own keeps it
 * as it is, so no load follows and no loading state should be shown for it.
 */
export function isOnlyPlayingPreset(presetId: string): boolean {
  return uiState.activePresetId === presetId && (uiState.mixer?.activePresetIds ?? []).every((id) => id === presetId);
}

export async function selectSetlistSlot(index: number): Promise<void> {
  const activeSetlist = findSetlistById(uiState.activeSetlistId);
  if (!activeSetlist || index < 0 || index >= activeSetlist.slots.length) return;
  const presetId = activeSetlist.slots[index].presetId;
  if (!presetId) return;
  if (uiState.presetDirty && uiState.activePresetId && uiState.activePresetId !== presetId) {
    const confirmDiscard = await showConfirm("Discard unsaved changes?", "Unsaved changes");
    if (!confirmDiscard) return;
    setPresetDirty(false);
  }
  uiState.setlistCursorIndex = index;
  // "setSetlistCursor" is the single switch verb — the backend moves the cursor *and* swaps
  // the preset in, matching what a footswitch or MIDI program change does.
  const reloads = !isOnlyPlayingPreset(presetId);
  postMessage({ type: "setSetlistCursor", cursorIndex: index });
  if (reloads) {
    setPresetLoadingId(presetId);
  }
  renderSetlistPanel();
  renderActivePreset();
}

export function findSetlistById(id: string | null | undefined): Setlist | undefined {
  if (!id) {
    return undefined;
  }
  return (uiState.setlists ?? []).find((setlist) => setlist.id === id);
}

export function isBankAvailable(bank: number, excludeId?: string): boolean {
  return !(uiState.setlists ?? []).some((setlist) => setlist.bank === bank && setlist.id !== excludeId);
}

export function createSetlist(name: string, bank?: number | null): Setlist | null {
  const trimmed = normalizeSetlistName(name);
  if (!trimmed) {
    showNotification("Setlist name required", "Enter a setlist name.");
    return null;
  }
  if (typeof bank === "number" && !isBankAvailable(bank)) {
    showNotification("Bank already used", "Only one setlist can use a bank number.");
    return null;
  }

  const newSetlist: Setlist = {
    id: generateResourceId(trimmed),
    name: trimmed,
    bank: typeof bank === "number" ? bank : null,
    slots: [],
  };
  uiState.setlists = uiState.setlists ?? [];
  uiState.setlists.push(newSetlist);
  uiState.activeSetlistId = newSetlist.id;
  uiState.setlistCursorIndex = 0;
  // The engine stores it and makes it active; its setlists reply confirms what is drawn now.
  postMessage({ type: "createSetlist", id: newSetlist.id, name: trimmed, bank: newSetlist.bank ?? null });
  renderSetlistPanel();
  return newSetlist;
}

export function addPresetToSetlist(presetId: string): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  setlist.slots.push({ presetId });
  postMessage({ type: "addSetlistSlot", setlistId: setlist.id, presetId });
  renderSetlistPanel();
}

export function assignPresetToActiveSetlistSlot(slotIndex: number, presetId: string): boolean {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist || slotIndex < 0 || !presetId.trim()) {
    return false;
  }

  while (setlist.slots.length <= slotIndex) {
    setlist.slots.push({ presetId: "" });
  }

  setlist.slots[slotIndex] = { presetId };
  postMessage({ type: "setSetlistSlot", setlistId: setlist.id, index: slotIndex, presetId });
  renderSetlistPanel();
  return true;
}

export function clearActiveSetlistSlot(slotIndex: number): boolean {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist || slotIndex < 0 || slotIndex >= setlist.slots.length) {
    return false;
  }
  setlist.slots[slotIndex] = { presetId: "" };
  postMessage({ type: "setSetlistSlot", setlistId: setlist.id, index: slotIndex, presetId: "" });
  renderSetlistPanel();
  return true;
}

export function updateActiveSetlistDetails(name: string, bank?: number | null): boolean {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    showNotification("No setlist selected", "Select a setlist before editing it.");
    return false;
  }

  const trimmed = normalizeSetlistName(name);
  if (!trimmed) {
    showNotification("Setlist name required", "Enter a setlist name.");
    return false;
  }
  if (typeof bank === "number" && (!Number.isFinite(bank) || bank < 0)) {
    showNotification("Invalid bank", "Bank must be a non-negative number.");
    return false;
  }
  if (typeof bank === "number" && !isBankAvailable(bank, setlist.id)) {
    showNotification("Bank already used", "Only one setlist can use a bank number.");
    return false;
  }

  setlist.name = trimmed;
  setlist.bank = typeof bank === "number" ? bank : null;
  postMessage({ type: "updateSetlist", setlistId: setlist.id, name: trimmed, bank: setlist.bank });
  renderSetlistPanel();
  return true;
}

export function deleteActiveSetlist(): boolean {
  const setlists = uiState.setlists ?? [];
  const activeIndex = setlists.findIndex((setlist) => setlist.id === uiState.activeSetlistId);
  if (activeIndex < 0) {
    showNotification("No setlist selected", "Select a setlist before deleting it.");
    return false;
  }

  const [deleted] = setlists.splice(activeIndex, 1);
  const nextActive = setlists[activeIndex] ?? setlists[activeIndex - 1] ?? null;
  uiState.activeSetlistId = nextActive?.id ?? null;
  uiState.setlistCursorIndex = 0;
  postMessage({ type: "deleteSetlist", setlistId: deleted.id });
  renderSetlistPanel();
  return true;
}

export function moveSetlistSlot(fromIndex: number, toIndex: number): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  if (fromIndex < 0 || toIndex < 0 || fromIndex >= setlist.slots.length || toIndex >= setlist.slots.length) {
    return;
  }
  if (fromIndex === toIndex) {
    return;
  }
  const [slot] = setlist.slots.splice(fromIndex, 1);
  setlist.slots.splice(toIndex, 0, slot);
  // The cursor stays on the slot it was on: the engine steps from it, so a footswitch
  // "next" after the move must still land on the slot after the one playing.
  const cursor = uiState.setlistCursorIndex ?? 0;
  if (cursor === fromIndex) {
    uiState.setlistCursorIndex = toIndex;
  } else if (fromIndex < cursor && cursor <= toIndex) {
    uiState.setlistCursorIndex = cursor - 1;
  } else if (toIndex <= cursor && cursor < fromIndex) {
    uiState.setlistCursorIndex = cursor + 1;
  }
  postMessage({ type: "moveSetlistSlot", setlistId: setlist.id, fromIndex, toIndex });
  renderSetlistPanel();
}

export function removeSetlistSlot(index: number): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  setlist.slots.splice(index, 1);
  const cursor = uiState.setlistCursorIndex ?? 0;
  if (index < cursor) {
    uiState.setlistCursorIndex = cursor - 1;
  } else if (index === cursor) {
    uiState.setlistCursorIndex = Math.max(0, Math.min(cursor, setlist.slots.length - 1));
  }
  postMessage({ type: "removeSetlistSlot", setlistId: setlist.id, index });
  renderSetlistPanel();
}

export function renderSetlistPanel(): void {
  if (!setlistListElement || !setlistSlotsElement || !setlistEditorHeader) {
    return;
  }

  const setlists = uiState.setlists ?? [];
  setlistListElement.innerHTML = setlists.length
    ? setlists
        .map((setlist) => {
          const active = setlist.id === uiState.activeSetlistId ? "active" : "";
          const bankLabel = typeof setlist.bank === "number" ? `Bank ${setlist.bank}` : "No Bank";
          return `
            <div class="setlist-item ${active}" data-setlist-id="${escapeHtml(setlist.id)}">
              <span>${escapeHtml(setlist.name)}</span>
              <span class="bank-pill">${escapeHtml(bankLabel)}</span>
            </div>
          `;
        })
        .join("")
    : '<div class="preset-library-empty">No setlists yet.</div>';

  setlistListElement.querySelectorAll<HTMLElement>(".setlist-item").forEach((item) => {
    item.addEventListener("click", () => {
      const id = item.dataset.setlistId ?? "";
      if (id && id !== uiState.activeSetlistId) {
        setActiveSetlist(id);
      }
    });
  });

  syncSetlistAddCurrentButton();
  const activeSetlist = findSetlistById(uiState.activeSetlistId);
  if (!activeSetlist) {
    setlistEditorHeader.textContent = "Select a setlist";
    setlistSlotsElement.innerHTML = "";
    return;
  }

  setlistEditorHeader.textContent = `${activeSetlist.name}${typeof activeSetlist.bank === "number" ? ` (Bank ${activeSetlist.bank})` : ""}`;
  if (!activeSetlist.slots.length) {
    setlistSlotsElement.innerHTML = '<div class="preset-library-empty">Drop presets to add slots.</div>';
  } else {
    const cursorIdx = uiState.setlistCursorIndex ?? 0;
    setlistSlotsElement.innerHTML = activeSetlist.slots
      .map((slot, index) => {
        const presetName = uiState.presetCache.get(slot.presetId)?.name ?? slot.presetId;
        const isActive = index === cursorIdx ? " active" : "";
        return `
          <div class="setlist-slot${isActive}" data-slot-index="${index}">
            <span class="setlist-slot-title">${escapeHtml(presetName)}</span>
            <button class="setlist-slot-remove" data-slot-index="${index}" type="button">×</button>
          </div>
        `;
      })
      .join("");
  }

  setlistSlotsElement.querySelectorAll<HTMLButtonElement>(".setlist-slot-remove").forEach((button) => {
    button.addEventListener("click", (e) => {
      e.stopPropagation();
      const index = Number(button.dataset.slotIndex ?? -1);
      if (index >= 0) {
        removeSetlistSlot(index);
      }
    });
  });

  setlistSlotsElement.querySelectorAll<HTMLElement>(".setlist-slot").forEach((slotEl) => {
    slotEl.addEventListener("click", () => {
      const index = Number(slotEl.dataset.slotIndex ?? -1);
      if (index >= 0) {
        void selectSetlistSlot(index);
      }
    });

    // Reordered by pointer (pointerDrag.ts), so an iPhone, which has no HTML5 drag, can do it.
    // A pack dropped from Explorer is a native drag, and left for the global importer.
    slotEl.addEventListener("pointerdown", (event) => {
      if (!(event.target instanceof Element) || !event.target.closest(".setlist-slot-remove")) {
        beginSetlistSlotDrag(event, slotEl);
      }
    });
  });
}

/** A slot a dragged one can land on, with its index read when the pointer reached it. */
interface SetlistSlotDropTarget extends PointerDragTarget {
  index: number;
}

function beginSetlistSlotDrag(event: PointerEvent, slotEl: HTMLElement): void {
  const fromIndex = Number(slotEl.dataset.slotIndex ?? -1);
  if (!(fromIndex >= 0)) {
    return;
  }
  beginPointerDrag<SetlistSlotDropTarget>(event, {
    source: slotEl,
    sourceSelector: ".setlist-slot",
    previewClass: "list-drag-preview",
    touchHoldMs: TOUCH_DRAG_HOLD_MS,
    autoScroll: setlistSlotsElement,
    resolveTarget: (moveEvent) => {
      const element = document.elementFromPoint(moveEvent.clientX, moveEvent.clientY)?.closest<HTMLElement>(".setlist-slot[data-slot-index]");
      const index = Number(element?.dataset.slotIndex ?? -1);
      return element && setlistSlotsElement?.contains(element) && index >= 0 && index !== fromIndex
        ? { element, index }
        : null;
    },
    setTargetHighlight: (target, highlighted) => target.element.classList.toggle("drag-over", highlighted),
    // After the release, as presets/drag.ts explains: the move re-renders the slots.
    onDrop: (target) => {
      if (target) {
        const toIndex = target.index;
        window.setTimeout(() => moveSetlistSlot(fromIndex, toIndex), 0);
      }
    },
  });
}

export function setSetlistExpanded(expanded: boolean): void {
  if (!setlistCollapsible || !setlistToggle || !setlistPanel) {
    return;
  }
  setlistCollapsible.classList.toggle("open", expanded);
  setlistToggle.setAttribute("aria-expanded", expanded ? "true" : "false");
  setlistPanel.setAttribute("aria-hidden", expanded ? "false" : "true");
}
