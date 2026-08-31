import {
  browsePracticeToolFile,
  seekPracticeToolFile,
  setPracticeToolBalance,
  setPracticeToolGain,
  setPracticeToolLoopRegion,
  setPracticeToolLooping,
  setPracticeToolPitch,
  setPracticeToolSpeed,
  setPracticeToolTransport,
} from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { uiState } from "./state.js";
import type { PracticeToolLoopRegion, PracticeToolProject, PracticeToolState } from "./types.js";
import { escapeHtml } from "./utils.js";
import type { RangeHandle, RatioRange } from "./waveform/range.js";
import { bindRangeSelect, type RangeSelectController } from "./waveform/rangeSelect.js";
import { drawWaveform } from "./waveform/render.js";
import {
  consumePendingProjectRecall,
  getFileFingerprint,
  loadLoopsForFingerprint,
  persistLoopsForCurrentFile,
  setPracticeToolProjectApplier,
} from "./practiceTool/projects.js";
import { bindPracticeToolProjectActions, renderPracticeToolProjects } from "./practiceTool/projectsPanel.js";
import { bindPracticeToolDropZone, confirmResetIfNeeded } from "./practiceTool/trackImport.js";
import { createDefaultPracticeToolEq, isPracticeToolEqShaping, sanitizePracticeToolEq } from "./practiceTool/eq.js";
import { pushPracticeToolEqToEngine } from "./practiceTool/eqSend.js";
import {
  bindPracticeToolEqModal,
  openPracticeToolEqModal,
  renderPracticeToolEqModal,
  setPracticeToolEqChangeListener,
} from "./practiceTool/eqModal.js";

/** Registered by main.ts, which can reach the preset library without closing an
 * import cycle back through this module. */
export { setPracticeToolPresetRecaller } from "./practiceTool/projects.js";

/** Small, static, non-user-editable set of common song-section names, offered as
 * `<datalist>` suggestions on every loop name/rename field. */
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

const MIN_LOOP_SPAN_SEC = 0.25;
const LOOP_REGION_SEND_DEBOUNCE_MS = 80;
const SPEED_PITCH_SEND_DEBOUNCE_MS = 80;
const DEFAULT_NEW_LOOP_LENGTH_SEC = 4;
// Arrow-key step sizes for a loop edge, in seconds. Backing tracks are long
// and loop edges are usually placed by ear against a bar line, so the fine
// step is a comfortable "just a little later" rather than sample-accurate.
const LOOP_NUDGE_STEP_SEC = { fine: 0.05, coarse: 0.5 };
// How long the "Undo" affordance stays available after deleting a loop
// before the delete becomes permanent. Tune to taste — there's no dialog
// asking "are you sure?" any more, this window IS the confirmation.
const DELETE_UNDO_WINDOW_MS = 10_000;

type SecondsRange = { startSec: number; endSec: number };
type PendingDeletedLoop = { loop: PracticeToolLoopRegion; index: number; timer: ReturnType<typeof setTimeout> };

let candidateRange: RatioRange | null = null;
// Mirrors the gesture controller's steered handle so renderWaveform() can draw
// the focus ring without reaching into the controller mid-draw.
let selectedHandle: RangeHandle = "start";
let rangeSelect: RangeSelectController | null = null;
// The loop currently showing inline-editable name/start/end fields in the
// list — covers both "just created, name it now" and "click the pencil on
// an existing row." There is no separate naming dialog/popover.
let editingLoopId: string | null = null;
// A just-deleted loop, kept around (out of player.loops but not forgotten)
// until DELETE_UNDO_WINDOW_MS elapses or another delete/undo/file-load
// supersedes it — only one pending delete is tracked at a time, matching
// common toast/snackbar UX (a second delete finalizes the first).
let pendingDeletedLoop: PendingDeletedLoop | null = null;

let playheadAnimFrame: number | null = null;
let playheadBaseSec = 0;
let playheadBaseMs = 0;
let playheadSpeed = 1;

/**
 * Builds a template-derived loop name with an auto-incrementing numeric suffix, e.g.
 * "Verse" -> "Verse 1" the first time it's picked on a track, "Verse 2" the next time,
 * regardless of whether the previous suggestion was actually kept. Pure/testable in
 * isolation from the DOM — see tests/practiceToolLoopNaming.test.ts.
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

function ensurePracticeToolState(): PracticeToolState {
  if (!uiState.practiceTool) {
    uiState.practiceTool = {
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
      balance: 0,
      eq: createDefaultPracticeToolEq(),
    };
  }
  return uiState.practiceTool;
}

/** True only when the Practice Tool's own Jam section is on screen. Attribute
 * and class reads only — no geometry, so this never forces a layout the way
 * offsetParent/getBoundingClientRect would. Mirrors isJamPanelVisible() in
 * jam.ts, with the extra check for the active section within the panel. */
function isPracticeToolPanelVisible(): boolean {
  if (!document.getElementById("panel-jam")?.classList.contains("active")) {
    return false;
  }
  const section = document.getElementById("jam-section-panel-practice-tool");
  return !!section && !section.hidden;
}

function getActiveLoop(): PracticeToolLoopRegion | null {
  const player = ensurePracticeToolState();
  if (!player.activeLoopId) {
    return null;
  }
  return player.loops.find((loop) => loop.id === player.activeLoopId) ?? null;
}

function formatClockTime(seconds: number): string {
  const total = Math.max(0, Math.floor(isFinite(seconds) ? seconds : 0));
  const mins = Math.floor(total / 60);
  const secs = total % 60;
  return `${mins}:${secs.toString().padStart(2, "0")}`;
}

/** The loop-length floor is expressed in seconds, so it only becomes a ratio
 * once a track (and therefore a duration) is loaded. */
function minLoopSpanRatio(durationSec: number): number {
  return durationSec > 0 ? Math.min(0.25, MIN_LOOP_SPAN_SEC / durationSec) : 0.01;
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
    const player = ensurePracticeToolState();
    if (!player.playing) {
      playheadAnimFrame = null;
      return;
    }
    // Jamming along with a track while working in the Play view is the normal
    // case, and the playhead is not on screen then — so keep the loop alive
    // (it must resume the moment the panel comes back) but skip the draw,
    // which is a full canvas repaint plus a forced layout, 60x a second.
    if (isPracticeToolPanelVisible()) {
      renderWaveform();
    }
    playheadAnimFrame = requestAnimationFrame(step);
  };
  playheadAnimFrame = requestAnimationFrame(step);
}

function getInterpolatedPositionSec(): number {
  const player = ensurePracticeToolState();
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

/** Called on the `practiceToolFileLoaded` engine message. */
export function applyPracticeToolFileLoaded(data: { path?: string; title?: string; durationSec?: number; waveformPeaksL?: unknown[]; waveformPeaksR?: unknown[] }): void {
  const player = ensurePracticeToolState();
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

  // Loading a new file resets the whole session — the loop list already
  // naturally follows the new file's own fingerprint (above), but Volume/
  // Balance/Speed/Pitch are otherwise global controls that would otherwise
  // silently carry a previous track's tweaks into a fresh one. The caller
  // (Browse/Drop) already confirmed this with the user via
  // confirmResetIfNeeded() before requesting the load, so this always runs
  // unconditionally here — including harmlessly on the very first load,
  // when every fader is already at its default.
  //
  // Unless this load *is* a project recall, in which case the project's own
  // loops and fader settings replace both of the above.
  const recalled = consumePendingProjectRecall(filePath);
  if (recalled) {
    applyPracticeToolProject(recalled, { rerender: false });
  } else {
    resetAllFadersToDefault(player);
    player.eq = createDefaultPracticeToolEq();
    pushPracticeToolEqToEngine(player.eq);
  }

  candidateRange = null;
  editingLoopId = null;
  finalizePendingDelete(); // restoring into a different file's context wouldn't make sense
  playheadBaseSec = 0;
  playheadBaseMs = performance.now();
  playheadSpeed = player.speed;
  stopPlayheadAnim();

  appendLog(`practice tool file loaded ← ${player.title} (${durationSec.toFixed(1)}s)`);
  renderPracticeToolPanel();
}

/** Called on the `practiceToolTransportState` engine message. */
export function applyPracticeToolTransportState(data: { state?: string; positionSec?: number }): void {
  const player = ensurePracticeToolState();
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

  // Playback continues while the user is off on another panel (that's the
  // point — jam along with the track while working in the Play view), but
  // there is nothing to draw then: the canvas and controls are display:none,
  // so a redraw per transport tick is pure cost, and renderWaveform() reads
  // canvas.clientWidth/clientHeight, forcing a synchronous layout each time.
  // State above is still applied, so opening the panel renders it correctly.
  if (!isPracticeToolPanelVisible()) {
    return;
  }

  renderTransportControls();
  renderWaveform();
}

/** Called on the `practiceToolPlaybackEnded` engine message. */
export function applyPracticeToolPlaybackEnded(): void {
  const player = ensurePracticeToolState();
  player.playing = false;
  player.positionSec = 0;
  playheadBaseSec = 0;
  playheadBaseMs = performance.now();
  stopPlayheadAnim();
  appendLog("practice tool playback ended");
  renderTransportControls();
  renderWaveform();
}

/** A debounced one-shot sender: `schedule()` coalesces rapid updates (a slider
 * being dragged, a loop handle being dragged), `flush()` cancels any pending
 * timer and sends straight away.
 *
 * Speed, pitch, and loop-region sends each flush the native render-ahead ring
 * buffer (they must, to apply the new value promptly) — sending on every
 * `input`/`mousemove` event during a drag would flush repeatedly and cause
 * audible stutter, which is exactly what un-throttled sends did before this
 * existed. The one-off end-of-gesture event (`change`, `mouseup`) calls
 * `flush()` so the final value is never left sitting in a pending timer. */
function debouncedSender<T>(send: (value: T) => void, delayMs: number) {
  let timer: ReturnType<typeof setTimeout> | null = null;
  const cancel = () => {
    if (timer !== null) {
      clearTimeout(timer);
      timer = null;
    }
  };
  return {
    schedule(value: T): void {
      cancel();
      timer = setTimeout(() => {
        timer = null;
        send(value);
      }, delayMs);
    },
    flush(value: T): void {
      cancel();
      send(value);
    },
  };
}

// Note the loop sender reads startSec/endSec when the send actually fires, so a
// scheduled send always carries the loop's latest dragged bounds.
const loopRegionSender = debouncedSender(
  (loop: PracticeToolLoopRegion) => setPracticeToolLoopRegion({ startSec: loop.startSec, endSec: loop.endSec }),
  LOOP_REGION_SEND_DEBOUNCE_MS);
const speedSender = debouncedSender(setPracticeToolSpeed, SPEED_PITCH_SEND_DEBOUNCE_MS);
const pitchSender = debouncedSender(setPracticeToolPitch, SPEED_PITCH_SEND_DEBOUNCE_MS);

// ════════════════════════════════════════════════════════════════════
// Unified faders (Volume/Balance/Speed/Pitch): one shared implementation
// instead of four near-duplicated sliders, so they all look, feel, and
// reset the same way.
//
// Every fader's default value sits at the exact visual center regardless
// of how asymmetric its real min/max range is (e.g. Volume's 0-150% with
// a 100% default, or Speed's 25%-200% with a 100% default) — the
// underlying <input type=range> always uses a normalized 0..FADER_SLIDER_STEPS
// domain split into two independently-scaled linear halves (min..default,
// default..max), converted to/from the real value on every read/write. A
// plain <input type=range> can't express that piecewise mapping itself, so
// this conversion layer is what makes "100%" (or "0 st", or center balance)
// always land in the middle of the track, and what makes double-clicking
// anywhere on the slider a well-defined "reset to default" regardless of
// the range's shape.
// ════════════════════════════════════════════════════════════════════

const FADER_SLIDER_STEPS = 1000;

type FaderId = "volume" | "balance" | "speed" | "pitch";

type FaderSpec = {
  id: FaderId;
  min: number;
  max: number;
  default: number;
  format: (value: number) => string;
  parse: (text: string) => number | null;
  getValue: (player: PracticeToolState) => number;
  setValue: (player: PracticeToolState, value: number) => void;
  /** immediate=true on release/reset/typed-entry; false for in-progress drag
   * ticks, letting speed/pitch debounce (they flush the render-ahead ring)
   * while volume/balance (pure audio-thread mix, no flush) can ignore the
   * flag and always send right away. */
  send: (value: number, immediate: boolean) => void;
  /** Extra side effects beyond the field write itself — currently only
   * Speed needs this, to keep the client-side playhead dead-reckoning in
   * sync with the newly-dragged rate. */
  onChange?: (value: number) => void;
};

function faderValueToSliderPos(spec: FaderSpec, value: number): number {
  const half = FADER_SLIDER_STEPS / 2;
  if (value <= spec.default) {
    if (spec.default === spec.min) {
      return half;
    }
    const t = (value - spec.min) / (spec.default - spec.min);
    return Math.round(t * half);
  }
  if (spec.max === spec.default) {
    return half;
  }
  const t = (value - spec.default) / (spec.max - spec.default);
  return Math.round(half + t * half);
}

function faderSliderPosToValue(spec: FaderSpec, pos: number): number {
  const half = FADER_SLIDER_STEPS / 2;
  if (pos <= half) {
    return spec.min + (pos / half) * (spec.default - spec.min);
  }
  return spec.default + ((pos - half) / half) * (spec.max - spec.default);
}

/** Extracts a leading signed number from free-typed text (tolerating a
 * trailing unit like "%" or "st"); returns null if nothing parseable. */
function parseLeadingNumber(text: string): number | null {
  const match = text.trim().match(/^[+-]?\d*\.?\d+/);
  if (!match) {
    return null;
  }
  const n = parseFloat(match[0]);
  return isFinite(n) ? n : null;
}

function parsePercentText(text: string): number | null {
  const n = parseLeadingNumber(text);
  return n === null ? null : n / 100;
}

function formatPitchText(value: number): string {
  return `${value > 0 ? "+" : ""}${value.toFixed(1)} st`;
}

function formatBalanceText(value: number): string {
  const pct = Math.round(value * 100);
  if (pct === 0) {
    return "C";
  }
  return pct < 0 ? `L${Math.abs(pct)}` : `R${pct}`;
}

function parseBalanceText(text: string): number | null {
  const trimmed = text.trim();
  if (/^c(enter)?$/i.test(trimmed)) {
    return 0;
  }
  const sided = /^([lr])\s*(\d+(?:\.\d+)?)$/i.exec(trimmed);
  if (sided) {
    const magnitude = parseFloat(sided[2]) / 100;
    return sided[1].toLowerCase() === "l" ? -magnitude : magnitude;
  }
  const n = parseLeadingNumber(trimmed);
  if (n === null) {
    return null;
  }
  // Accept both "35"/"-35" (percent-style) and "0.35"/"-0.35" (raw fraction).
  return Math.abs(n) > 1 ? n / 100 : n;
}

const FADER_SPECS: Record<FaderId, FaderSpec> = {
  volume: {
    id: "volume",
    min: 0,
    max: 1.5,
    default: 1,
    format: (v) => `${Math.round(v * 100)}%`,
    parse: parsePercentText,
    getValue: (p) => p.gain,
    setValue: (p, v) => { p.gain = v; },
    send: (v) => setPracticeToolGain(v),
  },
  balance: {
    id: "balance",
    min: -1,
    max: 1,
    default: 0,
    format: formatBalanceText,
    parse: parseBalanceText,
    getValue: (p) => p.balance,
    setValue: (p, v) => { p.balance = v; },
    send: (v) => setPracticeToolBalance(v),
  },
  speed: {
    id: "speed",
    min: 0.25,
    max: 2,
    default: 1,
    format: (v) => `${Math.round(v * 100)}%`,
    parse: parsePercentText,
    getValue: (p) => p.speed,
    setValue: (p, v) => { p.speed = v; },
    send: (v, immediate) => (immediate ? speedSender.flush(v) : speedSender.schedule(v)),
    onChange: (v) => {
      playheadSpeed = v;
      playheadBaseSec = getInterpolatedPositionSec();
      playheadBaseMs = performance.now();
    },
  },
  pitch: {
    id: "pitch",
    min: -12,
    max: 12,
    default: 0,
    format: formatPitchText,
    parse: parseLeadingNumber,
    getValue: (p) => p.pitchSemitones,
    setValue: (p, v) => { p.pitchSemitones = v; },
    send: (v, immediate) => (immediate ? pitchSender.flush(v) : pitchSender.schedule(v)),
  },
};

/** Used when loading a new file "resets the project" (see
 * applyPracticeToolFileLoaded) — pushes every fader back to its default,
 * both in local state and to the native engine, mirroring exactly what a
 * double-click reset does for a single fader. */
function resetAllFadersToDefault(player: PracticeToolState): void {
  Object.values(FADER_SPECS).forEach((spec) => {
    spec.setValue(player, spec.default);
    spec.onChange?.(spec.default);
    spec.send(spec.default, true);
  });
}

/**
 * Pushes a recalled project into the live player — its loops, whichever loop
 * was active, and all four fader settings, each sent on to the engine exactly
 * as moving that control by hand would. Getting the *track* loaded is the
 * caller's job: either it is already open, or this runs off the back of the
 * load it asked for (see the recall branch in applyPracticeToolFileLoaded,
 * which renders once at the end and so passes `rerender: false`).
 */
function applyPracticeToolProject(project: PracticeToolProject, options: { rerender?: boolean } = {}): void {
  const player = ensurePracticeToolState();

  player.loops = project.loops.map((loop) => ({ ...loop }));
  // The recalled set becomes this track's working set, so the per-file
  // autosave follows it rather than resurrecting the pre-recall loops the
  // next time the track is opened without a project.
  persistLoopsForCurrentFile();

  const projectFaderValues: Record<FaderId, number> = {
    volume: project.gain,
    balance: project.balance,
    speed: project.speed,
    pitch: project.pitchSemitones,
  };
  Object.values(FADER_SPECS).forEach((spec) => {
    const value = Math.max(spec.min, Math.min(spec.max, projectFaderValues[spec.id]));
    spec.setValue(player, value);
    spec.onChange?.(value);
    spec.send(value, true);
  });

  // A project saved before the EQ existed has no curve; sanitize turns that
  // (and any other gap) into the flat default rather than leaving the previous
  // track's EQ in place.
  player.eq = sanitizePracticeToolEq(project.eq);
  pushPracticeToolEqToEngine(player.eq);

  const activeLoop = project.activeLoopId
    ? player.loops.find((loop) => loop.id === project.activeLoopId) ?? null
    : null;
  player.activeLoopId = activeLoop?.id ?? null;
  player.looping = Boolean(activeLoop);
  if (activeLoop) {
    seekPracticeToolFile(activeLoop.startSec);
    setPracticeToolLoopRegion({ startSec: activeLoop.startSec, endSec: activeLoop.endSec });
    setPracticeToolLooping(true);
  } else {
    setPracticeToolLoopRegion(null);
    setPracticeToolLooping(false);
  }

  candidateRange = null;
  editingLoopId = null;
  finalizePendingDelete();

  if (options.rerender !== false) {
    renderPracticeToolPanel();
  }
}

// Hand the applier to the project bar, which can only *request* a recall —
// see practiceTool/projects.ts for why the indirection exists.
setPracticeToolProjectApplier(applyPracticeToolProject);

function renderFader(spec: FaderSpec, player: PracticeToolState): void {
  const slider = document.getElementById(`practice-tool-${spec.id}`) as HTMLInputElement | null;
  const valueInput = document.getElementById(`practice-tool-${spec.id}-value`) as HTMLInputElement | null;
  const value = spec.getValue(player);
  if (slider && document.activeElement !== slider) {
    slider.value = String(faderValueToSliderPos(spec, value));
  }
  if (valueInput && document.activeElement !== valueInput) {
    valueInput.value = spec.format(value);
  }
}

function bindFader(spec: FaderSpec): void {
  const slider = document.getElementById(`practice-tool-${spec.id}`) as HTMLInputElement | null;
  const valueInput = document.getElementById(`practice-tool-${spec.id}-value`) as HTMLInputElement | null;

  const applyValue = (value: number, immediate: boolean) => {
    const player = ensurePracticeToolState();
    const clamped = Math.max(spec.min, Math.min(spec.max, value));
    spec.setValue(player, clamped);
    spec.onChange?.(clamped);
    renderTransportControls();
    spec.send(clamped, immediate);
  };

  if (slider && slider.dataset.bound !== "true") {
    slider.dataset.bound = "true";
    slider.addEventListener("input", () => {
      const pos = parseFloat(slider.value);
      if (isFinite(pos)) {
        applyValue(faderSliderPosToValue(spec, pos), false);
      }
    });
    // Fires once on release (mouseup/keyup) — always commit the final
    // value immediately even if speed/pitch were mid-debounce.
    slider.addEventListener("change", () => {
      const pos = parseFloat(slider.value);
      if (isFinite(pos)) {
        applyValue(faderSliderPosToValue(spec, pos), true);
      }
    });
    slider.addEventListener("dblclick", () => {
      applyValue(spec.default, true);
      // The two clicks that make up a dblclick each jump the native thumb
      // to the click position first (and focus the slider) before this
      // handler runs — renderFader() then skips redrawing it because it
      // deliberately never overwrites the focused element mid-drag. Force
      // the visual thumb back to center explicitly so it doesn't end up
      // stuck at the click position while the value/text already reset.
      slider.value = String(faderValueToSliderPos(spec, spec.default));
    });
  }

  if (valueInput && valueInput.dataset.bound !== "true") {
    valueInput.dataset.bound = "true";
    const commit = () => {
      const parsed = spec.parse(valueInput.value);
      if (parsed === null) {
        renderTransportControls(); // invalid text — revert to the last real value
        return;
      }
      applyValue(parsed, true);
    };
    valueInput.addEventListener("focus", () => valueInput.select());
    valueInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        valueInput.blur();
      } else if (event.key === "Escape") {
        renderTransportControls();
        valueInput.blur();
      }
    });
    valueInput.addEventListener("focusout", commit);
  }
}

/** Whatever the gesture controller is currently editing: the active loop's
 * bounds, or the pending candidate selection for a not-yet-saved loop. */
function getEditableRange(): RatioRange | null {
  const player = ensurePracticeToolState();
  const activeLoop = getActiveLoop();
  if (!activeLoop) {
    return candidateRange;
  }
  const duration = Math.max(0.001, player.durationSec);
  return { startRatio: activeLoop.startSec / duration, endRatio: activeLoop.endSec / duration };
}

/** Writes a range from the gesture controller back to whichever thing is being
 * edited. Already clamped; the controller renders. */
function applyRangeChange(range: RatioRange): void {
  const player = ensurePracticeToolState();
  const activeLoop = getActiveLoop();
  if (activeLoop) {
    activeLoop.startSec = range.startRatio * player.durationSec;
    activeLoop.endSec = range.endRatio * player.durationSec;
    loopRegionSender.schedule(activeLoop);
  } else {
    candidateRange = { startRatio: range.startRatio, endRatio: range.endRatio };
  }
}

function renderWaveform(): void {
  const canvas = document.getElementById("practice-tool-waveform") as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }
  const player = ensurePracticeToolState();
  const hasAudio = player.waveformPeaksL.length > 0 && player.waveformPeaksR.length > 0 && player.durationSec > 0;
  const activeLoop = getActiveLoop();
  const range = getEditableRange();

  drawWaveform(canvas, {
    // Two lanes: a backing track is genuinely stereo, and a collapsed trace
    // hides that.
    lanes: hasAudio ? [player.waveformPeaksL, player.waveformPeaksR] : [],
    empty: { text: "Drop a WAV, AIFF, or MP3 file here, or use Browse File...", align: "center" },
    range: hasAudio && range
      ? {
          ...range,
          selectedHandle,
          // Solid and "active" once the range belongs to a saved loop; dashed
          // and "candidate" while it is still a selection waiting for
          // "+ Add Loop". The theme decides what each of those looks like.
          tone: activeLoop ? "active" : "candidate",
          emphasis: "tint",
          dashed: !activeLoop,
        }
      : null,
    playhead: hasAudio ? { ratio: getInterpolatedPositionSec() / player.durationSec } : null,
  });
}

function renderAddLoopAffordance(): void {
  const btn = document.getElementById("practice-tool-add-loop-btn") as HTMLButtonElement | null;
  const canvas = document.getElementById("practice-tool-waveform") as HTMLCanvasElement | null;
  if (!btn || !canvas) {
    return;
  }
  const activeLoop = getActiveLoop();
  const show = !activeLoop && Boolean(candidateRange) && !rangeSelect?.isCreating();
  btn.hidden = !show;
  if (show && candidateRange) {
    const width = canvas.clientWidth;
    const endX = Math.max(0, Math.min(width, candidateRange.endRatio * width));
    btn.style.left = `${Math.min(width - 8, endX + 8)}px`;
  }
}

function renderFileInfo(): void {
  const info = document.getElementById("practice-tool-file-info");
  const browseBtn = document.getElementById("practice-tool-browse-btn") as HTMLButtonElement | null;
  if (!info) {
    return;
  }
  const player = ensurePracticeToolState();
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
  const player = ensurePracticeToolState();
  const hasAudio = player.durationSec > 0;

  const playPauseBtn = document.getElementById("practice-tool-play-pause") as HTMLButtonElement | null;
  const stopBtn = document.getElementById("practice-tool-stop") as HTMLButtonElement | null;
  const loopStatus = document.getElementById("practice-tool-loop-status");

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

  const eqBtn = document.getElementById("practice-tool-eq-btn") as HTMLButtonElement | null;
  if (eqBtn) {
    // Lit only when the EQ is both on and actually shaping the track, so the
    // button answers "is something happening to my audio?" rather than "is a
    // checkbox ticked?".
    eqBtn.classList.toggle("is-active", isPracticeToolEqShaping(player.eq));
  }

  Object.values(FADER_SPECS).forEach((spec) => renderFader(spec, player));

  renderFileInfo();
}

/** Populates the shared <datalist> once with the song-section templates —
 * every name/rename input references it via list=, so typing or picking a
 * suggestion works the same whether you're naming a brand new loop or
 * renaming an existing one. */
function ensureLoopNameTemplatesDatalist(): void {
  const datalist = document.getElementById("practice-tool-loop-name-templates");
  if (!datalist || datalist.childElementCount > 0) {
    return;
  }
  datalist.innerHTML = LOOP_NAME_TEMPLATES
    .map((template) => `<option value="${escapeHtml(template)}"></option>`)
    .join("");
}

function renderLoopList(): void {
  const list = document.getElementById("practice-tool-loop-list");
  if (!list) {
    return;
  }
  const player = ensurePracticeToolState();

  // No confirmation dialog on delete — this banner (shown until the undo
  // window elapses, another delete supersedes it, or undo is clicked) IS
  // the confirmation, just reversible instead of blocking.
  const undoBannerHtml = pendingDeletedLoop
    ? `
        <div class="practice-tool-loop-undo-banner">
          <span>Deleted "${escapeHtml(pendingDeletedLoop.loop.name)}".</span>
          <button type="button" class="practice-tool-loop-undo-btn">Undo</button>
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
            <input type="text" class="practice-tool-loop-editable practice-tool-loop-name-input" data-loop-id="${loop.id}" data-field="name" list="practice-tool-loop-name-templates" placeholder="Loop name" value="${escapeHtml(loop.name)}" />
            <input type="number" class="practice-tool-loop-editable practice-tool-loop-time-input" data-loop-id="${loop.id}" data-field="start" min="0" max="${maxSec}" step="0.01" value="${loop.startSec.toFixed(2)}" aria-label="Start time in seconds" />
            <span class="practice-tool-loop-time-sep">–</span>
            <input type="number" class="practice-tool-loop-editable practice-tool-loop-time-input" data-loop-id="${loop.id}" data-field="end" min="0" max="${maxSec}" step="0.01" value="${loop.endSec.toFixed(2)}" aria-label="End time in seconds" />
            <span class="practice-tool-loop-time-unit">s</span>
          `
        : `
            <span class="practice-tool-loop-name">${escapeHtml(loop.name)}</span>
            <span class="practice-tool-loop-range">${formatClockTime(loop.startSec)}–${formatClockTime(loop.endSec)}</span>
          `;
      return `
        <div class="practice-tool-loop-row${isActive ? " is-active" : ""}${isEditing ? " is-editing" : ""}" data-loop-id="${loop.id}">
          <button type="button" class="practice-tool-loop-select-btn" data-loop-id="${loop.id}" aria-pressed="${isActive}" title="${isActive ? "Active loop — click to deactivate" : "Select loop"}">${isActive ? "●" : "○"}</button>
          <div class="practice-tool-loop-row-main" data-loop-id="${loop.id}">
            ${rowMainHtml}
          </div>
          <div class="practice-tool-loop-row-actions">
            ${isEditing ? "" : `<button type="button" class="practice-tool-loop-rename-btn" data-loop-id="${loop.id}" title="Edit name/time" aria-label="Edit loop name and time">✎</button>`}
            <button type="button" class="practice-tool-loop-delete-btn" data-loop-id="${loop.id}" title="Delete" aria-label="Delete loop">✕</button>
          </div>
        </div>
      `;
    })
    .join("");

  if (editingLoopId) {
    const nameInput = list.querySelector<HTMLInputElement>(`.practice-tool-loop-name-input[data-loop-id="${editingLoopId}"]`);
    nameInput?.focus();
    nameInput?.select();
  }
}

function selectLoop(loopId: string): void {
  const player = ensurePracticeToolState();
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
    setPracticeToolLoopRegion(null);
    setPracticeToolLooping(false);
    appendLog(`practice tool loop deactivated → ${loop.name}`);
    renderPracticeToolPanel();
    return;
  }

  player.activeLoopId = loopId;
  player.looping = true;
  candidateRange = null;
  editingLoopId = null;
  selectedHandle = "start";
  seekPracticeToolFile(loop.startSec);
  setPracticeToolLoopRegion({ startSec: loop.startSec, endSec: loop.endSec });
  setPracticeToolLooping(true);
  appendLog(`practice tool loop selected → ${loop.name} (${loop.startSec.toFixed(2)}-${loop.endSec.toFixed(2)}s)`);
  renderPracticeToolPanel();
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

  const player = ensurePracticeToolState();
  const insertAt = Math.min(index, player.loops.length);
  player.loops = [...player.loops.slice(0, insertAt), loop, ...player.loops.slice(insertAt)];
  persistLoopsForCurrentFile();
  appendLog(`practice tool loop delete undone → ${loop.name}`);
  renderPracticeToolPanel();
}

/** Deletes immediately — no confirmation dialog — and instead leaves the
 * loop restorable via an inline "Undo" affordance for DELETE_UNDO_WINDOW_MS.
 * The delete is real (removed from player.loops, engine loop region cleared
 * if it was active) the instant this runs; undo re-inserts it rather than
 * "cancelling" anything in flight. */
function deleteLoop(loopId: string): void {
  const player = ensurePracticeToolState();
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
    setPracticeToolLoopRegion(null);
    setPracticeToolLooping(false);
  }
  if (editingLoopId === loopId) {
    editingLoopId = null;
  }
  persistLoopsForCurrentFile();
  appendLog(`practice tool loop deleted → ${loop.name} (undo available for ${Math.round(DELETE_UNDO_WINDOW_MS / 1000)}s)`);

  pendingDeletedLoop = {
    loop,
    index,
    timer: setTimeout(() => {
      pendingDeletedLoop = null;
      renderLoopList();
    }, DELETE_UNDO_WINDOW_MS),
  };

  renderPracticeToolPanel();
}

/** Commits the name field only — does not touch editingLoopId, since the
 * user may still be tabbing on to the start/end fields in the same row
 * (see the focusout handler in bindLoopListActions for when editing mode
 * actually ends). Does not re-render the list, to avoid destroying the
 * user's in-progress Tab navigation between this row's fields. */
function commitEditLoopName(loopId: string, rawName: string): void {
  const player = ensurePracticeToolState();
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
  const player = ensurePracticeToolState();
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
    setPracticeToolLoopRegion({ startSec: loop.startSec, endSec: loop.endSec });
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

function suggestDefaultLoopName(existingLoops: readonly PracticeToolLoopRegion[]): string {
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
  const player = ensurePracticeToolState();
  const newLoop: PracticeToolLoopRegion = {
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
  seekPracticeToolFile(newLoop.startSec);
  setPracticeToolLoopRegion({ startSec: newLoop.startSec, endSec: newLoop.endSec });
  setPracticeToolLooping(true);
  appendLog(`practice tool loop created → ${newLoop.name} (${newLoop.startSec.toFixed(2)}-${newLoop.endSec.toFixed(2)}s)`);
  renderPracticeToolPanel();
}

function addNewLoop(): void {
  const player = ensurePracticeToolState();
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
  const canvas = document.getElementById("practice-tool-waveform") as HTMLCanvasElement | null;
  if (!canvas || canvas.dataset.bound === "true") {
    return;
  }
  canvas.dataset.bound = "true";

  // Canvas colours come from the theme, and a canvas does not restyle itself —
  // repaint so a theme switch is not stuck behind the next interaction. The
  // palette cache keys on the theme class, so it re-resolves on its own.
  window.addEventListener("themeChanged", () => renderWaveform());

  rangeSelect = bindRangeSelect({
    canvas,
    isEnabled: () => ensurePracticeToolState().durationSec > 0,
    getRange: getEditableRange,
    getMinSpanRatio: () => minLoopSpanRatio(ensurePracticeToolState().durationSec),
    getDurationSec: () => ensurePracticeToolState().durationSec,
    nudgeStepSec: LOOP_NUDGE_STEP_SEC,
    onResize: applyRangeChange,
    onCreate: (range) => {
      candidateRange = range;
    },
    // A sweep replaces whatever row was mid-rename; committing it here keeps
    // the list from re-rendering underneath the gesture.
    onCreateStart: finishEditingLoop,
    onSeek: (ratio) => {
      seekPracticeToolFile(ratio * ensurePracticeToolState().durationSec);
    },
    onCommit: () => {
      const activeLoop = getActiveLoop();
      if (activeLoop) {
        loopRegionSender.flush(activeLoop);
        persistLoopsForCurrentFile();
      }
    },
    onSelectedHandleChange: (handle) => {
      selectedHandle = handle;
    },
    render: () => {
      renderWaveform();
      renderAddLoopAffordance();
    },
  });
}

function bindTransportControls(): void {
  const browseBtn = document.getElementById("practice-tool-browse-btn") as HTMLButtonElement | null;
  const playPauseBtn = document.getElementById("practice-tool-play-pause") as HTMLButtonElement | null;
  const stopBtn = document.getElementById("practice-tool-stop") as HTMLButtonElement | null;

  if (browseBtn && browseBtn.dataset.bound !== "true") {
    browseBtn.dataset.bound = "true";
    browseBtn.addEventListener("click", () => {
      void confirmResetIfNeeded().then((proceed) => {
        if (proceed) {
          browsePracticeToolFile();
        }
      });
    });
  }

  if (playPauseBtn && playPauseBtn.dataset.bound !== "true") {
    playPauseBtn.dataset.bound = "true";
    playPauseBtn.addEventListener("click", () => {
      const player = ensurePracticeToolState();
      if (player.durationSec <= 0) {
        return;
      }
      setPracticeToolTransport(player.playing ? "pause" : "play");
    });
  }

  if (stopBtn && stopBtn.dataset.bound !== "true") {
    stopBtn.dataset.bound = "true";
    stopBtn.addEventListener("click", () => {
      setPracticeToolTransport("stop");
    });
  }

  Object.values(FADER_SPECS).forEach(bindFader);
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
  const list = document.getElementById("practice-tool-loop-list");
  if (list && list.dataset.bound !== "true") {
    list.dataset.bound = "true";
    list.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      if (!target) {
        return;
      }
      if (target.closest(".practice-tool-loop-undo-btn")) {
        undoDeleteLoop();
        return;
      }
      const selectBtn = target.closest<HTMLButtonElement>(".practice-tool-loop-select-btn");
      if (selectBtn) {
        const loopId = selectBtn.dataset.loopId ?? "";
        if (loopId) {
          selectLoop(loopId);
        }
        return;
      }
      const renameBtn = target.closest<HTMLButtonElement>(".practice-tool-loop-rename-btn");
      if (renameBtn) {
        editingLoopId = renameBtn.dataset.loopId ?? null;
        renderLoopList();
        return;
      }
      const deleteBtn = target.closest<HTMLButtonElement>(".practice-tool-loop-delete-btn");
      if (deleteBtn) {
        const loopId = deleteBtn.dataset.loopId ?? "";
        if (loopId) {
          deleteLoop(loopId);
        }
        return;
      }
      const rowMain = target.closest<HTMLElement>(".practice-tool-loop-row-main");
      if (rowMain && !target.closest(".practice-tool-loop-editable")) {
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
      const nameInput = target?.closest<HTMLInputElement>(".practice-tool-loop-name-input");
      if (!nameInput || !LOOP_NAME_TEMPLATES.includes(nameInput.value)) {
        return;
      }
      const player = ensurePracticeToolState();
      const otherNames = player.loops
        .filter((loop) => loop.id !== nameInput.dataset.loopId)
        .map((loop) => loop.name);
      nameInput.value = suggestLoopTemplateName(nameInput.value, otherNames);
    });

    list.addEventListener("keydown", (event) => {
      const target = event.target as HTMLElement | null;
      const input = target?.closest<HTMLInputElement>(".practice-tool-loop-editable");
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
      const input = target?.closest<HTMLInputElement>(".practice-tool-loop-editable");
      if (!input) {
        return;
      }
      commitEditableField(input);

      // Only end editing mode (and re-render) once focus actually leaves
      // this loop's row — e.g. Tab moving from the name field to the start-
      // time field must NOT re-render mid-tab, or the Tab destination would
      // vanish before the browser gets to focus it.
      const row = input.closest(".practice-tool-loop-row");
      const nextFocus = focusEvent.relatedTarget;
      const staysInRow = row && nextFocus instanceof Node && row.contains(nextFocus);
      if (!staysInRow) {
        finishEditingLoop();
      }
    });
  }

  const newLoopBtn = document.getElementById("practice-tool-new-loop-btn") as HTMLButtonElement | null;
  if (newLoopBtn && newLoopBtn.dataset.bound !== "true") {
    newLoopBtn.dataset.bound = "true";
    newLoopBtn.addEventListener("click", () => addNewLoop());
  }

  const addLoopBtn = document.getElementById("practice-tool-add-loop-btn") as HTMLButtonElement | null;
  if (addLoopBtn && addLoopBtn.dataset.bound !== "true") {
    addLoopBtn.dataset.bound = "true";
    addLoopBtn.addEventListener("click", () => addNewLoop());
  }
}

function bindAllActions(): void {
  bindWaveformInteractions();
  bindTransportControls();
  bindLoopListActions();
  bindPracticeToolProjectActions();
  bindPracticeToolEqModal();
  bindPracticeToolDropZone();

  const eqBtn = document.getElementById("practice-tool-eq-btn") as HTMLButtonElement | null;
  if (eqBtn && eqBtn.dataset.bound !== "true") {
    eqBtn.dataset.bound = "true";
    eqBtn.addEventListener("click", () => openPracticeToolEqModal());
  }
}

// The modal changes state the panel's EQ button reflects, and it can only
// *ask* for that redraw — see practiceTool/eqModal.ts for why.
setPracticeToolEqChangeListener(() => renderTransportControls());

export function renderPracticeToolPanel(): void {
  ensureLoopNameTemplatesDatalist();
  renderFileInfo();
  renderTransportControls();
  renderWaveform();
  renderAddLoopAffordance();
  renderLoopList();
  renderPracticeToolProjects();
  renderPracticeToolEqModal();
  bindAllActions();
}

export function initializePracticeToolPanel(): void {
  bindAllActions();
  renderPracticeToolPanel();
}
