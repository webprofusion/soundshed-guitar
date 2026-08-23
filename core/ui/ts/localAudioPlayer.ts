import {
  browseLocalAudioFile,
  loadLocalAudioFile,
  seekLocalAudioFile,
  setAppSetting,
  setLocalAudioGain,
  setLocalAudioLoopRegion,
  setLocalAudioLooping,
  setLocalAudioPitch,
  setLocalAudioSpeed,
  setLocalAudioTransport,
} from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { uiState } from "./state.js";
import type { LocalAudioLoopRegion, LocalAudioPlayerState } from "./types.js";
import { escapeHtml } from "./utils.js";

/** Small, static, non-user-editable set of common song-section names for the loop-naming quick-pick row. */
export const LOOP_NAME_TEMPLATES: readonly string[] = [
  "Intro",
  "Verse",
  "Pre-Chorus",
  "Chorus",
  "Bridge",
  "Solo",
  "Outro",
  "Turnaround",
  "Breakdown",
];

const LOCAL_AUDIO_LOOPS_SETTING = "localAudioPlayer.loops";
const MIN_LOOP_SPAN_SEC = 0.25;
const LOOP_REGION_SEND_DEBOUNCE_MS = 80;
const SPEED_PITCH_SEND_DEBOUNCE_MS = 80;
const DEFAULT_NEW_LOOP_LENGTH_SEC = 4;
// Below this many pixels of movement, a mousedown+mouseup on the waveform is
// treated as a plain click (seek) rather than the start of a new selection
// drag — otherwise the tiny jitter in every real click would constantly
// create zero-width "ranges".
const CLICK_DRAG_THRESHOLD_PX = 4;
// A mousedown within this many pixels of an existing handle grabs that
// handle; otherwise the click is free to become a seek instead. Without a
// threshold, EVERY click anywhere near a selection/active loop would yank
// the nearest edge to that position instead of ever allowing a seek.
const HANDLE_HIT_PX = 10;
// A resting hover (or a quick click/seek) should keep the normal pointer —
// the resize cursor only makes sense once the user is genuinely holding
// down for a drag. 150ms is long enough that a plain click/release never
// flashes it, short enough that a real drag still feels immediate.
const CURSOR_HOLD_MS = 150;
// How long the "Undo" affordance stays available after deleting a loop
// before the delete becomes permanent. Tune to taste — there's no dialog
// asking "are you sure?" any more, this window IS the confirmation.
const DELETE_UNDO_WINDOW_MS = 10_000;

type RatioRange = { startRatio: number; endRatio: number };
type SecondsRange = { startSec: number; endSec: number };
type PendingDeletedLoop = { loop: LocalAudioLoopRegion; index: number; timer: ReturnType<typeof setTimeout> };
// "pending": mouse is down but hasn't moved past the click/drag threshold
// yet, so it might still resolve to a plain seek-click on mouseup.
type DragMode = "handle" | "create" | "pending" | null;

let candidateRange: RatioRange | null = null;
let dragMode: DragMode = null;
let activeHandle: "start" | "end" = "start";
let selectedHandle: "start" | "end" = "start";
let dragAnchorRatio = 0;
let pointerDownClientX = 0;
let pointerDownClientY = 0;
let pointerDownRatio = 0;
// The loop currently showing inline-editable name/start/end fields in the
// list — covers both "just created, name it now" and "click the pencil on
// an existing row." There is no separate naming dialog/popover.
let editingLoopId: string | null = null;
// A just-deleted loop, kept around (out of player.loops but not forgotten)
// until DELETE_UNDO_WINDOW_MS elapses or another delete/undo/file-load
// supersedes it — only one pending delete is tracked at a time, matching
// common toast/snackbar UX (a second delete finalizes the first).
let pendingDeletedLoop: PendingDeletedLoop | null = null;

let loopRegionDebounceTimer: ReturnType<typeof setTimeout> | null = null;
let cursorHoldTimer: ReturnType<typeof setTimeout> | null = null;
let speedSendDebounceTimer: ReturnType<typeof setTimeout> | null = null;
let pitchSendDebounceTimer: ReturnType<typeof setTimeout> | null = null;

let playheadAnimFrame: number | null = null;
let playheadBaseSec = 0;
let playheadBaseMs = 0;
let playheadSpeed = 1;

/**
 * Builds a template-derived loop name with an auto-incrementing numeric suffix, e.g.
 * "Verse" -> "Verse 1" the first time it's picked on a track, "Verse 2" the next time,
 * regardless of whether the previous suggestion was actually kept. Pure/testable in
 * isolation from the DOM — see tests/localAudioLoopNaming.test.ts.
 */
export function suggestLoopTemplateName(baseName: string, existingNames: readonly string[]): string {
  const trimmedBase = baseName.trim();
  if (!trimmedBase) {
    return trimmedBase;
  }
  const suffixPattern = new RegExp(`^${trimmedBase.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")} (\\d+)$`);
  const usedNumbers = new Set<number>();
  for (const name of existingNames) {
    const match = suffixPattern.exec(name.trim());
    if (match) {
      usedNumbers.add(Number(match[1]));
    }
  }
  let next = 1;
  while (usedNumbers.has(next)) {
    next += 1;
  }
  return `${trimmedBase} ${next}`;
}

function generateLoopId(): string {
  return `loop-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}

function ensureLocalAudioPlayerState(): LocalAudioPlayerState {
  if (!uiState.localAudioPlayer) {
    uiState.localAudioPlayer = {
      filePath: "",
      title: "",
      durationSec: 0,
      positionSec: 0,
      waveformPeaksL: [],
      waveformPeaksR: [],
      loops: [],
      activeLoopId: null,
      looping: false,
      playing: false,
      speed: 1,
      pitchSemitones: 0,
      gain: 1,
    };
  }
  return uiState.localAudioPlayer;
}

function getActiveLoop(): LocalAudioLoopRegion | null {
  const player = ensureLocalAudioPlayerState();
  if (!player.activeLoopId) {
    return null;
  }
  return player.loops.find((loop) => loop.id === player.activeLoopId) ?? null;
}

function getFileFingerprint(filePath: string, durationSec: number): string {
  return `${filePath.trim().toLowerCase()}|${durationSec.toFixed(2)}`;
}

function isPersistedLoop(value: unknown): value is LocalAudioLoopRegion {
  if (!value || typeof value !== "object") {
    return false;
  }
  const record = value as Record<string, unknown>;
  return typeof record.id === "string"
    && typeof record.name === "string"
    && typeof record.startSec === "number"
    && typeof record.endSec === "number";
}

function loadLoopsForFingerprint(fingerprint: string): LocalAudioLoopRegion[] {
  if (!fingerprint) {
    return [];
  }
  const stored = uiState.appSettings?.[LOCAL_AUDIO_LOOPS_SETTING];
  if (!stored || typeof stored !== "object" || Array.isArray(stored)) {
    return [];
  }
  const entry = (stored as unknown as Record<string, unknown>)[fingerprint];
  if (!Array.isArray(entry)) {
    return [];
  }
  return entry.filter(isPersistedLoop).map((loop) => ({ ...loop }));
}

function persistLoopsForCurrentFile(): void {
  const player = ensureLocalAudioPlayerState();
  const fingerprint = getFileFingerprint(player.filePath, player.durationSec);
  if (!fingerprint) {
    return;
  }
  const stored = uiState.appSettings?.[LOCAL_AUDIO_LOOPS_SETTING];
  const map: Record<string, LocalAudioLoopRegion[]> = (stored && typeof stored === "object" && !Array.isArray(stored))
    ? { ...(stored as unknown as Record<string, LocalAudioLoopRegion[]>) }
    : {};
  if (player.loops.length > 0) {
    map[fingerprint] = player.loops.map((loop) => ({ ...loop }));
  } else {
    delete map[fingerprint];
  }
  uiState.appSettings[LOCAL_AUDIO_LOOPS_SETTING] = map as unknown as import("./types.js").AppSettingValue;
  setAppSetting(LOCAL_AUDIO_LOOPS_SETTING, map);
}

function formatClockTime(seconds: number): string {
  const total = Math.max(0, Math.floor(isFinite(seconds) ? seconds : 0));
  const mins = Math.floor(total / 60);
  const secs = total % 60;
  return `${mins}:${secs.toString().padStart(2, "0")}`;
}

function clampRatioRange(startRatio: number, endRatio: number, durationSec: number): RatioRange {
  const minSpanRatio = durationSec > 0 ? Math.min(0.25, MIN_LOOP_SPAN_SEC / durationSec) : 0.01;
  let start = Math.max(0, Math.min(1, startRatio));
  let end = Math.max(0, Math.min(1, endRatio));
  if (start > end) {
    [start, end] = [end, start];
  }
  if (end - start < minSpanRatio) {
    if (start + minSpanRatio <= 1) {
      end = start + minSpanRatio;
    } else {
      start = Math.max(0, 1 - minSpanRatio);
      end = 1;
    }
  }
  return { startRatio: start, endRatio: end };
}

function stopPlayheadAnim(): void {
  if (playheadAnimFrame !== null) {
    cancelAnimationFrame(playheadAnimFrame);
    playheadAnimFrame = null;
  }
}

function startPlayheadAnim(): void {
  if (playheadAnimFrame !== null) {
    return;
  }
  const step = () => {
    const player = ensureLocalAudioPlayerState();
    if (!player.playing) {
      playheadAnimFrame = null;
      return;
    }
    renderWaveform();
    playheadAnimFrame = requestAnimationFrame(step);
  };
  playheadAnimFrame = requestAnimationFrame(step);
}

function getInterpolatedPositionSec(): number {
  const player = ensureLocalAudioPlayerState();
  if (!player.playing) {
    return player.positionSec;
  }
  const elapsedSec = (performance.now() - playheadBaseMs) / 1000;
  const projected = playheadBaseSec + elapsedSec * playheadSpeed;
  return Math.max(0, Math.min(player.durationSec, projected));
}

function toNumberArray(value: unknown): number[] {
  return Array.isArray(value) ? value.filter((entry): entry is number => typeof entry === "number") : [];
}

/** Called on the `localAudioFileLoaded` engine message. */
export function applyLocalAudioFileLoaded(data: { path?: string; title?: string; durationSec?: number; waveformPeaksL?: unknown[]; waveformPeaksR?: unknown[] }): void {
  const player = ensureLocalAudioPlayerState();
  const filePath = typeof data.path === "string" ? data.path : "";
  const durationSec = typeof data.durationSec === "number" && isFinite(data.durationSec) ? Math.max(0, data.durationSec) : 0;
  const fallbackTitle = filePath ? (filePath.split(/[\\/]/).pop() ?? filePath) : "";

  player.filePath = filePath;
  player.title = typeof data.title === "string" && data.title.trim() ? data.title : fallbackTitle;
  player.durationSec = durationSec;
  player.positionSec = 0;
  player.playing = false;
  player.waveformPeaksL = toNumberArray(data.waveformPeaksL);
  player.waveformPeaksR = toNumberArray(data.waveformPeaksR);
  player.loops = loadLoopsForFingerprint(getFileFingerprint(filePath, durationSec));
  player.activeLoopId = null;

  candidateRange = null;
  editingLoopId = null;
  finalizePendingDelete(); // restoring into a different file's context wouldn't make sense
  playheadBaseSec = 0;
  playheadBaseMs = performance.now();
  playheadSpeed = player.speed;
  stopPlayheadAnim();

  appendLog(`local audio file loaded ← ${player.title} (${durationSec.toFixed(1)}s)`);
  renderLocalAudioPlayerPanel();
}

/** Called on the `localAudioTransportState` engine message. */
export function applyLocalAudioTransportState(data: { state?: string; positionSec?: number }): void {
  const player = ensureLocalAudioPlayerState();
  const playing = data.state === "playing";
  player.playing = playing;
  if (typeof data.positionSec === "number" && isFinite(data.positionSec)) {
    player.positionSec = Math.max(0, data.positionSec);
  }
  playheadBaseSec = player.positionSec;
  playheadBaseMs = performance.now();
  playheadSpeed = player.speed;

  if (playing) {
    startPlayheadAnim();
  } else {
    stopPlayheadAnim();
  }

  renderTransportControls();
  renderWaveform();
}

/** Called on the `localAudioPlaybackEnded` engine message. */
export function applyLocalAudioPlaybackEnded(): void {
  const player = ensureLocalAudioPlayerState();
  player.playing = false;
  player.positionSec = 0;
  playheadBaseSec = 0;
  playheadBaseMs = performance.now();
  stopPlayheadAnim();
  appendLog("local audio playback ended");
  renderTransportControls();
  renderWaveform();
}

function scheduleActiveLoopRegionSend(loop: LocalAudioLoopRegion): void {
  if (loopRegionDebounceTimer !== null) {
    clearTimeout(loopRegionDebounceTimer);
  }
  loopRegionDebounceTimer = setTimeout(() => {
    loopRegionDebounceTimer = null;
    setLocalAudioLoopRegion({ startSec: loop.startSec, endSec: loop.endSec });
  }, LOOP_REGION_SEND_DEBOUNCE_MS);
}

/** Debounces setLocalAudioSpeed/setLocalAudioPitch sends while a slider is
 * being dragged. Each of those calls flushes the native render-ahead ring
 * buffer (it must, to apply the new value promptly) — sending on every
 * `input` event during a drag would flush repeatedly and cause audible
 * stutter, which is exactly what un-throttled sends did before this existed.
 * `change` (fired once on release) still calls the flush* variant directly so
 * the final value is never lost to a pending timer. */
function scheduleSpeedSend(ratio: number): void {
  if (speedSendDebounceTimer !== null) {
    clearTimeout(speedSendDebounceTimer);
  }
  speedSendDebounceTimer = setTimeout(() => {
    speedSendDebounceTimer = null;
    setLocalAudioSpeed(ratio);
  }, SPEED_PITCH_SEND_DEBOUNCE_MS);
}

function flushSpeedSend(ratio: number): void {
  if (speedSendDebounceTimer !== null) {
    clearTimeout(speedSendDebounceTimer);
    speedSendDebounceTimer = null;
  }
  setLocalAudioSpeed(ratio);
}

function schedulePitchSend(semitones: number): void {
  if (pitchSendDebounceTimer !== null) {
    clearTimeout(pitchSendDebounceTimer);
  }
  pitchSendDebounceTimer = setTimeout(() => {
    pitchSendDebounceTimer = null;
    setLocalAudioPitch(semitones);
  }, SPEED_PITCH_SEND_DEBOUNCE_MS);
}

function flushPitchSend(semitones: number): void {
  if (pitchSendDebounceTimer !== null) {
    clearTimeout(pitchSendDebounceTimer);
    pitchSendDebounceTimer = null;
  }
  setLocalAudioPitch(semitones);
}

function flushActiveLoopRegionSend(loop: LocalAudioLoopRegion): void {
  if (loopRegionDebounceTimer !== null) {
    clearTimeout(loopRegionDebounceTimer);
    loopRegionDebounceTimer = null;
  }
  setLocalAudioLoopRegion({ startSec: loop.startSec, endSec: loop.endSec });
}

/** Applies a ratio-space range change to whichever thing is currently being edited:
 * the active loop's bounds (dragged/nudged, live-updates engine + local state, debounced),
 * or the pending candidate selection for a not-yet-saved loop. */
function applyRangeChange(startRatio: number, endRatio: number): void {
  const player = ensureLocalAudioPlayerState();
  const clamped = clampRatioRange(startRatio, endRatio, player.durationSec);
  const activeLoop = getActiveLoop();
  if (activeLoop) {
    activeLoop.startSec = clamped.startRatio * player.durationSec;
    activeLoop.endSec = clamped.endRatio * player.durationSec;
    scheduleActiveLoopRegionSend(activeLoop);
  } else {
    candidateRange = clamped;
  }
  renderWaveform();
  renderAddLoopAffordance();
}

function getCanvasRatioFromPointer(event: MouseEvent, canvas: HTMLCanvasElement): number {
  const rect = canvas.getBoundingClientRect();
  if (rect.width <= 0) {
    return 0;
  }
  return Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
}

function nudgeSelectedHandle(direction: -1 | 1, coarse = false): void {
  const player = ensureLocalAudioPlayerState();
  const activeLoop = getActiveLoop();
  const current = activeLoop
    ? { startRatio: activeLoop.startSec / Math.max(0.001, player.durationSec), endRatio: activeLoop.endSec / Math.max(0.001, player.durationSec) }
    : candidateRange;
  if (!current) {
    return;
  }
  const step = coarse ? 0.02 : 0.002;
  if (selectedHandle === "start") {
    applyRangeChange(current.startRatio + direction * step, current.endRatio);
  } else {
    applyRangeChange(current.startRatio, current.endRatio + direction * step);
  }
}

function renderWaveform(): void {
  const canvas = document.getElementById("local-audio-waveform") as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }
  const player = ensureLocalAudioPlayerState();
  const peaksL = player.waveformPeaksL;
  const peaksR = player.waveformPeaksR;
  const hasAudio = peaksL.length > 0 && peaksR.length > 0 && player.durationSec > 0;

  const dpr = window.devicePixelRatio || 1;
  const width = Math.max(1, Math.floor(canvas.clientWidth));
  const height = Math.max(1, Math.floor(canvas.clientHeight));
  canvas.width = Math.floor(width * dpr);
  canvas.height = Math.floor(height * dpr);

  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);

  ctx.fillStyle = "rgba(255,255,255,0.06)";
  ctx.fillRect(0, 0, width, height);

  // Divider between the L (top) and R (bottom) lanes.
  ctx.strokeStyle = "rgba(255,255,255,0.22)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, Math.floor(height / 2));
  ctx.lineTo(width, Math.floor(height / 2));
  ctx.stroke();

  if (!hasAudio) {
    ctx.fillStyle = "rgba(255,255,255,0.55)";
    ctx.font = "12px sans-serif";
    ctx.fillText("No file loaded", 10, Math.floor(height / 2) - 8);
    return;
  }

  // Two-lane stereo layout: L peaks centered in the top half, R peaks
  // centered in the bottom half — a cleaner, more honest picture of the
  // actual (genuinely stereo) audio than a single collapsed trace.
  const laneHeight = height / 2;
  const centerYL = laneHeight / 2;
  const centerYR = laneHeight + laneHeight / 2;
  const maxAmp = laneHeight / 2 - 4;

  // Faint per-lane center reference lines, fainter than the L/R divider.
  ctx.strokeStyle = "rgba(255,255,255,0.10)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, Math.floor(centerYL));
  ctx.lineTo(width, Math.floor(centerYL));
  ctx.moveTo(0, Math.floor(centerYR));
  ctx.lineTo(width, Math.floor(centerYR));
  ctx.stroke();

  const drawLane = (peaks: number[], centerY: number) => {
    const step = width / peaks.length;
    ctx.strokeStyle = "rgba(101, 186, 255, 0.95)";
    ctx.lineWidth = Math.max(1, step * 0.7);
    ctx.beginPath();
    peaks.forEach((peak, index) => {
      const x = index * step + step / 2;
      const amp = Math.max(1, Math.min(maxAmp, peak * maxAmp));
      ctx.moveTo(x, centerY - amp);
      ctx.lineTo(x, centerY + amp);
    });
    ctx.stroke();
  };
  drawLane(peaksL, centerYL);
  drawLane(peaksR, centerYR);

  const activeLoop = getActiveLoop();
  const range: RatioRange | null = activeLoop
    ? { startRatio: activeLoop.startSec / player.durationSec, endRatio: activeLoop.endSec / player.durationSec }
    : candidateRange;

  if (range) {
    const startX = Math.max(0, Math.min(width - 1, range.startRatio * width));
    const endX = Math.max(0, Math.min(width - 1, range.endRatio * width));

    ctx.fillStyle = activeLoop ? "rgba(255, 204, 102, 0.16)" : "rgba(101, 186, 255, 0.16)";
    ctx.fillRect(startX, 0, Math.max(1, endX - startX), height);

    ctx.strokeStyle = activeLoop ? "rgba(255, 204, 102, 0.95)" : "rgba(101, 186, 255, 0.95)";
    ctx.lineWidth = 2;
    ctx.setLineDash(activeLoop ? [] : [4, 3]);
    ctx.beginPath();
    ctx.moveTo(startX, 0);
    ctx.lineTo(startX, height);
    ctx.moveTo(endX, 0);
    ctx.lineTo(endX, height);
    ctx.stroke();
    ctx.setLineDash([]);

    // Handle markers sit on the L/R divider, spanning both lanes.
    const handleY = height / 2;
    ctx.fillStyle = ctx.strokeStyle as string;
    ctx.beginPath();
    ctx.arc(startX, handleY, 4, 0, Math.PI * 2);
    ctx.arc(endX, handleY, 4, 0, Math.PI * 2);
    ctx.fill();

    const selectedX = selectedHandle === "start" ? startX : endX;
    ctx.strokeStyle = "rgba(255,255,255,0.95)";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(selectedX, handleY, 6, 0, Math.PI * 2);
    ctx.stroke();
  }

  if (player.durationSec > 0) {
    const positionSec = getInterpolatedPositionSec();
    const playheadX = Math.max(0, Math.min(width, (positionSec / player.durationSec) * width));
    ctx.strokeStyle = "rgba(255, 255, 255, 0.85)";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(playheadX, 0);
    ctx.lineTo(playheadX, height);
    ctx.stroke();
  }
}

function renderAddLoopAffordance(): void {
  const btn = document.getElementById("local-audio-add-loop-btn") as HTMLButtonElement | null;
  const canvas = document.getElementById("local-audio-waveform") as HTMLCanvasElement | null;
  if (!btn || !canvas) {
    return;
  }
  const activeLoop = getActiveLoop();
  const show = !activeLoop && Boolean(candidateRange) && dragMode !== "create";
  btn.hidden = !show;
  if (show && candidateRange) {
    const width = canvas.clientWidth;
    const endX = Math.max(0, Math.min(width, candidateRange.endRatio * width));
    btn.style.left = `${Math.min(width - 8, endX + 8)}px`;
  }
}

function renderFileInfo(): void {
  const info = document.getElementById("local-audio-file-info");
  const browseBtn = document.getElementById("local-audio-browse-btn") as HTMLButtonElement | null;
  if (!info) {
    return;
  }
  const player = ensureLocalAudioPlayerState();
  if (!player.filePath || player.durationSec <= 0) {
    info.textContent = "No file loaded";
  } else {
    const position = formatClockTime(getInterpolatedPositionSec());
    const duration = formatClockTime(player.durationSec);
    info.textContent = `${player.title}   ${position} / ${duration}`;
  }
  if (browseBtn) {
    browseBtn.disabled = false;
  }
}

function renderTransportControls(): void {
  const player = ensureLocalAudioPlayerState();
  const hasAudio = player.durationSec > 0;

  const playPauseBtn = document.getElementById("local-audio-play-pause") as HTMLButtonElement | null;
  const stopBtn = document.getElementById("local-audio-stop") as HTMLButtonElement | null;
  const loopStatus = document.getElementById("local-audio-loop-status");
  const volumeSlider = document.getElementById("local-audio-volume") as HTMLInputElement | null;
  const volumeValue = document.getElementById("local-audio-volume-value");
  const speedSlider = document.getElementById("local-audio-speed") as HTMLInputElement | null;
  const speedValue = document.getElementById("local-audio-speed-value");
  const pitchSlider = document.getElementById("local-audio-pitch") as HTMLInputElement | null;
  const pitchValue = document.getElementById("local-audio-pitch-value");

  if (playPauseBtn) {
    playPauseBtn.disabled = !hasAudio;
    playPauseBtn.textContent = player.playing ? "⏸" : "▶";
    playPauseBtn.setAttribute("aria-label", player.playing ? "Pause" : "Play");
    playPauseBtn.title = player.playing ? "Pause" : "Play";
  }
  if (stopBtn) {
    stopBtn.disabled = !hasAudio;
  }
  if (loopStatus) {
    const activeLoop = getActiveLoop();
    loopStatus.hidden = !hasAudio || !activeLoop;
    if (activeLoop) {
      loopStatus.textContent = `Looping "${activeLoop.name}"`;
    }
  }
  if (volumeSlider && document.activeElement !== volumeSlider) {
    volumeSlider.value = String(player.gain);
  }
  if (volumeValue) {
    volumeValue.textContent = `${Math.round(player.gain * 100)}%`;
  }
  if (speedSlider && document.activeElement !== speedSlider) {
    speedSlider.value = String(player.speed);
  }
  if (speedValue) {
    speedValue.textContent = `${Math.round(player.speed * 100)}%`;
  }
  if (pitchSlider && document.activeElement !== pitchSlider) {
    pitchSlider.value = String(player.pitchSemitones);
  }
  if (pitchValue) {
    const semis = player.pitchSemitones;
    pitchValue.textContent = `${semis > 0 ? "+" : ""}${semis} st`;
  }

  renderFileInfo();
}

/** Populates the shared <datalist> once with the song-section templates —
 * every name/rename input references it via list=, so typing or picking a
 * suggestion works the same whether you're naming a brand new loop or
 * renaming an existing one. */
function ensureLoopNameTemplatesDatalist(): void {
  const datalist = document.getElementById("local-audio-loop-name-templates");
  if (!datalist || datalist.childElementCount > 0) {
    return;
  }
  datalist.innerHTML = LOOP_NAME_TEMPLATES
    .map((template) => `<option value="${escapeHtml(template)}"></option>`)
    .join("");
}

function renderLoopList(): void {
  const list = document.getElementById("local-audio-loop-list");
  if (!list) {
    return;
  }
  const player = ensureLocalAudioPlayerState();

  // No confirmation dialog on delete — this banner (shown until the undo
  // window elapses, another delete supersedes it, or undo is clicked) IS
  // the confirmation, just reversible instead of blocking.
  const undoBannerHtml = pendingDeletedLoop
    ? `
        <div class="local-audio-loop-undo-banner">
          <span>Deleted "${escapeHtml(pendingDeletedLoop.loop.name)}".</span>
          <button type="button" class="local-audio-loop-undo-btn">Undo</button>
        </div>
      `
    : "";

  if (!player.loops.length) {
    list.innerHTML = `${undoBannerHtml}<div class="equipment-library-empty">No loops saved for this file yet.</div>`;
    return;
  }

  const maxSec = player.durationSec.toFixed(2);

  list.innerHTML = undoBannerHtml + player.loops
    .map((loop) => {
      const isActive = loop.id === player.activeLoopId;
      const isEditing = loop.id === editingLoopId;
      const rowMainHtml = isEditing
        ? `
            <input type="text" class="local-audio-loop-editable local-audio-loop-name-input" data-loop-id="${loop.id}" data-field="name" list="local-audio-loop-name-templates" placeholder="Loop name" value="${escapeHtml(loop.name)}" />
            <input type="number" class="local-audio-loop-editable local-audio-loop-time-input" data-loop-id="${loop.id}" data-field="start" min="0" max="${maxSec}" step="0.01" value="${loop.startSec.toFixed(2)}" aria-label="Start time in seconds" />
            <span class="local-audio-loop-time-sep">–</span>
            <input type="number" class="local-audio-loop-editable local-audio-loop-time-input" data-loop-id="${loop.id}" data-field="end" min="0" max="${maxSec}" step="0.01" value="${loop.endSec.toFixed(2)}" aria-label="End time in seconds" />
            <span class="local-audio-loop-time-unit">s</span>
          `
        : `
            <span class="local-audio-loop-name">${escapeHtml(loop.name)}</span>
            <span class="local-audio-loop-range">${formatClockTime(loop.startSec)}–${formatClockTime(loop.endSec)}</span>
          `;
      return `
        <div class="local-audio-loop-row${isActive ? " is-active" : ""}${isEditing ? " is-editing" : ""}" data-loop-id="${loop.id}">
          <button type="button" class="local-audio-loop-select-btn" data-loop-id="${loop.id}" aria-pressed="${isActive}" title="${isActive ? "Active loop — click to deactivate" : "Select loop"}">${isActive ? "●" : "○"}</button>
          <div class="local-audio-loop-row-main" data-loop-id="${loop.id}">
            ${rowMainHtml}
          </div>
          <div class="local-audio-loop-row-actions">
            ${isEditing ? "" : `<button type="button" class="local-audio-loop-rename-btn" data-loop-id="${loop.id}" title="Edit name/time" aria-label="Edit loop name and time">✎</button>`}
            <button type="button" class="local-audio-loop-delete-btn" data-loop-id="${loop.id}" title="Delete" aria-label="Delete loop">✕</button>
          </div>
        </div>
      `;
    })
    .join("");

  if (editingLoopId) {
    const nameInput = list.querySelector<HTMLInputElement>(`.local-audio-loop-name-input[data-loop-id="${editingLoopId}"]`);
    nameInput?.focus();
    nameInput?.select();
  }
}

function selectLoop(loopId: string): void {
  const player = ensureLocalAudioPlayerState();
  const loop = player.loops.find((entry) => entry.id === loopId);
  if (!loop) {
    return;
  }

  if (player.activeLoopId === loopId) {
    // Clicking the already-active loop deactivates it — this is the only
    // "unselect"/stop-looping affordance; there is no separate Loop toggle,
    // since looping is implied entirely by whether a loop is selected.
    player.activeLoopId = null;
    player.looping = false;
    setLocalAudioLoopRegion(null);
    setLocalAudioLooping(false);
    appendLog(`local audio loop deactivated → ${loop.name}`);
    renderLocalAudioPlayerPanel();
    return;
  }

  player.activeLoopId = loopId;
  player.looping = true;
  candidateRange = null;
  editingLoopId = null;
  selectedHandle = "start";
  seekLocalAudioFile(loop.startSec);
  setLocalAudioLoopRegion({ startSec: loop.startSec, endSec: loop.endSec });
  setLocalAudioLooping(true);
  appendLog(`local audio loop selected → ${loop.name} (${loop.startSec.toFixed(2)}-${loop.endSec.toFixed(2)}s)`);
  renderLocalAudioPlayerPanel();
}

/** Finalizes whatever delete is currently pending (if any) — the undo
 * window is over, nothing more to do since the loop was already removed
 * from player.loops at delete time. Called when a new delete supersedes an
 * old one, when undo is invoked, when a new file loads, and when the
 * window's own timer elapses. */
function finalizePendingDelete(): void {
  if (!pendingDeletedLoop) {
    return;
  }
  clearTimeout(pendingDeletedLoop.timer);
  pendingDeletedLoop = null;
}

function undoDeleteLoop(): void {
  if (!pendingDeletedLoop) {
    return;
  }
  const { loop, index } = pendingDeletedLoop;
  clearTimeout(pendingDeletedLoop.timer);
  pendingDeletedLoop = null;

  const player = ensureLocalAudioPlayerState();
  const insertAt = Math.min(index, player.loops.length);
  player.loops = [...player.loops.slice(0, insertAt), loop, ...player.loops.slice(insertAt)];
  persistLoopsForCurrentFile();
  appendLog(`local audio loop delete undone → ${loop.name}`);
  renderLocalAudioPlayerPanel();
}

/** Deletes immediately — no confirmation dialog — and instead leaves the
 * loop restorable via an inline "Undo" affordance for DELETE_UNDO_WINDOW_MS.
 * The delete is real (removed from player.loops, engine loop region cleared
 * if it was active) the instant this runs; undo re-inserts it rather than
 * "cancelling" anything in flight. */
function deleteLoop(loopId: string): void {
  const player = ensureLocalAudioPlayerState();
  const index = player.loops.findIndex((entry) => entry.id === loopId);
  if (index === -1) {
    return;
  }
  const loop = player.loops[index];

  finalizePendingDelete(); // only one undo slot at a time

  player.loops = player.loops.filter((entry) => entry.id !== loopId);
  if (player.activeLoopId === loopId) {
    player.activeLoopId = null;
    player.looping = false;
    setLocalAudioLoopRegion(null);
    setLocalAudioLooping(false);
  }
  if (editingLoopId === loopId) {
    editingLoopId = null;
  }
  persistLoopsForCurrentFile();
  appendLog(`local audio loop deleted → ${loop.name} (undo available for ${Math.round(DELETE_UNDO_WINDOW_MS / 1000)}s)`);

  pendingDeletedLoop = {
    loop,
    index,
    timer: setTimeout(() => {
      pendingDeletedLoop = null;
      renderLoopList();
    }, DELETE_UNDO_WINDOW_MS),
  };

  renderLocalAudioPlayerPanel();
}

/** Commits the name field only — does not touch editingLoopId, since the
 * user may still be tabbing on to the start/end fields in the same row
 * (see the focusout handler in bindLoopListActions for when editing mode
 * actually ends). Does not re-render the list, to avoid destroying the
 * user's in-progress Tab navigation between this row's fields. */
function commitEditLoopName(loopId: string, rawName: string): void {
  const player = ensureLocalAudioPlayerState();
  const loop = player.loops.find((entry) => entry.id === loopId);
  const name = rawName.trim();
  if (loop && name && name !== loop.name) {
    loop.name = name;
    persistLoopsForCurrentFile();
    if (player.activeLoopId === loopId) {
      renderTransportControls(); // updates the "Looping <name>" status label
    }
  }
}

/** Commits one time field (start or end). Clamped to stay a valid,
 * non-inverted, at-least-MIN_LOOP_SPAN_SEC region. Live-updates the engine
 * and the waveform highlight immediately if this loop is active; does not
 * re-render the list itself, for the same Tab-navigation reason as above. */
function commitEditLoopTime(loopId: string, field: "start" | "end", rawValue: string): void {
  const player = ensureLocalAudioPlayerState();
  const loop = player.loops.find((entry) => entry.id === loopId);
  if (!loop) {
    return;
  }
  const parsed = parseFloat(rawValue);
  if (!isFinite(parsed)) {
    return; // leave the loop's data untouched; the input still shows what the user typed
  }
  const clamped = Math.max(0, Math.min(player.durationSec, parsed));
  if (field === "start") {
    loop.startSec = Math.min(clamped, Math.max(0, loop.endSec - MIN_LOOP_SPAN_SEC));
  } else {
    loop.endSec = Math.max(clamped, Math.min(player.durationSec, loop.startSec + MIN_LOOP_SPAN_SEC));
  }
  persistLoopsForCurrentFile();
  if (player.activeLoopId === loopId) {
    setLocalAudioLoopRegion({ startSec: loop.startSec, endSec: loop.endSec });
  }
  renderWaveform();
  renderAddLoopAffordance();
}

/** Ends editing mode for whichever loop is currently being edited (if any)
 * and re-renders the list to show its final committed values. Safe to call
 * even when nothing is being edited. */
function finishEditingLoop(): void {
  if (!editingLoopId) {
    return;
  }
  editingLoopId = null;
  renderLoopList();
}

function suggestDefaultLoopName(existingLoops: readonly LocalAudioLoopRegion[]): string {
  const existingNames = existingLoops.map((loop) => loop.name);
  let n = existingLoops.length + 1;
  let candidate = `New Loop ${n}`;
  while (existingNames.includes(candidate)) {
    n += 1;
    candidate = `New Loop ${n}`;
  }
  return candidate;
}

/** Creates a loop from a start/end range, adds it straight to the list
 * (auto-selected + looping, per the plan's "select implies loop" model),
 * and immediately opens it for inline name/time editing — there is no
 * separate naming dialog. */
function createLoopFromRange(range: SecondsRange): void {
  const player = ensureLocalAudioPlayerState();
  const newLoop: LocalAudioLoopRegion = {
    id: generateLoopId(),
    name: suggestDefaultLoopName(player.loops),
    startSec: range.startSec,
    endSec: range.endSec,
  };
  player.loops = [...player.loops, newLoop];
  player.activeLoopId = newLoop.id;
  player.looping = true;
  editingLoopId = newLoop.id;
  candidateRange = null;
  persistLoopsForCurrentFile();
  seekLocalAudioFile(newLoop.startSec);
  setLocalAudioLoopRegion({ startSec: newLoop.startSec, endSec: newLoop.endSec });
  setLocalAudioLooping(true);
  appendLog(`local audio loop created → ${newLoop.name} (${newLoop.startSec.toFixed(2)}-${newLoop.endSec.toFixed(2)}s)`);
  renderLocalAudioPlayerPanel();
}

function addNewLoop(): void {
  const player = ensureLocalAudioPlayerState();
  if (player.durationSec <= 0) {
    showNotification("Load an audio file first");
    return;
  }
  if (candidateRange) {
    createLoopFromRange({
      startSec: candidateRange.startRatio * player.durationSec,
      endSec: candidateRange.endRatio * player.durationSec,
    });
    return;
  }

  const start = getInterpolatedPositionSec();
  const end = Math.min(player.durationSec, start + DEFAULT_NEW_LOOP_LENGTH_SEC);
  const clampedStart = end - start < MIN_LOOP_SPAN_SEC ? Math.max(0, end - MIN_LOOP_SPAN_SEC) : start;
  createLoopFromRange({ startSec: clampedStart, endSec: end });
}

function bindWaveformInteractions(): void {
  const canvas = document.getElementById("local-audio-waveform") as HTMLCanvasElement | null;
  if (!canvas || canvas.dataset.bound === "true") {
    return;
  }
  canvas.dataset.bound = "true";

  canvas.addEventListener("mousedown", (event) => {
    const player = ensureLocalAudioPlayerState();
    if (player.durationSec <= 0) {
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const pointerRatio = getCanvasRatioFromPointer(event, canvas);
    const activeLoop = getActiveLoop();
    const range: RatioRange | null = activeLoop
      ? { startRatio: activeLoop.startSec / player.durationSec, endRatio: activeLoop.endSec / player.durationSec }
      : candidateRange;

    pointerDownClientX = event.clientX;
    pointerDownClientY = event.clientY;
    pointerDownRatio = pointerRatio;

    // Only show the resize cursor once the hold has lasted long enough to
    // be a deliberate drag, not on a resting hover or a quick click/seek.
    if (cursorHoldTimer !== null) {
      clearTimeout(cursorHoldTimer);
    }
    cursorHoldTimer = setTimeout(() => {
      cursorHoldTimer = null;
      canvas.classList.add("is-resizing");
    }, CURSOR_HOLD_MS);

    // Only grab a handle if the click actually landed near one — otherwise
    // every click anywhere on the waveform would yank the nearest edge of
    // whatever selection/loop is active, making it impossible to ever seek
    // and making the selection feel like it's constantly slipping.
    const handleHitRatio = rect.width > 0 ? HANDLE_HIT_PX / rect.width : 0;
    if (range) {
      const startDist = Math.abs(pointerRatio - range.startRatio);
      const endDist = Math.abs(pointerRatio - range.endRatio);
      if (Math.min(startDist, endDist) <= handleHitRatio) {
        dragMode = "handle";
        activeHandle = startDist <= endDist ? "start" : "end";
        selectedHandle = activeHandle;
        canvas.focus();
        event.preventDefault();
        renderWaveform();
        renderAddLoopAffordance();
        return;
      }
    }

    // Not near a handle: defer the decision. A plain click (no meaningful
    // movement before mouseup) becomes a seek; movement past the threshold
    // promotes this to a new range-selection drag (see mousemove below).
    dragMode = "pending";
    dragAnchorRatio = pointerRatio;
    canvas.focus();
    event.preventDefault();
  });

  window.addEventListener("mousemove", (event) => {
    if (!dragMode) {
      return;
    }
    const player = ensureLocalAudioPlayerState();
    if (player.durationSec <= 0) {
      return;
    }

    if (dragMode === "pending") {
      const dx = event.clientX - pointerDownClientX;
      const dy = event.clientY - pointerDownClientY;
      if (Math.hypot(dx, dy) < CLICK_DRAG_THRESHOLD_PX) {
        return;
      }
      dragMode = "create";
      candidateRange = { startRatio: dragAnchorRatio, endRatio: dragAnchorRatio };
      finishEditingLoop();
    }

    const pointerRatio = getCanvasRatioFromPointer(event, canvas);

    if (dragMode === "create") {
      candidateRange = {
        startRatio: Math.min(dragAnchorRatio, pointerRatio),
        endRatio: Math.max(dragAnchorRatio, pointerRatio),
      };
      renderWaveform();
      return;
    }

    const activeLoop = getActiveLoop();
    const range: RatioRange | null = activeLoop
      ? { startRatio: activeLoop.startSec / player.durationSec, endRatio: activeLoop.endSec / player.durationSec }
      : candidateRange;
    if (!range) {
      return;
    }
    if (activeHandle === "start") {
      applyRangeChange(pointerRatio, range.endRatio);
    } else {
      applyRangeChange(range.startRatio, pointerRatio);
    }
  });

  window.addEventListener("mouseup", () => {
    if (cursorHoldTimer !== null) {
      clearTimeout(cursorHoldTimer);
      cursorHoldTimer = null;
    }
    canvas.classList.remove("is-resizing");

    if (dragMode === "pending") {
      // Never moved past the click/drag threshold: a plain seek click.
      const player = ensureLocalAudioPlayerState();
      if (player.durationSec > 0) {
        seekLocalAudioFile(pointerDownRatio * player.durationSec);
      }
    } else if (dragMode === "handle") {
      const activeLoop = getActiveLoop();
      if (activeLoop) {
        flushActiveLoopRegionSend(activeLoop);
        persistLoopsForCurrentFile();
      }
    }
    dragMode = null;
    renderWaveform();
    renderAddLoopAffordance();
  });

  canvas.addEventListener("keydown", (event) => {
    const player = ensureLocalAudioPlayerState();
    if (player.durationSec <= 0) {
      return;
    }
    if (event.key === "ArrowLeft") {
      event.preventDefault();
      nudgeSelectedHandle(-1, event.shiftKey);
      return;
    }
    if (event.key === "ArrowRight") {
      event.preventDefault();
      nudgeSelectedHandle(1, event.shiftKey);
      return;
    }
    if (event.key === "ArrowUp" || event.key === "ArrowDown") {
      event.preventDefault();
      selectedHandle = selectedHandle === "start" ? "end" : "start";
      renderWaveform();
    }
  });
}

function bindTransportControls(): void {
  const browseBtn = document.getElementById("local-audio-browse-btn") as HTMLButtonElement | null;
  const playPauseBtn = document.getElementById("local-audio-play-pause") as HTMLButtonElement | null;
  const stopBtn = document.getElementById("local-audio-stop") as HTMLButtonElement | null;
  const volumeSlider = document.getElementById("local-audio-volume") as HTMLInputElement | null;
  const speedSlider = document.getElementById("local-audio-speed") as HTMLInputElement | null;
  const pitchSlider = document.getElementById("local-audio-pitch") as HTMLInputElement | null;

  if (browseBtn && browseBtn.dataset.bound !== "true") {
    browseBtn.dataset.bound = "true";
    browseBtn.addEventListener("click", () => {
      browseLocalAudioFile();
    });
  }

  if (playPauseBtn && playPauseBtn.dataset.bound !== "true") {
    playPauseBtn.dataset.bound = "true";
    playPauseBtn.addEventListener("click", () => {
      const player = ensureLocalAudioPlayerState();
      if (player.durationSec <= 0) {
        return;
      }
      setLocalAudioTransport(player.playing ? "pause" : "play");
    });
  }

  if (stopBtn && stopBtn.dataset.bound !== "true") {
    stopBtn.dataset.bound = "true";
    stopBtn.addEventListener("click", () => {
      setLocalAudioTransport("stop");
    });
  }

  if (volumeSlider && volumeSlider.dataset.bound !== "true") {
    volumeSlider.dataset.bound = "true";
    volumeSlider.addEventListener("input", () => {
      const gain = parseFloat(volumeSlider.value);
      if (!isFinite(gain)) {
        return;
      }
      const player = ensureLocalAudioPlayerState();
      player.gain = gain;
      renderTransportControls();
      setLocalAudioGain(gain);
    });
  }

  if (speedSlider && speedSlider.dataset.bound !== "true") {
    speedSlider.dataset.bound = "true";
    speedSlider.addEventListener("input", () => {
      const ratio = parseFloat(speedSlider.value);
      if (!isFinite(ratio)) {
        return;
      }
      const player = ensureLocalAudioPlayerState();
      player.speed = ratio;
      playheadSpeed = ratio;
      playheadBaseSec = getInterpolatedPositionSec();
      playheadBaseMs = performance.now();
      renderTransportControls();
      scheduleSpeedSend(ratio);
    });
    speedSlider.addEventListener("change", () => {
      const ratio = parseFloat(speedSlider.value);
      if (isFinite(ratio)) {
        flushSpeedSend(ratio);
      }
    });
  }

  if (pitchSlider && pitchSlider.dataset.bound !== "true") {
    pitchSlider.dataset.bound = "true";
    pitchSlider.addEventListener("input", () => {
      const semis = parseFloat(pitchSlider.value);
      if (!isFinite(semis)) {
        return;
      }
      const player = ensureLocalAudioPlayerState();
      player.pitchSemitones = semis;
      renderTransportControls();
      schedulePitchSend(semis);
    });
    pitchSlider.addEventListener("change", () => {
      const semis = parseFloat(pitchSlider.value);
      if (isFinite(semis)) {
        flushPitchSend(semis);
      }
    });
  }
}

/** Commits whichever editable field (name/start/end) `input` represents.
 * Shared by the Enter-key handler and the focusout handler below so the two
 * can't drift out of sync on which field maps to which commit function. */
function commitEditableField(input: HTMLInputElement): void {
  const loopId = input.dataset.loopId ?? "";
  const field = input.dataset.field;
  if (!loopId || !field) {
    return;
  }
  if (field === "name") {
    commitEditLoopName(loopId, input.value);
  } else if (field === "start" || field === "end") {
    commitEditLoopTime(loopId, field, input.value);
  }
}

function bindLoopListActions(): void {
  const list = document.getElementById("local-audio-loop-list");
  if (list && list.dataset.bound !== "true") {
    list.dataset.bound = "true";
    list.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      if (!target) {
        return;
      }
      if (target.closest(".local-audio-loop-undo-btn")) {
        undoDeleteLoop();
        return;
      }
      const selectBtn = target.closest<HTMLButtonElement>(".local-audio-loop-select-btn");
      if (selectBtn) {
        const loopId = selectBtn.dataset.loopId ?? "";
        if (loopId) {
          selectLoop(loopId);
        }
        return;
      }
      const renameBtn = target.closest<HTMLButtonElement>(".local-audio-loop-rename-btn");
      if (renameBtn) {
        editingLoopId = renameBtn.dataset.loopId ?? null;
        renderLoopList();
        return;
      }
      const deleteBtn = target.closest<HTMLButtonElement>(".local-audio-loop-delete-btn");
      if (deleteBtn) {
        const loopId = deleteBtn.dataset.loopId ?? "";
        if (loopId) {
          deleteLoop(loopId);
        }
        return;
      }
      const rowMain = target.closest<HTMLElement>(".local-audio-loop-row-main");
      if (rowMain && !target.closest(".local-audio-loop-editable")) {
        const loopId = rowMain.dataset.loopId ?? "";
        if (loopId && loopId !== editingLoopId) {
          selectLoop(loopId);
        }
      }
    });

    // Picking a bare template name from the datalist (or typing one exactly)
    // auto-suffixes a number, the same "Verse" -> "Verse 1" behavior the old
    // template-button row had — just triggered by the native suggestion
    // dropdown instead of a separate row of buttons.
    list.addEventListener("input", (event) => {
      const target = event.target as HTMLElement | null;
      const nameInput = target?.closest<HTMLInputElement>(".local-audio-loop-name-input");
      if (!nameInput || !LOOP_NAME_TEMPLATES.includes(nameInput.value)) {
        return;
      }
      const player = ensureLocalAudioPlayerState();
      const otherNames = player.loops
        .filter((loop) => loop.id !== nameInput.dataset.loopId)
        .map((loop) => loop.name);
      nameInput.value = suggestLoopTemplateName(nameInput.value, otherNames);
    });

    list.addEventListener("keydown", (event) => {
      const target = event.target as HTMLElement | null;
      const input = target?.closest<HTMLInputElement>(".local-audio-loop-editable");
      if (!input) {
        return;
      }
      if (event.key === "Enter") {
        // Enter doesn't move focus anywhere, so the focusout it triggers
        // (via blur below) will correctly see no relatedTarget and end
        // editing — matches "Enter finishes editing this loop."
        input.blur();
      } else if (event.key === "Escape") {
        finishEditingLoop();
      }
    });

    list.addEventListener("focusout", (event) => {
      const focusEvent = event as FocusEvent;
      const target = focusEvent.target as HTMLElement | null;
      const input = target?.closest<HTMLInputElement>(".local-audio-loop-editable");
      if (!input) {
        return;
      }
      commitEditableField(input);

      // Only end editing mode (and re-render) once focus actually leaves
      // this loop's row — e.g. Tab moving from the name field to the start-
      // time field must NOT re-render mid-tab, or the Tab destination would
      // vanish before the browser gets to focus it.
      const row = input.closest(".local-audio-loop-row");
      const nextFocus = focusEvent.relatedTarget;
      const staysInRow = row && nextFocus instanceof Node && row.contains(nextFocus);
      if (!staysInRow) {
        finishEditingLoop();
      }
    });
  }

  const newLoopBtn = document.getElementById("local-audio-new-loop-btn") as HTMLButtonElement | null;
  if (newLoopBtn && newLoopBtn.dataset.bound !== "true") {
    newLoopBtn.dataset.bound = "true";
    newLoopBtn.addEventListener("click", () => addNewLoop());
  }

  const addLoopBtn = document.getElementById("local-audio-add-loop-btn") as HTMLButtonElement | null;
  if (addLoopBtn && addLoopBtn.dataset.bound !== "true") {
    addLoopBtn.dataset.bound = "true";
    addLoopBtn.addEventListener("click", () => addNewLoop());
  }
}

function readDroppedFilePath(file: File): string | null {
  const withPath = file as File & { path?: string };
  return typeof withPath.path === "string" && withPath.path ? withPath.path : null;
}

function bindDropZone(): void {
  const dropZone = document.getElementById("local-audio-drop-zone");
  if (!dropZone || dropZone.dataset.bound === "true") {
    return;
  }
  dropZone.dataset.bound = "true";

  dropZone.addEventListener("dragover", (event) => {
    event.preventDefault();
    dropZone.classList.add("is-drag-over");
  });
  dropZone.addEventListener("dragleave", () => {
    dropZone.classList.remove("is-drag-over");
  });
  dropZone.addEventListener("drop", (event) => {
    event.preventDefault();
    dropZone.classList.remove("is-drag-over");
    const file = event.dataTransfer?.files?.[0];
    if (!file) {
      return;
    }
    const path = readDroppedFilePath(file);
    if (!path) {
      showNotification("Drag-and-drop isn't supported for this file", "Use Browse File... instead");
      return;
    }
    loadLocalAudioFile(path);
    appendLog(`local audio load requested (drop) → ${path}`);
  });
}

function bindAllActions(): void {
  bindWaveformInteractions();
  bindTransportControls();
  bindLoopListActions();
  bindDropZone();
}

export function renderLocalAudioPlayerPanel(): void {
  ensureLoopNameTemplatesDatalist();
  renderFileInfo();
  renderTransportControls();
  renderWaveform();
  renderAddLoopAffordance();
  renderLoopList();
  bindAllActions();
}

export function initializeLocalAudioPlayerPanel(): void {
  bindAllActions();
  renderLocalAudioPlayerPanel();
}
