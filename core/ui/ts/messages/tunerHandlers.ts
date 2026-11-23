/**
 * Tuner readings and the tuner's own settings.
 */

import { onScaleGuideReading } from "../scaleGuide/panel.js";
import { handleTunerLiveModeChanged, handleTunerReferenceChanged, handleTunerStarted, handleTunerStopped, handleTunerUpdate, isTunerOpen } from "../tuner.js";
import type { IncomingPayload } from "./types.js";

export function onTunerUpdate(payload: IncomingPayload): void {
  // A reading for the Scale Guide's LISTEN with the tuner closed: nothing to show or log here.
  if (onScaleGuideReading(payload) && !isTunerOpen()) {
    return;
  }

  const tunerPayload = payload as { 
    detected?: boolean; 
    noteName?: string; 
    octave?: number;
    frequency?: number;
    centOffset?: number;
    confidence?: number;
    levelDb?: number;
    debugRms?: number;
    debugRawFreq?: number;
  };
  
  // Log debug info to console
  const rms = tunerPayload.debugRms?.toFixed(6) ?? "?";
  const rawFreq = tunerPayload.debugRawFreq?.toFixed(2) ?? "?";
  console.log(`[Tuner] RMS=${rms}, rawFreq=${rawFreq}Hz, detected=${tunerPayload.detected}, note=${tunerPayload.noteName ?? "-"}`);
  
  handleTunerUpdate({
    detected: tunerPayload.detected ?? false,
    noteName: tunerPayload.noteName,
    octave: tunerPayload.octave,
    frequency: tunerPayload.frequency,
    centOffset: tunerPayload.centOffset,
    confidence: tunerPayload.confidence,
    levelDb: tunerPayload.levelDb,
  });
}

export function onTunerStarted(payload: IncomingPayload): void {
  const startPayload = payload as { referenceFrequency?: number; liveMode?: boolean };
  handleTunerStarted(startPayload.referenceFrequency ?? 440.0);
  if (startPayload.liveMode !== undefined) {
    handleTunerLiveModeChanged(startPayload.liveMode);
  }
}

export function onTunerStopped(): void {
  handleTunerStopped();
}

export function onTunerReferenceChanged(payload: IncomingPayload): void {
  handleTunerReferenceChanged((payload as { referenceFrequency?: number }).referenceFrequency ?? 440.0);
}

export function onTunerLiveModeChanged(payload: IncomingPayload): void {
  handleTunerLiveModeChanged((payload as { liveMode?: boolean }).liveMode ?? true);
}
