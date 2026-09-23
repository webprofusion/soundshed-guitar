/**
 * Riff capture, the metronome, the practice-tool transport, demo renders and
 * resource previews — everything with a transport behind it.
 */

import { getRiffLibrary } from "../bridge.js";
import { onDemoAudioStarted, onDemoAudioStopped, refreshDemoAudioSelectors, syncDemoAudioSelectionFromPreview } from "../demoAudio.js";
import { appendLog } from "../logging.js";
import { applyMetronomeBeat } from "../metronome.js";
import { showNotification } from "../notifications.js";
import { applyPracticeToolFileLoaded, applyPracticeToolPlaybackEnded, applyPracticeToolTransportState } from "../practiceTool.js";
import { applyRiffCaptureProgress, applyRiffCaptureState, applyRiffLibraryState, handleCapturedPreviewComplete, handleRiffPreviewPlayback, handleSavedRiffPreviewComplete } from "../riffLibrary.js";
import { uiState } from "../state.js";
import type { RiffLibrary } from "../types.js";
import type { IncomingPayload } from "./types.js";

export function onMetronomeBeat(payload: IncomingPayload): void {
  const beat = payload as { beatIndex?: number; beatsPerBar?: number; level?: string };
  if (typeof beat.beatIndex !== "number") return;
  applyMetronomeBeat(beat.beatIndex);
}

export function onRiffCaptureProgress(payload: IncomingPayload): void {
  applyRiffCaptureProgress(
    (payload as { capturedSamples?: number }).capturedSamples ?? 0,
    Array.isArray((payload as { waveformPeaks?: unknown[] }).waveformPeaks)
      ? ((payload as { waveformPeaks?: unknown[] }).waveformPeaks as unknown[])
          .filter((value): value is number => typeof value === "number")
      : [],
  );
}

export function onRiffCaptureStarted(payload: IncomingPayload): void {
  appendLog(`riff capture started ← ${(payload as { takeId?: string }).takeId ?? "take"}`);
  applyRiffCaptureState({
    active: true,
    complete: false,
    takeId: (payload as { takeId?: string }).takeId ?? "",
    bars: (payload as { bars?: number }).bars ?? uiState.riffCapture?.bars ?? 1,
    tempoBpm: (payload as { tempoBpm?: number }).tempoBpm ?? uiState.riffCapture?.tempoBpm ?? 120,
    timeSigNum: (payload as { timeSigNum?: number }).timeSigNum ?? uiState.riffCapture?.timeSigNum ?? 4,
    timeSigDen: (payload as { timeSigDen?: number }).timeSigDen ?? uiState.riffCapture?.timeSigDen ?? 4,
    metronomeClickEnabled: typeof (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled === "boolean"
      ? (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled
      : uiState.riffCapture?.metronomeClickEnabled ?? true,
    hasAudio: false,
    waveformPeaks: [],
    barAlignOffsetSamples: typeof (payload as { barAlignOffsetSamples?: number }).barAlignOffsetSamples === "number"
      ? (payload as { barAlignOffsetSamples?: number }).barAlignOffsetSamples
      : 0,
  });
  showNotification("Riff capture started");
}

export function onRiffCaptureStopped(payload: IncomingPayload): void {
  appendLog(`riff capture stopped ← ${(payload as { takeId?: string }).takeId ?? "take"}`);
  const source = (payload as { source?: string }).source ?? "capture";
  applyRiffCaptureState({
    active: false,
    complete: true,
    takeId: (payload as { takeId?: string }).takeId ?? uiState.riffCapture?.takeId ?? "",
    bars: (payload as { bars?: number }).bars ?? uiState.riffCapture?.bars ?? 1,
    tempoBpm: (payload as { tempoBpm?: number }).tempoBpm ?? uiState.riffCapture?.tempoBpm ?? 120,
    timeSigNum: (payload as { timeSigNum?: number }).timeSigNum ?? uiState.riffCapture?.timeSigNum ?? 4,
    timeSigDen: (payload as { timeSigDen?: number }).timeSigDen ?? uiState.riffCapture?.timeSigDen ?? 4,
    metronomeClickEnabled: typeof (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled === "boolean"
      ? (payload as { metronomeClickEnabled?: boolean }).metronomeClickEnabled
      : uiState.riffCapture?.metronomeClickEnabled ?? true,
    capturedSamples: (payload as { capturedSamples?: number }).capturedSamples ?? uiState.riffCapture?.capturedSamples ?? 0,
    sampleRate: (payload as { sampleRate?: number }).sampleRate ?? uiState.riffCapture?.sampleRate ?? 0,
    hasAudio: Boolean((payload as { hasAudio?: boolean }).hasAudio),
    waveformPeaks: Array.isArray((payload as { waveformPeaks?: unknown[] }).waveformPeaks)
      ? ((payload as { waveformPeaks?: unknown[] }).waveformPeaks as unknown[])
          .filter((value): value is number => typeof value === "number")
      : [],
  });
  showNotification(
    source === "import"
      ? "Riff WAV imported"
      : source === "editLoad"
        ? "Riff take loaded for edit"
      : source === "trim"
        ? "Riff cropped to markers"
        : "Riff capture complete",
  );
}

export function onRiffCaptureCanceled(payload: IncomingPayload): void {
  appendLog(`riff capture cancelled ← ${(payload as { takeId?: string }).takeId ?? "take"}`);
  applyRiffCaptureState({ active: false, complete: false, takeId: "", capturedSamples: 0, sampleRate: 0, hasAudio: false, waveformPeaks: [] });
  showNotification("Riff capture cancelled");
}

/** The reply to "getRiffLibrary": the library as the engine now has it on disk. */
export function onRiffLibraryState(payload: IncomingPayload): void {
  const riffLibrary = (payload as { library?: RiffLibrary }).library;
  if (riffLibrary) {
    applyRiffLibraryState(riffLibrary);
    refreshDemoAudioSelectors();
  }
}

export function onRiffSaved(payload: IncomingPayload): void {
  appendLog(`riff saved ← ${(payload as { riffId?: string }).riffId ?? "riff"}`);
  const riffLibrary = (payload as { library?: RiffLibrary }).library;
  if (riffLibrary) {
    applyRiffLibraryState(riffLibrary);
  }
  showNotification("Riff saved", (payload as { path?: string }).path ?? "");
  if (!riffLibrary) {
    getRiffLibrary();
  }
  refreshDemoAudioSelectors();
}

export function onPracticeToolFileLoaded(payload: IncomingPayload): void {
  const info = payload as { path?: string; title?: string; durationSec?: number; waveformPeaksL?: unknown[]; waveformPeaksR?: unknown[] };
  applyPracticeToolFileLoaded(info);
}

export function onPracticeToolTransportState(payload: IncomingPayload): void {
  const info = payload as { state?: string; positionSec?: number };
  applyPracticeToolTransportState(info);
}

export function onPracticeToolPlaybackEnded(): void {
  applyPracticeToolPlaybackEnded();
}

export function onPreviewStarted(payload: IncomingPayload): void {
  appendLog(`preview started ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
  handleRiffPreviewPlayback("start", (payload as { id?: string }).id ?? "");
  syncDemoAudioSelectionFromPreview((payload as { id?: string }).id ?? null);
  onDemoAudioStarted();
  showNotification("Playing demo audio", (payload as { title?: string }).title ?? "Demo");
}

export function onPreviewComplete(payload: IncomingPayload): void {
  appendLog(`preview complete ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
  const previewId = (payload as { id?: string }).id ?? "";
  const savedRiffLooped = handleSavedRiffPreviewComplete(previewId);
  if (savedRiffLooped) {
    return;
  }
  handleRiffPreviewPlayback("stop", previewId);
  const capturedLooped = handleCapturedPreviewComplete(previewId);
  if (capturedLooped) {
    return;
  }
  // A repeating demo clip loops in the engine and never completes, so this is the end.
  onDemoAudioStopped();
  showNotification("Demo playback finished", (payload as { title?: string }).title ?? "Demo");
}

export function onPreviewStopped(payload: IncomingPayload): void {
  appendLog(`preview stopped ← ${(payload as { title?: string; id?: string }).title ?? (payload as { id?: string }).id ?? "demo"}`);
  handleRiffPreviewPlayback("stop", (payload as { id?: string }).id ?? "");
  onDemoAudioStopped();
  showNotification("Demo playback stopped", (payload as { title?: string }).title ?? "Demo");
}

export function onDemoAudioRenderSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string; sampleRate?: number };
  const sampleRate = typeof info.sampleRate === "number" && info.sampleRate > 0
    ? `${Math.round(info.sampleRate / 100) / 10} kHz`
    : "";
  appendLog(`demo audio rendered ← ${info.path ?? "unknown"}${sampleRate ? ` @ ${sampleRate}` : ""}`);
  showNotification("Demo audio rendered", sampleRate ? `${sampleRate} - ${info.path ?? ""}` : info.path ?? "");
}

export function onDemoAudioRenderFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  appendLog(`demo audio render failed ← ${info.message ?? "unknown"}`);
  showNotification("Demo audio render failed", info.message ?? "");
}
