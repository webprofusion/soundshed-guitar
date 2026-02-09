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
    id,
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
