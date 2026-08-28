import { uiState } from "../state.js";
import type { Preset } from "../types.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { stripLegacyGlobals } from "./sanitize.js";
export function cachePresetInMemory(preset: Preset): void {
  const cleanedPreset = stripLegacyGlobals(preset);
  normalizePresetScenes(cleanedPreset);
  uiState.presetCache.set(cleanedPreset.id, cleanedPreset);
  if (!uiState.presets.some((p) => p.id === cleanedPreset.id)) {
    uiState.presets.push(cleanedPreset);
  }
}
