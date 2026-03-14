import { armRiffCapture, deleteRiff, getRiffLibrary, importRiffWav, loadRiffTakeForEdit, markRiffUsed, previewCapturedRiffRange, previewRiffTake, saveRiffTake, setMetronome, setRiffFavorite, setRiffLibraryPath, startRiffCapture, stopPreviewPlayback, stopRiffCapture, trimCapturedRiff } from "./bridge.js";
import { showConfirm } from "./dialogs.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { importPackWithConfirmation } from "./presets.js";
import { uiState } from "./state.js";
import type { RiffCaptureState, RiffLibrary } from "./types.js";
import { arrayBufferToBase64, parseWavMetadata } from "./utils.js";

let capturedPreviewAnimationFrame: number | null = null;
let capturedPreviewActive = false;
let capturedPreviewStartMs = 0;
let capturedPreviewDurationMs = 0;
let capturedPreviewProgress = 0;
let activeRiffTakePreviewId = "";
let trimStartRatio = 0;
let trimEndRatio = 1;
let activeTrimHandle: "start" | "end" | null = null;
let selectedTrimHandle: "start" | "end" = "start";
let editingRiffId = "";
let savingFromCapture = false;
// ARM state: true when armed (click playing, waiting for input signal)
let isArmed = false;
// Live waveform during active recording (populated by riffCaptureProgress messages)
let liveWaveformPeaks: number[] = [];

function openRiffCaptureModal(): void {
  const modal = document.getElementById("riff-capture-modal") as HTMLDivElement | null;
  if (!modal) {
    return;
  }
  // Auto-populate tempo from the running metronome so the user doesn't have to enter it twice
  const metronomeBpm = uiState.metronome?.bpm;
  if (metronomeBpm && metronomeBpm > 0) {
    const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
    if (tempoInput) {
      tempoInput.value = String(Math.round(metronomeBpm * 10) / 10);
    }
  }
  const accentInput = document.getElementById("riff-capture-accent") as HTMLInputElement | null;
  if (accentInput) accentInput.value = uiState.metronome?.beatPattern ?? "LHLH";
  const volumeDb = uiState.metronome?.volumeDb ?? -12;
  const metroVolumeSlider = document.getElementById("riff-capture-metro-volume") as HTMLInputElement | null;
  const metroVolumeLabel = document.getElementById("riff-capture-metro-volume-label") as HTMLElement | null;
  const clickEnabledInput = document.getElementById("riff-capture-enable-click") as HTMLInputElement | null;
  if (metroVolumeSlider) metroVolumeSlider.value = String(Math.round(volumeDb));
  if (metroVolumeLabel) metroVolumeLabel.textContent = `${Math.round(volumeDb)} dB`;
  if (clickEnabledInput) clickEnabledInput.checked = uiState.riffCapture?.metronomeClickEnabled ?? true;
  activateRiffCaptureTab("main");
  modal.style.display = "flex";
}

function activateRiffCaptureTab(tab: "main" | "advanced"): void {
  const buttons = Array.from(document.querySelectorAll<HTMLButtonElement>(".riff-capture-tab-btn"));
  const panels = Array.from(document.querySelectorAll<HTMLElement>(".riff-capture-tab-panel"));
  buttons.forEach((button) => {
    button.classList.toggle("active", button.dataset.riffCaptureTab === tab);
  });
  panels.forEach((panel) => {
    panel.classList.toggle("active", panel.dataset.riffCaptureTabPanel === tab);
  });
}

function bindRiffCaptureTabs(): void {
  const container = document.querySelector<HTMLElement>(".riff-capture-tabs");
  if (!container || container.dataset.bound === "true") {
    return;
  }
  container.dataset.bound = "true";
  container.addEventListener("click", (event) => {
    const button = (event.target as HTMLElement | null)?.closest<HTMLButtonElement>(".riff-capture-tab-btn");
    const tab = button?.dataset.riffCaptureTab;
    if (tab === "main" || tab === "advanced") {
      activateRiffCaptureTab(tab);
    }
  });
}

function startRiffCaptureFromModal(): boolean {
  if (isArmed || uiState.riffCapture?.active) {
    return false;
  }

  const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
  const numInput = document.getElementById("riff-capture-timesig-num") as HTMLInputElement | null;
  const denInput = document.getElementById("riff-capture-timesig-den") as HTMLInputElement | null;
  const countInInput = document.getElementById("riff-capture-countin") as HTMLInputElement | null;
  const patternTypeSelect = document.getElementById("riff-capture-pattern-type") as HTMLSelectElement | null;
  const patternIdInput = document.getElementById("riff-capture-pattern-id") as HTMLInputElement | null;
  const accentInput = document.getElementById("riff-capture-accent") as HTMLInputElement | null;

  const tempo = Math.max(30, Math.min(300, Number(tempoInput?.value ?? 120)));
  const timeSigNum = Math.max(1, Number(numInput?.value ?? 4));
  const timeSigDen = Math.max(1, Number(denInput?.value ?? 4));
  const bars = 64;
  const countInBars = Math.max(0, Number(countInInput?.value ?? 1));
  const metronomeClickEnabled = isCaptureMetronomeClickEnabled();
  const patternType = (patternTypeSelect?.value === "drum" ? "drum" : "click") as "click" | "drum";
  const patternId = patternIdInput?.value.trim() || undefined;
  const beatPattern = accentInput?.value.trim().toUpperCase() || undefined;

  armRiffCapture({ tempoBpm: tempo, timeSigNum, timeSigDen, bars, countInBars, metronomeClickEnabled, patternType, patternId, beatPattern });
  isArmed = true;
  appendLog(`riff capture armed → unlimited (64-bar buffer) @ ${tempo} bpm (${timeSigNum}/${timeSigDen}), count-in: ${countInBars} bars (waiting for signal)`);
  renderRiffCaptureStatus();
  return true;
}

function closeRiffCaptureModal(): void {
  const modal = document.getElementById("riff-capture-modal") as HTMLDivElement | null;
  if (!modal) {
    return;
  }
  if (isArmed || uiState.riffCapture?.active) {
    isArmed = false;
    stopRiffCapture(true);
    setMetronome({ enabled: false });
    renderRiffCaptureStatus();
  }
  modal.style.display = "none";
}

function hasFilePayload(event: DragEvent): boolean {
  const types = Array.from(event.dataTransfer?.types ?? []);
  return types.includes("Files");
}

function splitCsv(value: string): string[] {
  return value
    .split(",")
    .map((entry) => entry.trim())
    .filter((entry) => entry.length > 0);
}

function formatDuration(seconds: number): string {
  if (!isFinite(seconds) || seconds <= 0) {
    return "0.0s";
  }
  if (seconds < 60) {
    return `${seconds.toFixed(1)}s`;
  }
  const mins = Math.floor(seconds / 60);
  const secs = Math.floor(seconds % 60);
  return `${mins}:${secs.toString().padStart(2, "0")}`;
}

function getPreferredTakeId(riff: RiffLibrary["riffs"][number]): string | null {
  if (!riff.takes.length) {
    return null;
  }
  if (riff.preferredTakeId && riff.takes.some((take) => take.id === riff.preferredTakeId)) {
    return riff.preferredTakeId;
  }
  return riff.takes[0].id;
}

function populateSaveModalFields(riff?: RiffLibrary["riffs"][number]): void {
  const titleInput = document.getElementById("riff-save-title") as HTMLInputElement | null;
  const categoriesInput = document.getElementById("riff-save-categories") as HTMLInputElement | null;
  const tagsInput = document.getElementById("riff-save-tags") as HTMLInputElement | null;
  const notesInput = document.getElementById("riff-save-notes") as HTMLInputElement | null;
  const favoriteInput = document.getElementById("riff-save-favorite") as HTMLInputElement | null;

  if (!riff) {
    const riffs = uiState.riffLibrary?.riffs ?? [];
    const nextNumber = riffs.length + 1;
    if (titleInput) titleInput.value = `riff-${nextNumber}`;
    const mostRecent = riffs.length > 0 ? riffs[riffs.length - 1] : null;
    if (categoriesInput) categoriesInput.value = mostRecent?.categories?.join(", ") ?? "";
    if (tagsInput) tagsInput.value = mostRecent?.tags?.join(", ") ?? "";
    if (notesInput) notesInput.value = "";
    if (favoriteInput) favoriteInput.checked = false;
    return;
  }

  if (titleInput) {
    titleInput.value = riff.title ?? "";
  }
  if (categoriesInput) {
    categoriesInput.value = Array.isArray(riff.categories) ? riff.categories.join(", ") : "";
  }
  if (tagsInput) {
    tagsInput.value = Array.isArray(riff.tags) ? riff.tags.join(", ") : "";
  }
  if (notesInput) {
    notesInput.value = riff.notes ?? "";
  }
  if (favoriteInput) {
    favoriteInput.checked = Boolean(riff.favorite);
  }
}

export function applyRiffLibraryState(library: Partial<RiffLibrary>): void {
  uiState.riffLibrary = {
    path: typeof library.path === "string" ? library.path : uiState.riffLibrary?.path ?? "",
    riffs: Array.isArray(library.riffs) ? library.riffs : uiState.riffLibrary?.riffs ?? [],
  };
  if (activeRiffTakePreviewId && !isLibraryTakeId(activeRiffTakePreviewId)) {
    activeRiffTakePreviewId = "";
  }
  renderRiffLibraryPanel();
}

export function applyRiffCaptureState(capture: Partial<RiffCaptureState>): void {
  const previousTakeId = uiState.riffCapture?.takeId ?? "";
  const wasArmed = isArmed;
  // Update module-level armed state
  if (typeof capture.armed === "boolean") {
    isArmed = capture.armed;
  }
  // When recording starts or capture is cleared, reset live waveform
  if (capture.active === true || capture.active === false) {
    if (!capture.active) {
      liveWaveformPeaks = [];
    }
  }
  uiState.riffCapture = {
    active: Boolean(capture.active),
    complete: Boolean(capture.complete),
    armed: isArmed,
    takeId: typeof capture.takeId === "string" ? capture.takeId : uiState.riffCapture?.takeId ?? "",
    bars: typeof capture.bars === "number" ? capture.bars : uiState.riffCapture?.bars ?? 1,
    tempoBpm: typeof capture.tempoBpm === "number" ? capture.tempoBpm : uiState.riffCapture?.tempoBpm ?? 120,
    timeSigNum: typeof capture.timeSigNum === "number" ? capture.timeSigNum : uiState.riffCapture?.timeSigNum ?? 4,
    timeSigDen: typeof capture.timeSigDen === "number" ? capture.timeSigDen : uiState.riffCapture?.timeSigDen ?? 4,
    metronomeClickEnabled: typeof capture.metronomeClickEnabled === "boolean"
      ? capture.metronomeClickEnabled
      : uiState.riffCapture?.metronomeClickEnabled ?? true,
    capturedSamples: typeof capture.capturedSamples === "number" ? capture.capturedSamples : uiState.riffCapture?.capturedSamples ?? 0,
    sampleRate: typeof capture.sampleRate === "number" ? capture.sampleRate : uiState.riffCapture?.sampleRate ?? 0,
    hasAudio: typeof capture.hasAudio === "boolean" ? capture.hasAudio : uiState.riffCapture?.hasAudio ?? false,
    waveformPeaks: Array.isArray(capture.waveformPeaks)
      ? capture.waveformPeaks.filter((value): value is number => typeof value === "number")
      : uiState.riffCapture?.waveformPeaks ?? [],
    // Preserve barAlignOffsetSamples from riffCaptureStarted across state updates
    barAlignOffsetSamples: typeof capture.barAlignOffsetSamples === "number"
      ? capture.barAlignOffsetSamples
      : uiState.riffCapture?.barAlignOffsetSamples ?? 0,
  };

  if (!isArmed && wasArmed) {
    // renderRiffCaptureStatus called below handles the button state update
  }

  const shouldResetTrim = !uiState.riffCapture.hasAudio
    || uiState.riffCapture.takeId !== previousTakeId
    || Array.isArray(capture.waveformPeaks);
  if (shouldResetTrim) {
    trimStartRatio = 0;
    trimEndRatio = 1;
  }

  // Auto-apply bar-aligned trim start when capture completes in arm mode
  if (capture.complete && capture.hasAudio) {
    const offset = uiState.riffCapture.barAlignOffsetSamples ?? 0;
    const total = uiState.riffCapture.capturedSamples;
    if (offset > 0 && total > 0 && offset < total * 0.5) {
      trimStartRatio = offset / total;
    }
  }

  if (!uiState.riffCapture.hasAudio) {
    stopCapturedPreviewAnimation();
  }

  renderRiffCaptureStatus();
  syncTrimControlsFromState();
  renderCapturedWaveform();
  renderCapturedPlayButton();
}

/** Called with periodic waveform updates during active recording */
export function applyRiffCaptureProgress(capturedSamples: number, peaks: number[]): void {
  liveWaveformPeaks = peaks;
  if (uiState.riffCapture) {
    uiState.riffCapture = { ...uiState.riffCapture, capturedSamples };
  }
  renderCapturedWaveform();
}

function stopCapturedPreviewAnimation(resetProgress = true): void {
  capturedPreviewActive = false;
  if (capturedPreviewAnimationFrame !== null) {
    cancelAnimationFrame(capturedPreviewAnimationFrame);
    capturedPreviewAnimationFrame = null;
  }
  if (resetProgress) {
    capturedPreviewProgress = 0;
  }
  renderCapturedPlayButton();
}

function startCapturedPreviewAnimation(): void {
  const capture = uiState.riffCapture;
  const durationSec = capture && capture.sampleRate > 0 ? getTrimWindowDurationSec() : 0;
  if (!capture?.hasAudio || durationSec <= 0) {
    stopCapturedPreviewAnimation();
    renderCapturedWaveform();
    return;
  }

  stopCapturedPreviewAnimation(false);
  capturedPreviewActive = true;
  capturedPreviewStartMs = performance.now();
  capturedPreviewDurationMs = durationSec * 1000;
  capturedPreviewProgress = 0;
  renderCapturedPlayButton();

  const step = (now: number) => {
    if (!capturedPreviewActive) {
      return;
    }
    const elapsed = now - capturedPreviewStartMs;
    capturedPreviewProgress = Math.max(0, Math.min(1, capturedPreviewDurationMs > 0 ? elapsed / capturedPreviewDurationMs : 1));
    renderCapturedWaveform();
    if (capturedPreviewProgress >= 1) {
      stopCapturedPreviewAnimation();
      renderCapturedWaveform();
      return;
    }
    capturedPreviewAnimationFrame = requestAnimationFrame(step);
  };

  capturedPreviewAnimationFrame = requestAnimationFrame(step);
}

function isCapturedPreviewId(previewId: string): boolean {
  const takeId = uiState.riffCapture?.takeId ?? "";
  return Boolean(previewId) && (previewId === takeId || previewId === "captured-take");
}

function isLibraryTakeId(takeId: string): boolean {
  if (!takeId) {
    return false;
  }
  const riffs = uiState.riffLibrary?.riffs ?? [];
  return riffs.some((riff) => riff.takes.some((take) => take.id === takeId));
}

function clampTrimRange(start: number, end: number): { start: number; end: number } {
  const clampedStart = Math.max(0, Math.min(1, start));
  const clampedEnd = Math.max(0, Math.min(1, end));
  const minSpan = 0.001;
  if (clampedEnd - clampedStart >= minSpan) {
    return { start: clampedStart, end: clampedEnd };
  }
  if (clampedStart + minSpan <= 1) {
    return { start: clampedStart, end: clampedStart + minSpan };
  }
  return { start: Math.max(0, 1 - minSpan), end: 1 };
}

function getCaptureDurationSec(): number {
  const capture = uiState.riffCapture;
  if (!capture || capture.sampleRate <= 0 || capture.capturedSamples <= 0) {
    return 0;
  }
  return capture.capturedSamples / capture.sampleRate;
}

function getTrimWindowDurationSec(): number {
  const totalDuration = getCaptureDurationSec();
  const range = clampTrimRange(trimStartRatio, trimEndRatio);
  return Math.max(0, totalDuration * (range.end - range.start));
}

function updateTrimLabels(): void {
  const startLabel = document.getElementById("riff-trim-start-label");
  const endLabel = document.getElementById("riff-trim-end-label");
  if (!startLabel || !endLabel) {
    return;
  }
  const totalDuration = getCaptureDurationSec();
  const range = clampTrimRange(trimStartRatio, trimEndRatio);
  startLabel.textContent = `${(totalDuration * range.start).toFixed(2)}s`;
  endLabel.textContent = `${(totalDuration * range.end).toFixed(2)}s`;

  const barsDisplay = document.getElementById("riff-capture-bars-display");
  if (barsDisplay) {
    const hasAudio = Boolean(uiState.riffCapture?.hasAudio);
    barsDisplay.textContent = hasAudio ? `${computeBarsFromTrimWindow()} bars` : "— bars";
  }
}

function syncTrimControlsFromState(): void {
  const hasAudio = Boolean(uiState.riffCapture?.hasAudio);
  const cropBtn = document.getElementById("riff-capture-trim") as HTMLButtonElement | null;
  if (cropBtn) {
    cropBtn.disabled = !hasAudio;
  }
  updateTrimLabels();
}

function setTrimRange(startRatio: number, endRatio: number): void {
  const range = clampTrimRange(startRatio, endRatio);
  trimStartRatio = range.start;
  trimEndRatio = range.end;
  updateTrimLabels();
  renderCapturedWaveform();
}

function getCanvasRatioFromPointer(event: MouseEvent, canvas: HTMLCanvasElement): number {
  const rect = canvas.getBoundingClientRect();
  if (rect.width <= 0) {
    return 0;
  }
  return Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
}

function nudgeSelectedTrimHandle(direction: -1 | 1, coarse = false): void {
  const step = coarse ? 0.01 : 0.001;
  if (selectedTrimHandle === "start") {
    setTrimRange(trimStartRatio + direction * step, trimEndRatio);
  } else {
    setTrimRange(trimStartRatio, trimEndRatio + direction * step);
  }
}

function isCapturedRepeatEnabled(): boolean {
  const repeatInput = document.getElementById("riff-capture-repeat") as HTMLInputElement | null;
  return Boolean(repeatInput?.checked);
}

function isCaptureMetronomeClickEnabled(): boolean {
  const input = document.getElementById("riff-capture-enable-click") as HTMLInputElement | null;
  return input ? input.checked : (uiState.riffCapture?.metronomeClickEnabled ?? true);
}

export function handleCapturedPreviewComplete(previewId: string): boolean {
  if (!isCapturedPreviewId(previewId)) {
    return false;
  }
  if (!isCapturedRepeatEnabled() || !uiState.riffCapture?.hasAudio) {
    return false;
  }

  const range = clampTrimRange(trimStartRatio, trimEndRatio);
  previewCapturedRiffRange(range.start, range.end);
  appendLog("riff preview repeat loop");
  return true;
}

export function handleSavedRiffPreviewComplete(previewId: string): boolean {
  if (!previewId || activeRiffTakePreviewId !== previewId || !isLibraryTakeId(previewId)) {
    return false;
  }
  previewRiffTake(previewId);
  appendLog(`riff preview repeat loop → ${previewId}`);
  return true;
}

export function handleRiffPreviewPlayback(phase: "start" | "stop", previewId: string): void {
  const capturedPreview = isCapturedPreviewId(previewId);
  const libraryTakePreview = isLibraryTakeId(previewId);

  if (phase === "start") {
    if (capturedPreview) {
      activeRiffTakePreviewId = "";
      renderRiffTakePreviewButtons();
      startCapturedPreviewAnimation();
      return;
    }
    if (libraryTakePreview) {
      stopCapturedPreviewAnimation();
      renderCapturedWaveform();
      activeRiffTakePreviewId = previewId;
      renderRiffTakePreviewButtons();
    }
    return;
  }

  if (capturedPreview) {
    stopCapturedPreviewAnimation();
    renderCapturedWaveform();
  }
  if (!previewId || activeRiffTakePreviewId === previewId || libraryTakePreview) {
    activeRiffTakePreviewId = "";
    renderRiffTakePreviewButtons();
  }
}

function renderCapturedWaveform(): void {
  const canvas = document.getElementById("riff-capture-waveform") as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const isRecording = Boolean(uiState.riffCapture?.active);
  // During recording show live peaks; after capture show finalized peaks
  const peaks = isRecording && liveWaveformPeaks.length > 0
    ? liveWaveformPeaks
    : (uiState.riffCapture?.waveformPeaks ?? []);
  const hasAudio = peaks.length > 0 && (Boolean(uiState.riffCapture?.hasAudio) || isRecording);
  // For live rendering, compute how far through the 16-bar max buffer we are
  // 64 bars is the max capture buffer allocated by armRiffCapture
  const liveFillRatio = isRecording && uiState.riffCapture && uiState.riffCapture.sampleRate > 0
    ? Math.min(1, uiState.riffCapture.capturedSamples / Math.max(1, liveWaveformPeaks.length
        * Math.max(1, Math.ceil((uiState.riffCapture.sampleRate
          * (60 / Math.max(1, uiState.riffCapture.tempoBpm))
          * (4 / Math.max(1, uiState.riffCapture.timeSigDen))
          * uiState.riffCapture.timeSigNum * 64) / 256))))
    : 1;

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

  ctx.strokeStyle = "rgba(255,255,255,0.22)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, Math.floor(height / 2));
  ctx.lineTo(width, Math.floor(height / 2));
  ctx.stroke();

  if (!hasAudio) {
    ctx.fillStyle = "rgba(255,255,255,0.55)";
    ctx.font = "12px sans-serif";
    ctx.fillText(isArmed ? "Waiting for signal…" : "No capture yet", 10, Math.floor(height / 2) - 8);
    return;
  }

  const centerY = height / 2;
  const step = width / peaks.length;

  // For live recording: shade the un-recorded portion (right side)
  const recordedX = isRecording ? Math.max(0, Math.min(width, liveFillRatio * width)) : width;

  const trimRange = clampTrimRange(trimStartRatio, trimEndRatio);
  const trimStartX = isRecording ? 0 : Math.max(0, Math.min(width - 1, trimRange.start * width));
  const trimEndX = isRecording ? recordedX : Math.max(0, Math.min(width - 1, trimRange.end * width));

  if (!isRecording) {
    ctx.fillStyle = "rgba(0,0,0,0.20)";
    if (trimStartX > 0) {
      ctx.fillRect(0, 0, trimStartX, height);
    }
    if (trimEndX < width) {
      ctx.fillRect(trimEndX, 0, width - trimEndX, height);
    }
  } else {
    // Shade un-recorded region
    ctx.fillStyle = "rgba(0,0,0,0.35)";
    if (recordedX < width) {
      ctx.fillRect(recordedX, 0, width - recordedX, height);
    }
  }

  ctx.strokeStyle = isRecording ? "rgba(255, 100, 80, 0.95)" : "rgba(101, 186, 255, 0.95)";
  ctx.lineWidth = Math.max(1, step * 0.7);
  ctx.beginPath();

  peaks.forEach((peak, index) => {
    const x = index * step + step / 2;
    if (isRecording && x > recordedX) {
      return; // don't draw unrecorded region
    }
    const amp = Math.max(1, Math.min(centerY - 2, peak * (centerY - 2)));
    ctx.moveTo(x, centerY - amp);
    ctx.lineTo(x, centerY + amp);
  });
  ctx.stroke();

  if (!isRecording) {
    ctx.strokeStyle = "rgba(255, 204, 102, 0.95)";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(trimStartX, 0);
    ctx.lineTo(trimStartX, height);
    ctx.moveTo(trimEndX, 0);
    ctx.lineTo(trimEndX, height);
    ctx.stroke();

    ctx.fillStyle = "rgba(255, 204, 102, 0.95)";
    ctx.beginPath();
    ctx.arc(trimStartX, centerY, 4, 0, Math.PI * 2);
    ctx.arc(trimEndX, centerY, 4, 0, Math.PI * 2);
    ctx.fill();

    const selectedX = selectedTrimHandle === "start" ? trimStartX : trimEndX;
    ctx.strokeStyle = "rgba(255,255,255,0.95)";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(selectedX, centerY, 6, 0, Math.PI * 2);
    ctx.stroke();

    if (capturedPreviewActive) {
      const playheadX = Math.max(trimStartX, Math.min(trimEndX, trimStartX + capturedPreviewProgress * (trimEndX - trimStartX)));
      ctx.strokeStyle = "rgba(255, 204, 102, 0.95)";
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(playheadX, 0);
      ctx.lineTo(playheadX, height);
      ctx.stroke();
    }
  } else {
    // Draw recording head at current position
    ctx.strokeStyle = "rgba(255, 80, 80, 0.9)";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(recordedX, 0);
    ctx.lineTo(recordedX, height);
    ctx.stroke();
  }
}

function renderCapturedPlayButton(): void {
  const playCaptureBtn = document.getElementById("riff-capture-play") as HTMLButtonElement | null;
  if (!playCaptureBtn) {
    return;
  }

  const hasAudio = Boolean(uiState.riffCapture?.hasAudio);
  const hideWhileRecording = Boolean(uiState.riffCapture?.active) || isArmed;
  playCaptureBtn.disabled = !hasAudio;
  playCaptureBtn.textContent = capturedPreviewActive ? "■" : "▶";
  playCaptureBtn.classList.add("riff-icon-btn");
  playCaptureBtn.classList.toggle("hidden", hideWhileRecording);

  const trimBtn = document.getElementById("riff-capture-trim") as HTMLButtonElement | null;
  if (trimBtn) {
    trimBtn.disabled = !hasAudio;
  }
}

function renderRiffTakePreviewButtons(): void {
  const list = document.getElementById("riff-library-list");
  if (!list) {
    return;
  }
  const buttons = Array.from(list.querySelectorAll<HTMLButtonElement>(".riff-preview-btn"));
  buttons.forEach((button) => {
    const takeId = button.dataset.takeId ?? "";
    const active = Boolean(takeId) && takeId === activeRiffTakePreviewId;
    button.textContent = active ? "■" : "▶";
  });
}

async function importDroppedRiffWav(file: File): Promise<void> {
  if (!file.name.toLowerCase().endsWith(".wav") && file.type !== "audio/wav" && file.type !== "audio/x-wav") {
    showNotification("Only WAV files are supported for riff import");
    return;
  }

  const dataBuffer = await file.arrayBuffer();
  const wavInfo = parseWavMetadata(dataBuffer);
  if (!wavInfo) {
    showNotification("Invalid WAV file", "Could not parse WAV metadata");
    return;
  }

  const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
  const numInput = document.getElementById("riff-capture-timesig-num") as HTMLInputElement | null;
  const denInput = document.getElementById("riff-capture-timesig-den") as HTMLInputElement | null;
  const patternTypeSelect = document.getElementById("riff-capture-pattern-type") as HTMLSelectElement | null;
  const patternIdInput = document.getElementById("riff-capture-pattern-id") as HTMLInputElement | null;

  const tempoBpm = Math.max(30, Math.min(300, Number(tempoInput?.value ?? 120)));
  const timeSigNum = Math.max(1, Number(numInput?.value ?? 4));
  const timeSigDen = Math.max(1, Number(denInput?.value ?? 4));
  // Compute bars from WAV duration and tempo rather than relying on user input
  const bars = computeBarsFromSamples(wavInfo.numFrames, wavInfo.sampleRate, tempoBpm, timeSigNum, timeSigDen);
  const patternType = (patternTypeSelect?.value === "drum" ? "drum" : "click") as "click" | "drum";
  const patternId = patternIdInput?.value.trim() || undefined;

  importRiffWav({
    data: arrayBufferToBase64(dataBuffer),
    fileName: file.name,
    tempoBpm,
    timeSigNum,
    timeSigDen,
    bars,
    patternType,
    patternId,
  });

  appendLog(`riff wav import requested → ${file.name} (${wavInfo.sampleRate} Hz, ${wavInfo.channels} ch, ${bars} bars)`);
  showNotification("Importing WAV into current take", file.name);
}

function renderRiffCaptureStatus(): void {
  const status = document.getElementById("riff-capture-status");
  if (!status) {
    return;
  }
  const capture = uiState.riffCapture;
  const recordBtn = document.getElementById("riff-capture-record-toggle") as HTMLButtonElement | null;

  // Toggle recording/armed class on modal content for visual indicator
  const modalContent = document.querySelector<HTMLElement>(".riff-capture-modal-content");
  modalContent?.classList.toggle("recording", Boolean(capture?.active));
  modalContent?.classList.toggle("armed", isArmed && !Boolean(capture?.active));

  // Show inline save fields when a take has been captured; disable Save Take until then
  const saveSectionEl = document.getElementById("riff-capture-save-section") as HTMLElement | null;
  const saveTakeBtn = document.getElementById("riff-capture-modal-save-take") as HTMLButtonElement | null;
  const hasAudio = Boolean(capture?.hasAudio);
  if (saveSectionEl) {
    saveSectionEl.style.display = hasAudio ? "block" : "none";
    if (hasAudio) {
      const titleInput = saveSectionEl.querySelector<HTMLInputElement>("#riff-capture-save-title");
      if (titleInput && !titleInput.value) {
        const riffs = uiState.riffLibrary?.riffs ?? [];
        titleInput.value = `riff-${riffs.length + 1}`;
        titleInput.select();
      }
    }
  }
  if (saveTakeBtn) saveTakeBtn.disabled = !hasAudio;

  if (!capture) {
    status.textContent = "Idle";
    if (recordBtn) {
      recordBtn.textContent = "● Record";
      recordBtn.classList.add("riff-record-btn");
      recordBtn.classList.remove("recording", "armed");
    }
    return;
  }

  if (isArmed && !capture.active) {
    const countIn = capture.tempoBpm > 0
      ? `${capture.tempoBpm.toFixed(1)} BPM · count-in active`
      : "armed";
    status.textContent = `Armed · ${countIn} · waiting for signal…`;
    if (recordBtn) {
      recordBtn.textContent = "■ Stop";
      recordBtn.classList.add("riff-record-btn", "armed");
      recordBtn.classList.remove("recording");
    }
    return;
  }

  if (capture.active) {
    const capturedSec = capture.sampleRate > 0 ? capture.capturedSamples / capture.sampleRate : 0;
    const barText = capturedSec > 0 && capture.tempoBpm > 0
      ? `~${computeBarsFromSamples(capture.capturedSamples, capture.sampleRate, capture.tempoBpm, capture.timeSigNum, capture.timeSigDen)} bar(s)`
      : `${capture.bars} bar(s)`;
    status.textContent = `Recording · ${barText} @ ${capture.tempoBpm.toFixed(1)} BPM`;
    if (recordBtn) {
      recordBtn.textContent = "■ Stop";
      recordBtn.classList.add("riff-record-btn", "recording");
      recordBtn.classList.remove("armed");
    }
    return;
  }

  if (capture.complete && capture.takeId) {
    const seconds = capture.sampleRate > 0 ? capture.capturedSamples / capture.sampleRate : 0;
    status.textContent = `Captured ${capture.takeId} (${formatDuration(seconds)}, ${capture.bars} bar(s))`;
    if (recordBtn) {
      recordBtn.textContent = "● Record";
      recordBtn.classList.add("riff-record-btn");
      recordBtn.classList.remove("recording", "armed");
    }
    return;
  }

  status.textContent = "Idle";
  if (recordBtn) {
    recordBtn.textContent = "● Record";
    recordBtn.classList.add("riff-record-btn");
    recordBtn.classList.remove("recording", "armed");
  }
}

function computeBarsFromSamples(capturedSamples: number, sampleRate: number, tempoBpm: number, timeSigNum: number, timeSigDen: number): number {
  if (sampleRate <= 0 || tempoBpm <= 0 || timeSigNum <= 0 || timeSigDen <= 0) {
    return 1;
  }
  const samplesPerBeat = sampleRate * (60 / tempoBpm) * (4 / timeSigDen);
  const samplesPerBar = samplesPerBeat * timeSigNum;
  return Math.max(1, Math.round(capturedSamples / samplesPerBar));
}

/** Compute bar count from the current trim window + tempo inputs. Used for saving. */
function computeBarsFromTrimWindow(): number {
  const trimDurationSec = getTrimWindowDurationSec();
  if (trimDurationSec <= 0) {
    return uiState.riffCapture?.bars ?? 1;
  }
  const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
  const numInput = document.getElementById("riff-capture-timesig-num") as HTMLInputElement | null;
  const denInput = document.getElementById("riff-capture-timesig-den") as HTMLInputElement | null;
  const tempo = Math.max(30, Math.min(300, Number(tempoInput?.value ?? uiState.riffCapture?.tempoBpm ?? 120)));
  const timeSigNum = Math.max(1, Number(numInput?.value ?? uiState.riffCapture?.timeSigNum ?? 4));
  const timeSigDen = Math.max(1, Number(denInput?.value ?? uiState.riffCapture?.timeSigDen ?? 4));
  const sampleRate = uiState.riffCapture?.sampleRate ?? 44100;
  const trimSamples = Math.round(trimDurationSec * sampleRate);
  return computeBarsFromSamples(trimSamples, sampleRate, tempo, timeSigNum, timeSigDen);
}


function bindRiffLibraryActions(): void {
  const pathInput = document.getElementById("riff-library-path") as HTMLInputElement | null;
  const pathSaveBtn = document.getElementById("riff-library-path-save") as HTMLButtonElement | null;
  const refreshBtn = document.getElementById("riff-library-refresh") as HTMLButtonElement | null;
  const openCaptureModalBtn = document.getElementById("riff-open-capture-modal") as HTMLButtonElement | null;
  const footerRecordButton = document.getElementById("footer-riff-record-btn") as HTMLButtonElement | null;
  const captureModal = document.getElementById("riff-capture-modal") as HTMLDivElement | null;
  const captureModalCloseBtn = document.getElementById("riff-capture-modal-close") as HTMLButtonElement | null;
  const captureModalCancelBtn = document.getElementById("riff-capture-modal-cancel") as HTMLButtonElement | null;
  const recordToggleBtn = document.getElementById("riff-capture-record-toggle") as HTMLButtonElement | null;
  const playCaptureBtn = document.getElementById("riff-capture-play") as HTMLButtonElement | null;
  const trimButton = document.getElementById("riff-capture-trim") as HTMLButtonElement | null;
  const openSaveModalBtn = document.getElementById("riff-open-save-modal") as HTMLButtonElement | null;
  const saveModal = document.getElementById("riff-save-modal") as HTMLDivElement | null;
  const saveModalCloseBtn = document.getElementById("riff-save-modal-close") as HTMLButtonElement | null;
  const saveModalCancelBtn = document.getElementById("riff-save-modal-cancel") as HTMLButtonElement | null;
  const saveModalConfirmBtn = document.getElementById("riff-save-modal-confirm") as HTMLButtonElement | null;
  const saveModalTitle = document.getElementById("riff-save-modal-title") as HTMLHeadingElement | null;

  if (pathSaveBtn && pathSaveBtn.dataset.bound !== "true") {
    pathSaveBtn.dataset.bound = "true";
    pathSaveBtn.addEventListener("click", () => {
      const path = pathInput?.value.trim() ?? "";
      if (!path) {
        showNotification("Riff Library path is required");
        return;
      }
      setRiffLibraryPath(path);
      appendLog(`riff library path → ${path}`);
    });
  }

  if (refreshBtn && refreshBtn.dataset.bound !== "true") {
    refreshBtn.dataset.bound = "true";
    refreshBtn.addEventListener("click", () => {
      getRiffLibrary();
    });
  }

  const syncMetronomeBtn = document.getElementById("riff-sync-metronome-btn") as HTMLButtonElement | null;
  if (syncMetronomeBtn && syncMetronomeBtn.dataset.bound !== "true") {
    syncMetronomeBtn.dataset.bound = "true";
    syncMetronomeBtn.addEventListener("click", () => {
      const bpm = uiState.metronome?.bpm;
      if (!bpm || bpm <= 0) {
        showNotification("Metronome is not active");
        return;
      }
      const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
      if (tempoInput) {
        tempoInput.value = String(Math.round(bpm * 10) / 10);
      }
      appendLog(`riff capture tempo synced from metronome → ${bpm.toFixed(1)} BPM`);
    });
  }

  if (openCaptureModalBtn && openCaptureModalBtn.dataset.bound !== "true") {
    openCaptureModalBtn.dataset.bound = "true";
    openCaptureModalBtn.addEventListener("click", () => {
      openRiffCaptureModal();
    });
  }

  if (footerRecordButton && footerRecordButton.dataset.bound !== "true") {
    footerRecordButton.dataset.bound = "true";
    footerRecordButton.addEventListener("click", () => {
      openRiffCaptureModal();
      if (!startRiffCaptureFromModal()) {
        appendLog("riff capture footer rec ignored — capture already active");
      }
    });
  }

  if (captureModalCloseBtn && captureModalCloseBtn.dataset.bound !== "true") {
    captureModalCloseBtn.dataset.bound = "true";
    captureModalCloseBtn.addEventListener("click", () => {
      closeRiffCaptureModal();
    });
  }

  if (captureModalCancelBtn && captureModalCancelBtn.dataset.bound !== "true") {
    captureModalCancelBtn.dataset.bound = "true";
    captureModalCancelBtn.addEventListener("click", () => {
      closeRiffCaptureModal();
    });
  }

  if (captureModal && captureModal.dataset.bound !== "true") {
    captureModal.dataset.bound = "true";
    // Intentionally no backdrop-click dismiss: dragging waveform trim markers outside the
    // dialog would otherwise close it. Only the Cancel button or a completed save closes this modal.
  }

  const openSaveModal = (editing = false) => {
    if (!saveModal) {
      return;
    }
    if (saveModalTitle) {
      saveModalTitle.textContent = editing ? "Edit Riff Take" : "Save Take";
    }
    saveModal.style.display = "flex";
  };

  const closeSaveModal = () => {
    if (!saveModal) {
      return;
    }
    saveModal.style.display = "none";
    editingRiffId = "";
  };

  if (openSaveModalBtn && openSaveModalBtn.dataset.bound !== "true") {
    openSaveModalBtn.dataset.bound = "true";
    openSaveModalBtn.addEventListener("click", () => {
      editingRiffId = "";
      savingFromCapture = false;
      populateSaveModalFields();
      openSaveModal(false);
    });
  }

  const captureModalSaveTakeBtn = document.getElementById("riff-capture-modal-save-take") as HTMLButtonElement | null;
  if (captureModalSaveTakeBtn && captureModalSaveTakeBtn.dataset.bound !== "true") {
    captureModalSaveTakeBtn.dataset.bound = "true";
    captureModalSaveTakeBtn.addEventListener("click", () => {
      if (!uiState.riffCapture?.hasAudio) {
        showNotification("No captured take available yet");
        return;
      }
      const titleInput = document.getElementById("riff-capture-save-title") as HTMLInputElement | null;
      const categoriesInput = document.getElementById("riff-capture-save-categories") as HTMLInputElement | null;
      const tagsInput = document.getElementById("riff-capture-save-tags") as HTMLInputElement | null;
      const notesInput = document.getElementById("riff-capture-save-notes") as HTMLInputElement | null;
      const favoriteInput = document.getElementById("riff-capture-save-favorite") as HTMLInputElement | null;
      const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
      const numInput = document.getElementById("riff-capture-timesig-num") as HTMLInputElement | null;
      const denInput = document.getElementById("riff-capture-timesig-den") as HTMLInputElement | null;
      const patternTypeSelect = document.getElementById("riff-capture-pattern-type") as HTMLSelectElement | null;
      const patternIdInput = document.getElementById("riff-capture-pattern-id") as HTMLInputElement | null;

      const title = titleInput?.value.trim() ?? "";
      if (!title) {
        showNotification("Riff title is required");
        titleInput?.focus();
        return;
      }

      saveRiffTake({
        riffId: undefined,
        title,
        categories: splitCsv(categoriesInput?.value ?? ""),
        tags: splitCsv(tagsInput?.value ?? ""),
        notes: notesInput?.value ?? "",
        favorite: Boolean(favoriteInput?.checked),
        tempoBpm: Math.max(30, Math.min(300, Number(tempoInput?.value ?? uiState.riffCapture?.tempoBpm ?? 120))),
        timeSigNum: Math.max(1, Number(numInput?.value ?? uiState.riffCapture?.timeSigNum ?? 4)),
        timeSigDen: Math.max(1, Number(denInput?.value ?? uiState.riffCapture?.timeSigDen ?? 4)),
        bars: computeBarsFromTrimWindow(),
        metronomeClickEnabled: isCaptureMetronomeClickEnabled(),
        patternType: (patternTypeSelect?.value === "drum" ? "drum" : "click") as "click" | "drum",
        patternId: patternIdInput?.value.trim() ?? "",
        presetId: uiState.activePresetId ?? undefined,
      });
      closeRiffCaptureModal();
      appendLog(`riff save requested \u2192 ${title} (${computeBarsFromTrimWindow()} bars)`);
    });
  }

  if (saveModalCloseBtn && saveModalCloseBtn.dataset.bound !== "true") {
    saveModalCloseBtn.dataset.bound = "true";
    saveModalCloseBtn.addEventListener("click", closeSaveModal);
  }

  if (saveModalCancelBtn && saveModalCancelBtn.dataset.bound !== "true") {
    saveModalCancelBtn.dataset.bound = "true";
    saveModalCancelBtn.addEventListener("click", closeSaveModal);
  }

  if (saveModal && saveModal.dataset.bound !== "true") {
    saveModal.dataset.bound = "true";
    saveModal.addEventListener("mousedown", (event) => {
      if (event.target === saveModal) {
        closeSaveModal();
      }
    });
  }

  if (recordToggleBtn && recordToggleBtn.dataset.bound !== "true") {
    recordToggleBtn.dataset.bound = "true";
    recordToggleBtn.addEventListener("click", () => {
      if (isArmed || uiState.riffCapture?.active) {
        const cancel = isArmed && !uiState.riffCapture?.active;
        isArmed = false;
        stopRiffCapture(cancel);
        appendLog("riff capture stop");
        renderRiffCaptureStatus();
        return;
      }
      startRiffCaptureFromModal();
    });
  }

  const metroVolumeSlider = document.getElementById("riff-capture-metro-volume") as HTMLInputElement | null;
  const metroVolumeLabel = document.getElementById("riff-capture-metro-volume-label") as HTMLElement | null;
  if (metroVolumeSlider && metroVolumeSlider.dataset.bound !== "true") {
    metroVolumeSlider.dataset.bound = "true";
    metroVolumeSlider.addEventListener("input", () => {
      const db = parseFloat(metroVolumeSlider.value);
      if (metroVolumeLabel) metroVolumeLabel.textContent = `${db} dB`;
      setMetronome({ volumeDb: db });
    });
  }

  if (saveModalConfirmBtn && saveModalConfirmBtn.dataset.bound !== "true") {
    saveModalConfirmBtn.dataset.bound = "true";
    saveModalConfirmBtn.addEventListener("click", () => {
      if (!uiState.riffCapture?.hasAudio) {
        showNotification("No captured take available yet");
        return;
      }
      const titleInput = document.getElementById("riff-save-title") as HTMLInputElement | null;
      const categoriesInput = document.getElementById("riff-save-categories") as HTMLInputElement | null;
      const tagsInput = document.getElementById("riff-save-tags") as HTMLInputElement | null;
      const notesInput = document.getElementById("riff-save-notes") as HTMLInputElement | null;
      const favoriteInput = document.getElementById("riff-save-favorite") as HTMLInputElement | null;
      const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
      const numInput = document.getElementById("riff-capture-timesig-num") as HTMLInputElement | null;
      const denInput = document.getElementById("riff-capture-timesig-den") as HTMLInputElement | null;
      const patternTypeSelect = document.getElementById("riff-capture-pattern-type") as HTMLSelectElement | null;
      const patternIdInput = document.getElementById("riff-capture-pattern-id") as HTMLInputElement | null;

      const title = titleInput?.value.trim() ?? "";
      if (!title) {
        showNotification("Riff title is required");
        return;
      }

      saveRiffTake({
        riffId: editingRiffId || undefined,
        title,
        categories: splitCsv(categoriesInput?.value ?? ""),
        tags: splitCsv(tagsInput?.value ?? ""),
        notes: notesInput?.value ?? "",
        favorite: Boolean(favoriteInput?.checked),
        tempoBpm: Math.max(30, Math.min(300, Number(tempoInput?.value ?? uiState.riffCapture?.tempoBpm ?? 120))),
        timeSigNum: Math.max(1, Number(numInput?.value ?? uiState.riffCapture?.timeSigNum ?? 4)),
        timeSigDen: Math.max(1, Number(denInput?.value ?? uiState.riffCapture?.timeSigDen ?? 4)),
        bars: computeBarsFromTrimWindow(),
        metronomeClickEnabled: isCaptureMetronomeClickEnabled(),
        patternType: (patternTypeSelect?.value === "drum" ? "drum" : "click") as "click" | "drum",
        patternId: patternIdInput?.value.trim() ?? "",
        presetId: uiState.activePresetId ?? undefined,
      });
      const fromCapture = savingFromCapture;
      closeSaveModal();
      if (fromCapture) {
        closeRiffCaptureModal();
        savingFromCapture = false;
      }
      appendLog(`riff save requested → ${title}`);
    });
  }

  const appRoot = document.getElementById("app");
  if (appRoot && appRoot.dataset.riffDropBound !== "true") {
    appRoot.dataset.riffDropBound = "true";

    const setActive = (active: boolean) => {
      appRoot.classList.toggle("riff-drop-active", active);
    };

    document.addEventListener("dragenter", (event) => {
      if (!hasFilePayload(event)) {
        return;
      }
      event.preventDefault();
      setActive(true);
    });

    document.addEventListener("dragover", (event) => {
      if (!hasFilePayload(event)) {
        return;
      }
      event.preventDefault();
      setActive(true);
    });

    document.addEventListener("dragleave", (event) => {
      const related = event.relatedTarget as Node | null;
      if (!related || !appRoot.contains(related)) {
        setActive(false);
      }
    });

    document.addEventListener("dragend", () => {
      setActive(false);
    });

    document.addEventListener("drop", async (event) => {
      if (!hasFilePayload(event)) {
        return;
      }
      event.preventDefault();
      setActive(false);

      const files = Array.from(event.dataTransfer?.files ?? []);
      if (!files.length) {
        return;
      }

      // Route ZIP files to the pack/preset importer.
      const zipFile = files.find((file) =>
        file.name.toLowerCase().endsWith(".zip") || file.type === "application/zip"
      );
      if (zipFile) {
        await importPackWithConfirmation(zipFile, { source: "zipImport" });
        return;
      }

      const wavFile = files.find((file) => file.name.toLowerCase().endsWith(".wav")
        || file.type === "audio/wav"
        || file.type === "audio/x-wav");
      if (!wavFile) {
        showNotification("Only WAV files are supported for riff import");
        return;
      }

      openRiffCaptureModal();
      await importDroppedRiffWav(wavFile);
    });
  }

  if (playCaptureBtn && playCaptureBtn.dataset.bound !== "true") {
    playCaptureBtn.dataset.bound = "true";
    playCaptureBtn.addEventListener("click", () => {
      if (!uiState.riffCapture?.hasAudio) {
        showNotification("No captured take available yet");
        return;
      }
      if (capturedPreviewActive) {
        stopCapturedPreviewAnimation();
        renderCapturedWaveform();
        stopPreviewPlayback();
        appendLog("riff preview stop requested");
        return;
      }
      const trimRange = clampTrimRange(trimStartRatio, trimEndRatio);
      previewCapturedRiffRange(trimRange.start, trimRange.end);
      appendLog("riff preview captured take");
    });
  }

  if (trimButton && trimButton.dataset.bound !== "true") {
    trimButton.dataset.bound = "true";
    trimButton.addEventListener("click", () => {
      if (!uiState.riffCapture?.hasAudio) {
        showNotification("No captured take available yet");
        return;
      }
      const trimRange = clampTrimRange(trimStartRatio, trimEndRatio);
      trimCapturedRiff(trimRange.start, trimRange.end);
      appendLog(`riff crop to markers → ${trimRange.start.toFixed(3)}-${trimRange.end.toFixed(3)}`);
    });
  }

  const waveform = document.getElementById("riff-capture-waveform") as HTMLCanvasElement | null;
  if (waveform && waveform.dataset.bound !== "true") {
    waveform.dataset.bound = "true";

    waveform.addEventListener("mousedown", (event) => {
      if (!uiState.riffCapture?.hasAudio) {
        return;
      }
      const pointerRatio = getCanvasRatioFromPointer(event, waveform);
      const startDist = Math.abs(pointerRatio - trimStartRatio);
      const endDist = Math.abs(pointerRatio - trimEndRatio);
      activeTrimHandle = startDist <= endDist ? "start" : "end";
      selectedTrimHandle = activeTrimHandle;
      waveform.focus();
      event.preventDefault();
    });

    window.addEventListener("mousemove", (event) => {
      if (!activeTrimHandle || !uiState.riffCapture?.hasAudio) {
        return;
      }
      const pointerRatio = getCanvasRatioFromPointer(event, waveform);
      if (activeTrimHandle === "start") {
        setTrimRange(pointerRatio, trimEndRatio);
      } else {
        setTrimRange(trimStartRatio, pointerRatio);
      }
    });

    window.addEventListener("mouseup", () => {
      activeTrimHandle = null;
    });

    waveform.addEventListener("keydown", (event) => {
      if (!uiState.riffCapture?.hasAudio) {
        return;
      }
      if (event.key === "ArrowLeft") {
        event.preventDefault();
        nudgeSelectedTrimHandle(-1, event.shiftKey);
        return;
      }
      if (event.key === "ArrowRight") {
        event.preventDefault();
        nudgeSelectedTrimHandle(1, event.shiftKey);
        return;
      }
      if (event.key === "ArrowUp" || event.key === "ArrowDown") {
        event.preventDefault();
        selectedTrimHandle = selectedTrimHandle === "start" ? "end" : "start";
        renderCapturedWaveform();
        return;
      }
      if (event.key === "Home") {
        event.preventDefault();
        if (selectedTrimHandle === "start") {
          setTrimRange(0, trimEndRatio);
        } else {
          setTrimRange(trimStartRatio, Math.max(trimStartRatio + 0.001, 0.001));
        }
        return;
      }
      if (event.key === "End") {
        event.preventDefault();
        if (selectedTrimHandle === "start") {
          setTrimRange(Math.min(trimEndRatio - 0.001, 0.999), trimEndRatio);
        } else {
          setTrimRange(trimStartRatio, 1);
        }
      }
    });
  }

  const list = document.getElementById("riff-library-list");
  if (list && list.dataset.bound !== "true") {
    list.dataset.bound = "true";
    list.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      if (!target) {
        return;
      }

      const previewButton = target.closest<HTMLButtonElement>(".riff-preview-btn");
      if (previewButton) {
        const takeId = previewButton.dataset.takeId ?? "";
        if (takeId) {
          if (activeRiffTakePreviewId === takeId) {
            activeRiffTakePreviewId = "";
            renderRiffTakePreviewButtons();
            stopPreviewPlayback();
            appendLog(`riff preview stop requested → ${takeId}`);
            return;
          }
          previewRiffTake(takeId);
          appendLog(`riff preview → ${takeId}`);
        }
        return;
      }

      const editButton = target.closest<HTMLButtonElement>(".riff-edit-btn");
      if (editButton) {
        const riffId = editButton.dataset.riffId ?? "";
        const takeId = editButton.dataset.takeId ?? "";
        const riff = (uiState.riffLibrary?.riffs ?? []).find((entry) => entry.id === riffId);
        if (!riff) {
          showNotification("Riff not found");
          return;
        }
        editingRiffId = riffId;
        savingFromCapture = false;
        populateSaveModalFields(riff);
        openSaveModal(true);

        const take = riff.takes.find((entry) => entry.id === takeId) ?? riff.takes[0];
        if (take) {
          const tempoInput = document.getElementById("riff-capture-tempo") as HTMLInputElement | null;
          const numInput = document.getElementById("riff-capture-timesig-num") as HTMLInputElement | null;
          const denInput = document.getElementById("riff-capture-timesig-den") as HTMLInputElement | null;
          const patternTypeSelect = document.getElementById("riff-capture-pattern-type") as HTMLSelectElement | null;
          const patternIdInput = document.getElementById("riff-capture-pattern-id") as HTMLInputElement | null;
          const clickEnabledInput = document.getElementById("riff-capture-enable-click") as HTMLInputElement | null;

          if (tempoInput) tempoInput.value = String(Math.round(take.tempoBpm ?? 120));
          if (numInput) numInput.value = String(take.timeSigNum ?? 4);
          if (denInput) denInput.value = String(take.timeSigDen ?? 4);
          if (patternTypeSelect) patternTypeSelect.value = take.patternType === "drum" ? "drum" : "click";
          if (patternIdInput) patternIdInput.value = take.patternId ?? "";
          if (clickEnabledInput) clickEnabledInput.checked = take.metronomeClickEnabled ?? true;

          loadRiffTakeForEdit(take.id);
          appendLog(`riff edit load requested → ${take.id}`);
        }
        return;
      }

      const favoriteButton = target.closest<HTMLButtonElement>(".riff-favorite-btn");
      if (favoriteButton) {
        const riffId = favoriteButton.dataset.riffId ?? "";
        const nextFavorite = favoriteButton.dataset.favorite !== "true";
        if (riffId) {
          setRiffFavorite(riffId, nextFavorite);
        }
        return;
      }

      const usedButton = target.closest<HTMLButtonElement>(".riff-used-btn");
      if (usedButton) {
        const riffId = usedButton.dataset.riffId ?? "";
        const currentlyUsed = usedButton.dataset.used === "true";
        if (riffId) {
          markRiffUsed(riffId, !currentlyUsed, "");
        }
        return;
      }

      const deleteButton = target.closest<HTMLButtonElement>(".riff-delete-btn");
      if (deleteButton) {
        const riffId = deleteButton.dataset.riffId ?? "";
        const riffTitle = (uiState.riffLibrary?.riffs ?? []).find((r) => r.id === riffId)?.title ?? riffId;
        if (riffId) {
          void showConfirm(`Delete "${riffTitle}"? This cannot be undone.`, "Delete riff").then((confirmed) => {
            if (confirmed) deleteRiff(riffId);
          });
        }
      }
    });
  }
}

export function renderRiffLibraryPanel(): void {
  const pathInput = document.getElementById("riff-library-path") as HTMLInputElement | null;
  const list = document.getElementById("riff-library-list");
  if (pathInput) {
    pathInput.value = uiState.riffLibrary?.path ?? "";
  }

  if (!list) {
    return;
  }

  const riffs = uiState.riffLibrary?.riffs ?? [];
  if (!riffs.length) {
    list.innerHTML = "<div class=\"equipment-library-empty\">No riffs saved yet.</div>";
    renderRiffCaptureStatus();
    syncTrimControlsFromState();
    renderCapturedWaveform();
    renderCapturedPlayButton();
    renderRiffTakePreviewButtons();
    bindRiffLibraryActions();
    return;
  }

  list.innerHTML = riffs
    .map((riff) => {
      const takeId = getPreferredTakeId(riff);
      const take = riff.takes.find((entry) => entry.id === takeId) ?? riff.takes[0];
      const tagText = riff.tags?.length ? riff.tags.join(", ") : "—";
      const categoriesText = riff.categories?.length ? riff.categories.join(", ") : "—";
      const usedText = riff.used ? `Used${riff.usedSongTitle ? ` (${riff.usedSongTitle})` : ""}` : "Unused";
      const takeSummary = take
        ? `${take.bars} bar(s) · ${take.tempoBpm.toFixed(1)} BPM · ${take.timeSigNum}/${take.timeSigDen} · ${formatDuration(take.durationSec)}`
        : "No take";
      return `
        <div class="equipment-library-item riff-library-item">
          <div class="equipment-library-item-main">
            <div class="equipment-library-item-title">
              ${riff.title}
            </div>
            <div class="equipment-library-item-meta">
              <span>Categories: ${categoriesText}</span>
              <span>Tags: ${tagText}</span>
              <span>${usedText}</span>
              <span>Takes: ${riff.takes.length}</span>
            </div>
            <div class="equipment-library-item-path" title="${take?.filePath ?? ""}">${takeSummary}</div>
          </div>
          <div class="equipment-library-item-actions riff-library-actions">
            <button class="equipment-library-browse riff-preview-btn riff-icon-btn" data-take-id="${take?.id ?? ""}" ${take ? "" : "disabled"} title="Preview" aria-label="Preview">▶</button>
            <button class="equipment-library-browse riff-icon-btn riff-edit-btn" data-riff-id="${riff.id}" data-take-id="${take?.id ?? ""}" title="Edit" aria-label="Edit"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/></svg></button>
            <button class="equipment-library-browse riff-icon-btn riff-favorite-btn${riff.favorite ? " riff-fav-active" : ""}" data-riff-id="${riff.id}" data-favorite="${riff.favorite ? "true" : "false"}" title="${riff.favorite ? "Remove from favourites" : "Add to favourites"}" aria-label="${riff.favorite ? "Remove from favourites" : "Add to favourites"}"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="${riff.favorite ? "currentColor" : "none"}" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"/></svg></button>
            <button class="equipment-library-browse riff-icon-btn riff-used-btn${riff.used ? " riff-used-active" : ""}" data-riff-id="${riff.id}" data-used="${riff.used ? "true" : "false"}" title="${riff.used ? "Mark as unused" : "Mark as used"}" aria-label="${riff.used ? "Mark as unused" : "Mark as used"}"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${riff.used ? "<circle cx=\"12\" cy=\"12\" r=\"10\"/><line x1=\"15\" y1=\"9\" x2=\"9\" y2=\"15\"/><line x1=\"9\" y1=\"9\" x2=\"15\" y2=\"15\"/>" : "<circle cx=\"12\" cy=\"12\" r=\"10\"/><polyline points=\"9 12 11 14 15 10\"/>"}</svg></button>
            <button class="equipment-library-browse riff-icon-btn riff-delete-btn" data-riff-id="${riff.id}" title="Delete" aria-label="Delete"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg></button>
          </div>
        </div>
      `;
    })
    .join("");

  renderRiffCaptureStatus();
  syncTrimControlsFromState();
  renderCapturedWaveform();
  renderCapturedPlayButton();
  renderRiffTakePreviewButtons();
  bindRiffLibraryActions();
}

export function initializeRiffLibraryPanel(): void {
  bindRiffCaptureTabs();
  bindRiffLibraryActions();
  renderRiffCaptureStatus();
  syncTrimControlsFromState();
  renderCapturedWaveform();
  renderCapturedPlayButton();
  getRiffLibrary();
}
