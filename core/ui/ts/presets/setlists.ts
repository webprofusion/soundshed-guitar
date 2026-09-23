/**
 * Setlists: banked slots of presets for live use, the editor behind them, and
 * the cursor the host moves through them.
 */

import { generateResourceId } from "../archiveUtils.js";
import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { setPresetDirty, uiState } from "../state.js";
import { setPresetLoadingId } from "../presetLibraryStore.js";
import type { Setlist } from "../types.js";
import { setlistCollapsible, setlistEditorHeader, setlistListElement, setlistPanel, setlistSlotsElement, setlistToggle } from "./dom.js";
import { renderActivePreset } from "./filter.js";

export function normalizeSetlistName(name: string): string {
  return name.trim();
}

/** Setlists only make sense against the regular preset library, not the Multi-Rig list. */
export function setSetlistPanelVisible(visible: boolean): void {
  if (setlistCollapsible) {
    setlistCollapsible.hidden = !visible;
  }
}

export function ensureSetlists(): void {
  const stored = uiState.setlists ?? [];
  uiState.activeSetlistId = uiState.activeSetlistId || (stored[0]?.id ?? null);
}

export function persistSetlists(): void {
  postMessage({
    type: "setSetlists",
    setlists: uiState.setlists ?? [],
    activeSetlistId: uiState.activeSetlistId ?? "",
    cursorIndex: uiState.setlistCursorIndex ?? 0,
  });
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
  persistSetlists();
  setActiveSetlist(newSetlist.id);
  return newSetlist;
}

export function addPresetToSetlist(presetId: string): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  setlist.slots.push({ presetId });
  persistSetlists();
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
  persistSetlists();
  renderSetlistPanel();
  return true;
}

export function clearActiveSetlistSlot(slotIndex: number): boolean {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist || slotIndex < 0 || slotIndex >= setlist.slots.length) {
    return false;
  }
  setlist.slots[slotIndex] = { presetId: "" };
  persistSetlists();
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
  persistSetlists();
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

  setlists.splice(activeIndex, 1);
  const nextActive = setlists[activeIndex] ?? setlists[activeIndex - 1] ?? null;
  uiState.activeSetlistId = nextActive?.id ?? null;
  uiState.setlistCursorIndex = 0;
  persistSetlists();
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
  persistSetlists();
  renderSetlistPanel();
}

export function removeSetlistSlot(index: number): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  setlist.slots.splice(index, 1);
  persistSetlists();
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
            <div class="setlist-item ${active}" data-setlist-id="${setlist.id}">
              <span>${setlist.name}</span>
              <span class="bank-pill">${bankLabel}</span>
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
          <div class="setlist-slot${isActive}" data-slot-index="${index}" draggable="true">
            <span class="setlist-slot-title">${presetName}</span>
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

    slotEl.addEventListener("dragstart", (event) => {
      const index = slotEl.dataset.slotIndex ?? "";
      event.dataTransfer?.setData("application/x-setlist-slot", index);
      event.dataTransfer?.setDragImage(slotEl, 20, 20);
    });

    slotEl.addEventListener("dragover", (event) => {
      event.preventDefault();
    });

    slotEl.addEventListener("drop", (event) => {
      event.preventDefault();
      const fromIndex = Number(event.dataTransfer?.getData("application/x-setlist-slot") ?? -1);
      const toIndex = Number(slotEl.dataset.slotIndex ?? -1);
      if (fromIndex >= 0 && toIndex >= 0) {
        moveSetlistSlot(fromIndex, toIndex);
      }
    });
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
