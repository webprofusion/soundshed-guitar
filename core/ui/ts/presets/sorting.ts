import type { Preset } from "../types.js";
export const presetNameCollator = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });

export const PRESET_FOLDER_FAVORITES_ID = "__favorites__";

export const PRESET_FOLDER_RECENTS_ID = "__recents__";

export function comparePresetNames(left: Preset, right: Preset): number {
  const leftName = left.name?.trim() || left.id;
  const rightName = right.name?.trim() || right.id;
  const nameComparison = presetNameCollator.compare(leftName, rightName);
  if (nameComparison !== 0) {
    return nameComparison;
  }
  return presetNameCollator.compare(left.id, right.id);
}

export function sortPresetsAlphabetically(presets: Preset[]): Preset[] {
  return [...presets].sort(comparePresetNames);
}
