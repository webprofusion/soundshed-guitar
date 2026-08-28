import { clonePreset, uiState } from "../state.js";
import type { Preset, PresetArchiveSessionState } from "../types.js";
export function getPresetArchiveSessionState(): PresetArchiveSessionState | null {
  return uiState.presetArchiveSession?.active ? uiState.presetArchiveSession : null;
}

export function applyPresetArchiveSessionState(state: PresetArchiveSessionState | null): void {
  if (state?.active) {
    uiState.presetArchiveSession = {
      active: true,
      archiveName: state.archiveName ?? uiState.presetArchiveSession?.archiveName,
      archiveKey: state.archiveKey ?? uiState.presetArchiveSession?.archiveKey,
      presetCount: typeof state.presetCount === "number" ? state.presetCount : uiState.presetArchiveSession?.presetCount,
    };
  } else {
    uiState.presetArchiveSession = null;
  }
}

export function stripLegacyGlobals(preset: Preset): Preset {
  const cleaned = clonePreset(preset);
  delete (cleaned as Record<string, unknown>).globals;
  delete (cleaned as Record<string, unknown>).global;
  return cleaned;
}

export function sanitizePresetForArchive(preset: Preset): Preset {
  const cleaned = clonePreset(preset);
  delete (cleaned as Record<string, unknown>).globals;
  delete (cleaned as Record<string, unknown>).global;
  delete (cleaned as Record<string, unknown>).globalSignalChain;
  return cleaned;
}
