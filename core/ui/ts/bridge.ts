import type { EqSpectrumSource } from "./eqSpectrum.js";
import { appendLog } from "./logging.js";
import { setPresetDirty, uiState } from "./state.js";

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

/**
 * Tells the engine to switch its editing focus (the preset that signal-chain
 * edits apply to) to an already-active mixer slot, without touching the
 * running DSP instances. Call this alongside setFocusedMixerPresetId()
 * whenever the user switches which preset tab they're viewing/editing in a
 * multi-preset mixer session — otherwise node edits silently target whatever
 * preset the engine last considered "active" instead of the one on screen.
 * No-op when fewer than 2 presets are active in the mixer.
 */
export function focusMixerPreset(presetId: string): void {
  const mixer = uiState.mixer;
  if (!mixer || mixer.activePresetIds.length < 2 || !mixer.activePresetIds.includes(presetId)) {
    return;
  }
  postMessage({ type: "focusMixerPreset", presetId });
  appendLog(`focusMixerPreset → ${presetId}`);
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

// ── Edits the engine makes itself ─────────────────────────────────────────────
// Each names one change and the engine applies it to its own copy, replying with the
// result (docs/user-interface.md), so the web UI and Soundshed Guitar Nano cannot make
// the same edit two different ways.

/** Switches the active preset's scene; answered by "presetLoaded". */
export function selectScene(sceneId: string): void {
  postMessage({ type: "selectScene", sceneId });
  appendLog(`selectScene → ${sceneId}`);
}

/** Adds a scene copied from `fromSceneId` (the playing scene when omitted) and selects it. */
export function addScene(fromSceneId?: string | null): void {
  postMessage({ type: "addScene", ...(fromSceneId ? { fromSceneId } : {}) });
  appendLog("addScene");
}

/** Renames a scene; the engine trims the title, and an empty one becomes "Scene". */
export function renameScene(sceneId: string, title: string): void {
  postMessage({ type: "renameScene", sceneId, title });
}

/** Removes a scene; the engine refuses the last one with an "error". */
export function removeScene(sceneId: string): void {
  postMessage({ type: "removeScene", sceneId });
  appendLog(`removeScene → ${sceneId}`);
}

/** Marks or unmarks one preset as a favourite; answered by the whole "presetFavorites" list. */
export function sendPresetFavorite(presetId: string, favorite: boolean): void {
  postMessage({ type: "setPresetFavorite", presetId, favorite });
}

/** Rates one preset 1-5, or clears its rating with 0; answered by the whole "presetRatings" map. */
export function sendPresetRating(presetId: string, rating: number): void {
  postMessage({ type: "setPresetRating", presetId, rating });
}

/** Mutes or unmutes the output after the output gain; answered by "outputMutedChanged". */
export function setOutputMuted(muted: boolean): void {
  postMessage({ type: "setOutputMuted", muted });
  appendLog(`setOutputMuted → ${muted}`);
}

/** Marks or unmarks one library resource as a favourite; answered by "appSettingChanged". */
export function sendResourceFavorite(resourceId: string, favorite: boolean): void {
  postMessage({ type: "setResourceFavorite", resourceId, favorite });
}

/**
 * Asks the backend for a fresh signal-diagnostics roster.
 *
 * The roster is only sent when the node set changes, so a UI that loaded after it went
 * out — a reload, or a panel opened later in the session — can be holding none at all,
 * and then silently drops every level frame it receives. This is the way back: the
 * backend treats the message as "the UI is up and wants a roster" and re-sends one.
 */
export function requestSignalDiagnosticsRoster(): void {
  postMessage({ type: "setSignalDiagnosticsEnabled", enabled: true });
}

/** Starts, moves or renews the backend's spectrum tap (see eqSpectrum.ts); null stops it. */
export function sendSpectrumWatch(source: EqSpectrumSource | null): void {
  postMessage(source
    ? {
      type: "setSpectrumWatch",
      scope: source.scope,
      nodeId: source.nodeId,
      ...(source.presetId ? { presetId: source.presetId } : {}),
    }
    : { type: "setSpectrumWatch" });
}

/** Asks for an effect's response curve with these parameters; answered by "effectResponse"
 * with the same requestId (see effectResponse.ts). */
export function sendEffectResponseRequest(
  requestId: string,
  effectType: string,
  params: Record<string, number>,
  points: number,
): void {
  postMessage({ type: "getEffectResponse", requestId, effectType, params, points });
}

/** Renders an effect with these parameters as an IR into the library; answered by
 * "resourceImported" or "resourceImportFailed" carrying the same requestId. */
export function sendExportEffectAsIr(
  requestId: string,
  effectType: string,
  params: Record<string, number>,
  name: string,
): void {
  postMessage({ type: "exportEffectAsIr", requestId, effectType, params, name });
}

/** Asks for the Simple Cabinet settings that best match a library IR; answered by
 * "simpleCabIrMatch" with the same requestId. */
export function sendMatchSimpleCabToIr(requestId: string, resourceId: string): void {
  postMessage({ type: "matchSimpleCabToIr", requestId, resourceId });
}

/** One request to the standalone app's audio device settings (see settings/audioDevice.ts). */
export function sendAudioDeviceRequest(action: string, args: Record<string, unknown> = {}): void {
  postMessage({ type: "audioDevice", action, ...args });
}

/** The Multi-Rig's own level in dB, applied to the preset mix ahead of the global output stage. */
export function setMixGainDb(gainDb: number): void {
  postMessage({ type: "setMixGain", gainDb });
  appendLog(`setMixGain → ${gainDb.toFixed(1)} dB`);
}

export function setMetronome(payload: {
  bpm?: number;
  enabled?: boolean;
  volumeDb?: number;
  pan?: number;
  clickType?: string;
  beatPattern?: string;
  timeSigNum?: number;
  timeSigDen?: number;
  grouping?: string;
  subdivision?: string;
  clickConfig?: Array<{ id: string; label?: string; lowPath?: string; highPath?: string; subPath?: string }>;
}): void {
  postMessage({ type: "setMetronome", ...payload });
}

export function getRiffLibrary(): void {
  postMessage({ type: "getRiffLibrary" });
}

export function setRiffLibraryPath(path: string): void {
  postMessage({ type: "setRiffLibraryPath", path });
}

export function armRiffCapture(payload: {
  tempoBpm: number;
  timeSigNum: number;
  timeSigDen: number;
  bars: number;
  countInBars: number;
  metronomeClickEnabled?: boolean;
  patternType: "click" | "drum";
  patternId?: string;
  beatPattern?: string;
}): void {
  postMessage({ type: "armRiffCapture", ...payload });
}

export function startRiffCapture(payload: {
  tempoBpm: number;
  timeSigNum: number;
  timeSigDen: number;
  bars: number;
  countInBars: number;
  metronomeClickEnabled?: boolean;
  patternType: "click" | "drum";
  patternId?: string;
  beatPattern?: string;
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
  metronomeClickEnabled?: boolean;
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

export function previewRiffTake(takeId: string, enableGuidance = true): void {
  postMessage({ type: "previewRiffTake", takeId, enableGuidance });
}

/** Renders a demo clip (by `clipId`, from the engine's own list) or a riff take through the current preset. */
export function renderDemoAudio(payload: {
  clipId?: string;
  takeId?: string;
  title?: string;
  suggestedName?: string;
  renderSampleRate?: number;
}): void {
  postMessage({ type: "renderDemoAudio", ...payload });
}

export function loadRiffTakeForEdit(takeId: string): void {
  postMessage({ type: "loadRiffTakeForEdit", takeId });
}

export function previewCapturedRiff(): void {
  postMessage({ type: "previewCapturedRiff" });
}

/** Starts the captured take playing. The engine gets the whole take plus the
 * marker range, and does the looping itself — the UI does not re-request the
 * clip each time round, which is what used to put an audible gap at the seam. */
export function previewCapturedRiffRange(startRatio: number, endRatio: number, repeat: boolean): void {
  postMessage({ type: "previewCapturedRiff", startRatio, endRatio, repeat });
}

/** Retunes the range of the preview already playing — for a marker dragged
 * mid-preview, and for toggling Repeat without restarting. */
export function setRiffPreviewRegion(startRatio: number, endRatio: number, repeat: boolean): void {
  postMessage({ type: "setRiffPreviewRegion", startRatio, endRatio, repeat });
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
  bars?: number;
  patternType: "click" | "drum";
  patternId?: string;
}): void {
  postMessage({ type: "importRiffWav", ...payload });
}

export function trimCapturedRiff(startRatio: number, endRatio: number): void {
  postMessage({ type: "trimCapturedRiff", startRatio, endRatio });
}

export function requestAppInfo(): void {
  postMessage({ type: "getAppInfo" });
}

export function requestCaptureDebugSnapshot(source = "footer-button"): void {
  postMessage({ type: "captureDebugSnapshot", source });
  appendLog(`captureDebugSnapshot → ${source}`);
}

// ── Composite Presets (Multi-Rig) ─────────────────────────────────────────────

export function saveCompositePreset(name: string, description?: string, tags?: string[], id?: string): void {
  postMessage({
    type: "saveCompositePreset",
    name,
    description: description ?? "",
    tags: Array.isArray(tags) ? tags : [],
    ...(id ? { id } : {}),
  });
  appendLog(`saveCompositePreset → ${name}${id ? ` (updating ${id})` : ""}`);
}

export function loadCompositePreset(id: string): void {
  postMessage({ type: "loadCompositePreset", id });
  appendLog(`loadCompositePreset → ${id}`);
}

export function getCompositePresetList(): void {
  postMessage({ type: "getCompositePresetList" });
}

export function removeCompositePreset(id: string): void {
  postMessage({ type: "removeCompositePreset", id });
  appendLog(`removeCompositePreset → ${id}`);
}

// ── Practice Tool (Jam panel) ────────────────────────────────────────────
// The engine has no concept of a loop library — only the currently-active
// loop's bounds and whether looping is on. Loop add/rename/delete/select are
// pure UI state (see practiceTool.ts) and never round-trip through these.

export function browsePracticeToolFile(): void {
  postMessage({ type: "browsePracticeToolFile" });
  appendLog("browsePracticeToolFile");
}

export function loadPracticeToolFile(path: string): void {
  postMessage({ type: "loadPracticeToolFile", path });
  appendLog(`loadPracticeToolFile → ${path}`);
}

/** For a file dropped on the waveform: WebView2 never exposes a dropped
 * File's real path (that's Electron-only), so the caller reads its bytes
 * via file.arrayBuffer() and sends them here as base64 instead. */
export function loadPracticeToolFileData(fileName: string, dataBase64: string): void {
  postMessage({ type: "loadPracticeToolFileData", fileName, data: dataBase64 });
  appendLog(`loadPracticeToolFileData → ${fileName} (${dataBase64.length} b64 chars)`);
}

export function setPracticeToolTransport(action: "play" | "pause" | "stop"): void {
  postMessage({ type: "setPracticeToolTransport", action });
  appendLog(`setPracticeToolTransport → ${action}`);
}

export function seekPracticeToolFile(seconds: number): void {
  postMessage({ type: "seekPracticeToolFile", seconds });
}

export function setPracticeToolSpeed(ratio: number): void {
  postMessage({ type: "setPracticeToolSpeed", ratio });
}

export function setPracticeToolPitch(semitones: number): void {
  postMessage({ type: "setPracticeToolPitch", semitones });
}

export function setPracticeToolGain(gain: number): void {
  postMessage({ type: "setPracticeToolGain", gain });
}

export function setPracticeToolBalance(balance: number): void {
  postMessage({ type: "setPracticeToolBalance", balance });
}

/** Pass null (or omit bounds) to clear the active loop region — looping the whole track. */
export function setPracticeToolLoopRegion(region: { startSec: number; endSec: number } | null): void {
  if (region) {
    postMessage({ type: "setPracticeToolLoopRegion", startSec: region.startSec, endSec: region.endSec });
  } else {
    postMessage({ type: "setPracticeToolLoopRegion" });
  }
}

export function setPracticeToolLooping(enabled: boolean): void {
  postMessage({ type: "setPracticeToolLooping", enabled });
}

/**
 * Backing-track EQ. Both fields are optional and applied independently by the
 * engine, so this carries just the toggle, just the band being dragged, or the
 * whole curve when a project is recalled.
 */
export function setPracticeToolEq(update: { enabled?: boolean; params?: Record<string, number> }): void {
  postMessage({ type: "setPracticeToolEq", ...update });
}

/**
 * Sets one parameter on the global (always-on) signal chain — input gain,
 * gate, EQ, transpose, doubler and output.
 */
export function sendGlobalChainParam(paramPath: string, value: number | boolean): void {
  postMessage({
    type: "setGlobalChainParam",
    path: paramPath,
    value,
  });
}

/** Asks the plugin to send back the current global signal chain configuration. */
export function requestGlobalChainState(): void {
  postMessage({ type: "getGlobalChain" });
}
