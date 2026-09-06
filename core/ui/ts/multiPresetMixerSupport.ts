/**
 * multiPresetMixerSupport.ts — pure helpers behind the Multi-Rig tab.
 *
 * Nothing here touches the DOM or uiState, so the list filtering, the
 * "does the mixer still match this Multi-Rig" check and the slot-name
 * resolution can be unit tested on their own.
 */

import type { CompositePreset } from "./types.js";

/** Range of the Multi-Rig's own output level. A mix of several rigs usually needs trimming down, so the range leans that way. */
export const MIX_GAIN_MIN_DB = -24;
export const MIX_GAIN_MAX_DB = 12;

export function normalizeCompositePresetTag(tag: string): string {
  return tag.trim().toLowerCase();
}

/** Every distinct tag used across the saved Multi-Rigs, normalised and sorted. */
export function collectCompositePresetTags(presets: readonly CompositePreset[]): string[] {
  const seen = new Set<string>();
  const result: string[] = [];

  for (const preset of presets) {
    for (const tag of preset.tags ?? []) {
      const normalized = normalizeCompositePresetTag(tag);
      if (!normalized || seen.has(normalized)) continue;
      seen.add(normalized);
      result.push(normalized);
    }
  }

  return result.sort((left, right) => left.localeCompare(right));
}

/**
 * Filter the Multi-Rig list by the library search box. Matches the name,
 * description, tags and — when a resolver is supplied — the names of the
 * presets in each slot, so searching for a preset also finds the mixes it
 * belongs to.
 */
export function filterCompositePresets(
  presets: readonly CompositePreset[],
  query: string,
  slotNames?: (preset: CompositePreset) => readonly string[],
): CompositePreset[] {
  const needle = query.trim().toLowerCase();
  if (!needle) {
    return [...presets];
  }

  return presets.filter((preset) => {
    const haystack = [
      preset.name,
      preset.description ?? "",
      ...(preset.tags ?? []),
      ...(slotNames?.(preset) ?? []),
    ].join("\n").toLowerCase();
    return haystack.includes(needle);
  });
}

/**
 * True when the mixer holds exactly the presets this Multi-Rig was saved
 * with (order and levels aside). Used to decide whether the mixer toolbar's
 * Save should update the loaded Multi-Rig or create a new one.
 */
export function compositePresetMatchesMixer(preset: CompositePreset, activePresetIds: readonly string[]): boolean {
  const slotIds = new Set((preset.slots ?? []).map((slot) => slot.presetId));
  const activeIds = new Set(activePresetIds);
  if (slotIds.size !== activeIds.size) {
    return false;
  }
  for (const id of activeIds) {
    if (!slotIds.has(id)) {
      return false;
    }
  }
  return true;
}

/** Display names for each slot's preset, falling back to the raw id when the preset is gone. */
export function resolveCompositeSlotNames(
  preset: CompositePreset,
  lookupName: (presetId: string) => string | undefined,
): string[] {
  return (preset.slots ?? []).map((slot) => lookupName(slot.presetId) ?? slot.presetId);
}
