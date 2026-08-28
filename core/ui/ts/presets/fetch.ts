import type { Preset } from "../types.js";
import { postMessage } from "../bridge.js";
import { clonePreset, uiState } from "../state.js";
const PRESET_REQUEST_TIMEOUT_MS = 5000;
const pendingPresetRequests = new Map<string, {
  presetId: string;
  resolve: (preset: Preset) => void;
  reject: (error: Error) => void;
  timeoutId: number;
}>();

export function createPresetRequestId(): string {
  return typeof crypto !== "undefined" && typeof crypto.randomUUID === "function"
    ? crypto.randomUUID()
    : `preset-request-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;
}

export function requestPresetFromBackend(presetId: string): Promise<Preset> {
  const requestId = createPresetRequestId();
  postMessage({ type: "getPresetById", presetId, requestId });

  return new Promise((resolve, reject) => {
    const timeoutId = window.setTimeout(() => {
      pendingPresetRequests.delete(requestId);
      reject(new Error(`Preset ${presetId} request timed out`));
    }, PRESET_REQUEST_TIMEOUT_MS);

    pendingPresetRequests.set(requestId, { presetId, resolve, reject, timeoutId });
  });
}

export function refreshPresetCacheEntryFromBackend(presetId: string): void {
  const id = presetId.trim();
  if (!id) {
    return;
  }

  void requestPresetFromBackend(id).catch((error) => {
    console.warn("Preset sync refresh failed", id, error);
  });
}

export function handlePresetDataMessage(preset: Preset, requestId?: string): void {
  if (!preset?.id) return;
  const requestPending = requestId ? pendingPresetRequests.get(requestId) : null;
  const presetPending = pendingPresetRequests.get(preset.id);
  const pending = requestPending ?? presetPending;
  if (pending) {
    window.clearTimeout(pending.timeoutId);
    if (requestPending && requestId) {
      pendingPresetRequests.delete(requestId);
    } else {
      pendingPresetRequests.delete(preset.id);
    }
    pending.resolve(preset);
  }
  uiState.presetCache.set(preset.id, clonePreset(preset));
  const index = uiState.presets.findIndex((p) => p.id === preset.id);
  if (index >= 0) {
    uiState.presets[index] = clonePreset(preset);
  } else {
    uiState.presets.push(clonePreset(preset));
  }
}

export function rejectPendingPresetRequest(requestId: string | undefined, message: string, detail?: string): void {
  const pending = requestId ? pendingPresetRequests.get(requestId) : null;
  if (pending) {
    window.clearTimeout(pending.timeoutId);
    pendingPresetRequests.delete(requestId!);
    pending.reject(new Error(detail ? `${message}: ${detail}` : message));
  }
}
