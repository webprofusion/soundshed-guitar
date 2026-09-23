/**
 * Undo/redo across preset loads.
 *
 * Stepping through the history re-applies a preset, which would itself record a
 * new entry — hence the replay flag that suppresses it.
 *
 * The loader is passed in rather than imported: load.ts records every load here,
 * so importing it back would put the two modules in an import cycle.
 */

import { uiState } from "../state.js";
import { presetRedoBtn, presetUndoBtn } from "./dom.js";
import { updatePresetDropdownSelection } from "./filter.js";

// ── Preset navigation history ────────────────────────────────────────────────
// Tracks which presets were loaded, so the user can step back to the one they
// were just on after auditioning something else. Only preset IDs are held, so a
// deep history costs nothing; the cap keeps it to a useful working window rather
// than a full session log. This is navigation history, not an edit undo stack —
// it does not restore unsaved parameter tweaks.

export const presetHistory: string[] = [];

let presetHistoryIndex = -1;

let replayingPresetHistory = false;

export const MAX_PRESET_HISTORY = 10;

export function recordPresetInHistory(presetId: string): void {
  // Undo/redo re-apply presets through the same path; those must move the cursor,
  // not rewrite the history they are walking.
  if (replayingPresetHistory) {
    return;
  }
  if (presetHistory[presetHistoryIndex] === presetId) {
    return;
  }

  // Branching from a past entry drops the forward steps that are no longer reachable.
  presetHistory.length = presetHistoryIndex + 1;
  presetHistory.push(presetId);
  if (presetHistory.length > MAX_PRESET_HISTORY) {
    presetHistory.shift(); // oldest falls off; cursor still points at the newest
  } else {
    presetHistoryIndex++;
  }
  updatePresetHistoryButtons();
}

export function updatePresetHistoryButtons(): void {
  if (presetUndoBtn) {
    presetUndoBtn.disabled = presetHistoryIndex <= 0;
  }
  if (presetRedoBtn) {
    presetRedoBtn.disabled = presetHistoryIndex >= presetHistory.length - 1;
  }
}

export async function stepPresetHistory(offset: -1 | 1, applyPreset: (presetId: string) => Promise<void>): Promise<void> {
  const targetIndex = presetHistoryIndex + offset;
  if (targetIndex < 0 || targetIndex >= presetHistory.length) {
    return;
  }
  const targetId = presetHistory[targetIndex];
  if (!targetId) {
    return;
  }

  replayingPresetHistory = true;
  try {
    await applyPreset(targetId);
  } finally {
    replayingPresetHistory = false;
  }

  // Only advance the cursor once the load actually went ahead — applyPresetFromLibrary
  // swallows failures and can also be cancelled by the unsaved-changes prompt. A load by id
  // may still be in flight, the preset shown loading until the engine answers.
  if (uiState.activePresetId === targetId || uiState.presetLoadingId === targetId) {
    presetHistoryIndex = targetIndex;
  }
  updatePresetHistoryButtons();
  updatePresetDropdownSelection();
}
