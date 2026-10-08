import {
  browsePracticeToolFile,
  seekPracticeToolFile,
  setPracticeToolLoopRegion,
  setPracticeToolLooping,
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
import {
  bindPracticeToolProjectActions,
  renderPracticeToolProjects,
  suggestPracticeToolProjectName,
} from "./practiceTool/projectsPanel.js";
import {
  applyFaderSettings,
  bindPracticeToolFaders,
  captureFaderSettings,
  debouncedSender,
  type FaderId,
  renderPracticeToolFaders,
} from "./practiceTool/faders.js";
import { bindPracticeToolDropZone, confirmResetIfNeeded } from "./practiceTool/trackImport.js";
import { createDefaultPracticeToolEq, isPracticeToolEqShaping, sanitizePracticeToolEq } from "./practiceTool/eq.js";
import { pushPracticeToolEqToEngine } from "./practiceTool/eqSend.js";
import {
  bindPracticeToolEqModal,
  openPracticeToolEqModal,
  renderPracticeToolEqModal,
  setPracticeToolEqChangeListener,
} from "./practiceTool/eqModal.js";
import {
  bindJumpBackButton,
  clearJumpBackPoint,
  getJumpBackMarkerSec,
  renderJumpBackButton,
  setJumpBackPoint,
} from "./practiceTool/jumpBack.js";

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
    applyFaderSettings(player, null, onFaderChange);
    player.eq = createDefaultPracticeToolEq();
    pushPracticeToolEqToEngine(player.eq);
    suggestPracticeToolProjectName(player.title);
  }

  candidateRange = null;
  editingLoopId = null;
  clearJumpBackPoint();
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
  renderLoopPlayButtons();
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
  renderLoopPlayButtons();
}

// Note the loop sender reads startSec/endSec when the send actually fires, so a
// scheduled send always carries the loop's latest dragged bounds — and drops
// it if another loop was selected in the meantime, whose own restart has
// already told the engine where to loop.
const loopRegionSender = debouncedSender(
  (loop: PracticeToolLoopRegion) => {
    if (loop.id === ensurePracticeToolState().activeLoopId) {
      setPracticeToolLoopRegion({ startSec: loop.startSec, endSec: loop.endSec });
    }
  },
  LOOP_REGION_SEND_DEBOUNCE_MS);

/** Every fader write lands here, by hand or not. Speed keeps the playhead's
 * dead-reckoning in step, and the active loop takes the new values as its own
 * settings — saved on release, not on every tick of a drag. */
function onFaderChange(id: FaderId, value: number, immediate: boolean): void {
  if (id === "speed") {
    playheadBaseSec = getInterpolatedPositionSec(); // at the old rate, up to now
    playheadBaseMs = performance.now();
    playheadSpeed = value;
  }
  const activeLoop = getActiveLoop();
  if (activeLoop) {
    activeLoop.settings = captureFaderSettings(ensurePracticeToolState());
    if (immediate) {
      persistLoopsForCurrentFile();
    }
  }
}

/** Moves the client-side playhead straight to `sec` rather than waiting for
 * the engine's next transport report, which never comes while paused. */
function resetPlayheadTo(sec: number): void {
  const player = ensurePracticeToolState();
  player.positionSec = sec;
  playheadBaseSec = sec;
  playheadBaseMs = performance.now();
}

/** Seeks the track, playing or not, and moves the drawn playhead with it. */
function jumpTo(sec: number): void {
  seekPracticeToolFile(sec);
  resetPlayheadTo(sec);
  renderWaveform();
  renderFileInfo();
}

/**
 * Makes `loop` the active one and puts playback at its very start. Its own
 * track settings go first — Speed among them, which the engine's restart is
 * computed at — then the region, looping and the jump to its start travel as
 * one engine change (`restart`), so nothing of the previous position or the
 * previous loop's bounds can leak into the top of this one.
 */
function activateLoop(loop: PracticeToolLoopRegion): void {
  const player = ensurePracticeToolState();
  // Cleared first: applying this loop's settings must not record them onto
  // whichever loop is being left (see onFaderChange).
  player.activeLoopId = null;
  if (loop.settings) {
    applyFaderSettings(player, loop.settings, onFaderChange);
  }
  player.activeLoopId = loop.id;
  player.looping = true;
  if (!loop.settings) {
    // A loop from before loops had settings adopts the current ones.
    loop.settings = captureFaderSettings(player);
    persistLoopsForCurrentFile();
  }
  candidateRange = null;
  selectedHandle = "start";
  clearJumpBackPoint(); // Jump Back now returns to this loop's start
  setPracticeToolLoopRegion({ startSec: loop.startSec, endSec: loop.endSec }, { restart: true });
  resetPlayheadTo(loop.startSec);
}

function deactivateActiveLoop(): void {
  const player = ensurePracticeToolState();
  player.activeLoopId = null;
  player.looping = false;
  setPracticeToolLoopRegion(null);
  setPracticeToolLooping(false);
}

/**
 * Pushes a recalled project into the live player — its loops, all four fader
 * settings and the EQ, each sent on to the engine exactly as moving that
 * control by hand would, then whichever loop was active (with its own
 * settings). Getting the *track* loaded is the caller's job: either it is
 * already open, or this runs off the back of the load it asked for (see the
 * recall branch in applyPracticeToolFileLoaded, which renders once at the end
 * and so passes `rerender: false`).
 */
function applyPracticeToolProject(project: PracticeToolProject, options: { rerender?: boolean } = {}): void {
  const player = ensurePracticeToolState();

  player.loops = project.loops.map((loop) => ({ ...loop }));
  player.activeLoopId = null;
  // The recalled set becomes this track's working set, so the per-file
  // autosave follows it rather than resurrecting the pre-recall loops the
  // next time the track is opened without a project.
  persistLoopsForCurrentFile();

  applyFaderSettings(player, project, onFaderChange);

  // A project saved before the EQ existed has no curve; sanitize turns that
  // (and any other gap) into the flat default rather than leaving the previous
  // track's EQ in place.
  player.eq = sanitizePracticeToolEq(project.eq);
  pushPracticeToolEqToEngine(player.eq);

  const activeLoop = project.activeLoopId
    ? player.loops.find((loop) => loop.id === project.activeLoopId) ?? null
    : null;
  if (activeLoop) {
    activateLoop(activeLoop);
  } else {
    deactivateActiveLoop();
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

/** True when a loop is active and `ratio` (a point on the waveform) is not in it. */
function isOutsideActiveLoop(ratio: number): boolean {
  const activeLoop = getActiveLoop();
  if (!activeLoop) {
    return false;
  }
  const sec = ratio * ensurePracticeToolState().durationSec;
  return sec < activeLoop.startSec || sec > activeLoop.endSec;
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
  const jumpBackSec = getJumpBackMarkerSec(activeLoop);

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
    marker: hasAudio && jumpBackSec !== null ? { ratio: jumpBackSec / player.durationSec } : null,
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
  renderJumpBackButton(hasAudio);
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

  renderPracticeToolFaders(player);

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
            <input type="text" class="practice-tool-loop-editable practice-tool-loop-name-input" data-loop-id="${escapeHtml(loop.id)}" data-field="name" list="practice-tool-loop-name-templates" placeholder="Loop name" value="${escapeHtml(loop.name)}" />
            <input type="number" class="practice-tool-loop-editable practice-tool-loop-time-input" data-loop-id="${escapeHtml(loop.id)}" data-field="start" min="0" max="${escapeHtml(maxSec)}" step="0.01" value="${loop.startSec.toFixed(2)}" aria-label="Start time in seconds" />
            <span class="practice-tool-loop-time-sep">–</span>
            <input type="number" class="practice-tool-loop-editable practice-tool-loop-time-input" data-loop-id="${escapeHtml(loop.id)}" data-field="end" min="0" max="${escapeHtml(maxSec)}" step="0.01" value="${loop.endSec.toFixed(2)}" aria-label="End time in seconds" />
            <span class="practice-tool-loop-time-unit">s</span>
          `
        : `
            <span class="practice-tool-loop-name">${escapeHtml(loop.name)}</span>
            <span class="practice-tool-loop-range">${formatClockTime(loop.startSec)}–${formatClockTime(loop.endSec)}</span>
          `;
      return `
        <div class="practice-tool-loop-row${isActive ? " is-active" : ""}${isEditing ? " is-editing" : ""}" data-loop-id="${escapeHtml(loop.id)}">
          <button type="button" class="practice-tool-loop-select-btn" data-loop-id="${escapeHtml(loop.id)}" aria-pressed="${isActive}" title="${isActive ? "Active loop — click to deactivate" : "Select loop"}">${isActive ? "●" : "○"}</button>
          <button type="button" class="practice-tool-loop-play-btn" data-loop-id="${escapeHtml(loop.id)}"></button>
          <div class="practice-tool-loop-row-main" data-loop-id="${escapeHtml(loop.id)}">
            ${rowMainHtml}
          </div>
          <div class="practice-tool-loop-row-actions">
            ${isEditing ? "" : `<button type="button" class="practice-tool-loop-rename-btn" data-loop-id="${escapeHtml(loop.id)}" title="Edit name/time" aria-label="Edit loop name and time">✎</button>`}
            <button type="button" class="practice-tool-loop-delete-btn" data-loop-id="${escapeHtml(loop.id)}" title="Delete" aria-label="Delete loop">✕</button>
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
  renderLoopPlayButtons();
}

/** Each row's play/stop button follows the transport, which reports several
 * times a second while playing — so it is updated in place rather than by
 * rebuilding the list, which would also tear down a row mid-rename. */
function renderLoopPlayButtons(): void {
  const player = ensurePracticeToolState();
  document.querySelectorAll<HTMLButtonElement>("#practice-tool-loop-list .practice-tool-loop-play-btn").forEach((btn) => {
    const playingThis = player.playing && btn.dataset.loopId === player.activeLoopId;
    const label = playingThis ? "Stop this loop" : "Play this loop from its start";
    if (btn.textContent !== (playingThis ? "■" : "▶")) {
      btn.textContent = playingThis ? "■" : "▶";
      btn.title = label;
      btn.setAttribute("aria-label", label);
      btn.classList.toggle("is-playing", playingThis);
    }
  });
}

/** A row's ▶: select the loop and play it from the top, whatever was playing
 * before. Its ■: stop, which the engine rewinds to the loop's start. */
function toggleLoopPlayback(loopId: string): void {
  const player = ensurePracticeToolState();
  const loop = player.loops.find((entry) => entry.id === loopId);
  if (!loop || player.durationSec <= 0) {
    return;
  }
  if (loop.id === player.activeLoopId && player.playing) {
    setPracticeToolTransport("stop");
    return;
  }
  editingLoopId = null;
  activateLoop(loop);
  setPracticeToolTransport("play");
  appendLog(`practice tool loop played → ${loop.name} (${loop.startSec.toFixed(2)}-${loop.endSec.toFixed(2)}s)`);
  renderPracticeToolPanel();
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
    deactivateActiveLoop();
    appendLog(`practice tool loop deactivated → ${loop.name}`);
    renderPracticeToolPanel();
    return;
  }

  editingLoopId = null;
  activateLoop(loop);
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
    deactivateActiveLoop();
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
    // A new loop starts out with the track settings it was made under.
    settings: captureFaderSettings(player),
  };
  player.loops = [...player.loops, newLoop];
  activateLoop(newLoop);
  editingLoopId = newLoop.id;
  persistLoopsForCurrentFile();
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
      // Still set only when the sweep began inside the active loop, which
      // keeps it: a drag there is not a request for a new one.
      if (!getActiveLoop()) {
        candidateRange = range;
      }
    },
    // A sweep replaces whatever row was mid-rename; committing it here keeps
    // the list from re-rendering underneath the gesture. One begun outside
    // the active loop lets go of it and marks out a new loop instead.
    onCreateStart: (anchorRatio) => {
      finishEditingLoop();
      if (isOutsideActiveLoop(anchorRatio)) {
        deactivateActiveLoop();
        renderLoopList();
        renderTransportControls();
      }
    },
    onSeek: (ratio) => {
      const player = ensurePracticeToolState();
      const sec = ratio * player.durationSec;
      // A click outside the active loop starts a new one there: the loop is
      // let go, playback moves to the click, and a default-length selection
      // waits on "+ Add Loop" with its handles ready to drag. A click inside
      // the loop (or with none active) is just a seek.
      if (isOutsideActiveLoop(ratio)) {
        deactivateActiveLoop();
        const end = Math.min(player.durationSec, sec + DEFAULT_NEW_LOOP_LENGTH_SEC);
        const start = Math.max(0, Math.min(sec, end - MIN_LOOP_SPAN_SEC));
        candidateRange = { startRatio: start / player.durationSec, endRatio: end / player.durationSec };
        renderLoopList();
        renderTransportControls();
      }
      setJumpBackPoint(sec);
      jumpTo(sec);
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

  bindJumpBackButton(getActiveLoop, jumpTo);
  bindPracticeToolFaders(ensurePracticeToolState, onFaderChange, renderTransportControls);
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
      const playBtn = target.closest<HTMLButtonElement>(".practice-tool-loop-play-btn");
      if (playBtn) {
        const loopId = playBtn.dataset.loopId ?? "";
        if (loopId) {
          toggleLoopPlayback(loopId);
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
