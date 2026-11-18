/**
 * What the Your Presets tab shows, worked out from plain data: the order of the cards, the
 * tags on offer, the folder rows and a card's initials. No DOM and no uiState, so it is tested
 * directly.
 */

import { presetNameCollator, sortPresetsAlphabetically } from "../presets/sorting.js";
import type { Preset, PresetFolder } from "../types.js";

export type LocalPresetSort = "name" | "rating";

export function isLocalPresetSort(value: unknown): value is LocalPresetSort {
  return value === "name" || value === "rating";
}

/**
 * The cards in order. "rating" puts the highest rated first and the unrated last, by name
 * within a rating. `keepOrder` is for Recents, whose order is the point of the folder.
 */
export function sortLocalPresets(
  presets: Preset[],
  sort: LocalPresetSort,
  getRating: (presetId: string) => number | null,
  keepOrder = false,
): Preset[] {
  if (sort === "rating") {
    const byName = sortPresetsAlphabetically(presets);
    const ratings = new Map(byName.map((preset) => [preset.id, getRating(preset.id) ?? 0]));
    // Array.sort is stable, so presets with the same rating keep their name order.
    return byName.sort((left, right) => (ratings.get(right.id) ?? 0) - (ratings.get(left.id) ?? 0));
  }
  return keepOrder ? presets.slice() : sortPresetsAlphabetically(presets);
}

/** Every tag the presets carry, once each, in name order. */
export function collectPresetTags(presets: Preset[]): string[] {
  const tags = new Set<string>();
  presets.forEach((preset) => {
    (preset.tags ?? []).forEach((tag) => {
      const trimmed = typeof tag === "string" ? tag.trim() : "";
      if (trimmed) {
        tags.add(trimmed);
      }
    });
  });
  return Array.from(tags).sort((left, right) => presetNameCollator.compare(left, right));
}

/**
 * Two letters for a card with no artwork: the first two words' initials, else the first two
 * characters. Punctuation is not a word, and neither is a number the name starts with, so
 * "01 David Gilmour - PULSE" is DG rather than 0D.
 */
export function presetInitials(name: string): string {
  let words = name.split(/\s+/).map((word) => word.replace(/[^\p{L}\p{N}]/gu, "")).filter(Boolean);
  while (words.length > 1 && /^\p{N}+$/u.test(words[0])) {
    words = words.slice(1);
  }
  if (words.length >= 2) {
    return `${words[0][0]}${words[1][0]}`.toUpperCase();
  }
  return (words[0] ?? "").slice(0, 2).toUpperCase() || "?";
}

export interface LocalFolderRow {
  id: string;
  name: string;
  /** 0 for a top-level folder. */
  depth: number;
}

/** The folder tree as rows, parents before their children. Expects the folders already sorted. */
export function flattenPresetFolders(folders: PresetFolder[], depth = 0): LocalFolderRow[] {
  return folders.flatMap((folder) => [
    { id: folder.id, name: folder.name, depth },
    ...flattenPresetFolders(folder.children ?? [], depth + 1),
  ]);
}

/** "12 presets", or "3 of 12 presets" when a search or a tag has narrowed them. */
export function describePresetCount(shown: number, inFolder: number): string {
  const noun = inFolder === 1 ? "preset" : "presets";
  return shown === inFolder ? `${inFolder} ${noun}` : `${shown} of ${inFolder} ${noun}`;
}
