import { appendLog } from "./logging.js";
import { setPresetDirty } from "./state.js";

const NAMBridge = {
  postMessage(message: unknown): void {
    if (!window.IPlugSendMsg) return;

    // WebView bridge expects a JSON string; stringify objects defensively.
    const payload = typeof message === "string" ? message : JSON.stringify(message);
    window.IPlugSendMsg(payload);
  },
};

window.NAMBridge = NAMBridge;

export function postMessage(payload: unknown): void {
  NAMBridge.postMessage(payload);
}

export function setAppSetting(key: string, value: unknown): void {
  postMessage({
    type: "setSetting",
    key,
    value,
  });
}

export function setParameter(id: string, value: number): void {
  postMessage({
    type: "setParameter",
    name: id,
    value,
  });
  appendLog(`${id} → ${value}`);
  setPresetDirty(true);
}

// Multi-Preset Mixer controls
export function addActivePreset(presetId: string): void {
  postMessage({ type: "addActivePreset", presetId });
  appendLog(`addActivePreset → ${presetId}`);
}

export function removeActivePreset(presetId: string): void {
  postMessage({ type: "removeActivePreset", presetId });
  appendLog(`removeActivePreset → ${presetId}`);
}

export function setPresetMix(presetId: string, mix: number): void {
  postMessage({ type: "setPresetMix", presetId, mix });
  appendLog(`setPresetMix(${presetId}) → ${mix.toFixed(3)}`);
}

export function setPresetPan(presetId: string, pan: number): void {
  postMessage({ type: "setPresetPan", presetId, pan });
  appendLog(`setPresetPan(${presetId}) → ${pan.toFixed(3)}`);
}

export function setPresetMute(presetId: string, mute: boolean): void {
  postMessage({ type: "setPresetMute", presetId, mute });
  appendLog(`setPresetMute(${presetId}) → ${mute}`);
}

export function setPresetSolo(presetId: string, solo: boolean): void {
  postMessage({ type: "setPresetSolo", presetId, solo });
  appendLog(`setPresetSolo(${presetId}) → ${solo}`);
}

export function setMasterGain(gain: number): void {
  postMessage({ type: "setMasterGain", gain });
  appendLog(`setMasterGain → ${gain.toFixed(3)}`);
}

export function setLimiterEnabled(enabled: boolean): void {
  postMessage({ type: "setLimiterEnabled", enabled });
  appendLog(`setLimiterEnabled → ${enabled}`);
}

export function setMetronome(payload: {
  bpm?: number;
  enabled?: boolean;
  volumeDb?: number;
  pan?: number;
  clickType?: string;
  clickConfig?: Array<{ id: string; label?: string; lowPath?: string; highPath?: string }>;
}): void {
  postMessage({ type: "setMetronome", ...payload });
}

export function getRiffLibrary(): void {
  postMessage({ type: "getRiffLibrary" });
}

export function setRiffLibraryPath(path: string): void {
  postMessage({ type: "setRiffLibraryPath", path });
}

export function startRiffCapture(payload: {
  tempoBpm: number;
  timeSigNum: number;
  timeSigDen: number;
  bars: number;
  countInBars: number;
  patternType: "click" | "drum";
  patternId?: string;
}): void {
  postMessage({ type: "startRiffCapture", ...payload });
}

export function stopRiffCapture(canceled = false): void {
  postMessage({ type: "stopRiffCapture", canceled });
}

export function saveRiffTake(payload: {
  riffId?: string;
  title: string;
  categories: string[];
  tags: string[];
  notes?: string;
  favorite?: boolean;
  tempoBpm?: number;
  timeSigNum?: number;
  timeSigDen?: number;
  bars?: number;
  patternType?: "click" | "drum";
  patternId?: string;
  presetId?: string;
}): void {
  postMessage({ type: "saveRiffTake", ...payload });
}

export function setRiffFavorite(riffId: string, favorite: boolean): void {
  postMessage({ type: "setRiffFavorite", riffId, favorite });
}

export function markRiffUsed(riffId: string, used: boolean, songTitle = ""): void {
  postMessage({ type: "markRiffUsed", riffId, used, songTitle });
}

export function deleteRiff(riffId: string): void {
  postMessage({ type: "deleteRiff", riffId });
}

export function previewRiffTake(takeId: string): void {
  postMessage({ type: "previewRiffTake", takeId });
}

export function loadRiffTakeForEdit(takeId: string): void {
  postMessage({ type: "loadRiffTakeForEdit", takeId });
}

export function previewCapturedRiff(): void {
  postMessage({ type: "previewCapturedRiff" });
}

export function previewCapturedRiffRange(startRatio: number, endRatio: number): void {
  postMessage({ type: "previewCapturedRiff", startRatio, endRatio });
}

export function stopPreviewPlayback(): void {
  postMessage({ type: "stopDemoAudio" });
}

export function importRiffWav(payload: {
  data: string;
  fileName?: string;
  tempoBpm: number;
  timeSigNum: number;
  timeSigDen: number;
  bars: number;
  patternType: "click" | "drum";
  patternId?: string;
}): void {
  postMessage({ type: "importRiffWav", ...payload });
}

export function trimCapturedRiff(startRatio: number, endRatio: number): void {
  postMessage({ type: "trimCapturedRiff", startRatio, endRatio });
}
