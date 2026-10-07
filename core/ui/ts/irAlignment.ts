/**
 * Lining up the IR Cabinet's two IRs: asking the engine where IR B lines up with
 * IR A (core/src/controller/PluginControllerEffectAnalysis.cpp, dsp/IrAlignment.h),
 * and the arithmetic the Alignment section draws from its answer.
 *
 * The engine reads both IR files and answers once: the start of each at 48 kHz,
 * the offset and polarity that line them up, and how alike the two are at every
 * offset in reach. What the section redraws while an offset is dragged (the match
 * there, the two summed) is worked out here from that answer, so a drag never
 * waits on the engine. With only one IR loaded it answers with that one's start
 * alone, and the section draws its response and nothing to align.
 */

import { sendAnalyzeIrAlignment } from "./bridge.js";
import { BUTTERWORTH_Q, bandMagnitude, highPassMagnitude, lowPassMagnitude, shelfQFromSlope } from "./eqPlot.js";
import type { ResourceRef } from "./types.js";

/** An analysis takes well under a second; one not answered by then has failed. */
export const IR_ALIGNMENT_TIMEOUT_MS = 20000;

export interface IrAlignmentAnalysis {
  /** Which slots had an IR: both, or only A or only B. Only a pair has the
   * alignment fields below; for one IR they are zero and the curve one entry. */
  slots: "both" | "a" | "b";
  /** The rate of the waveforms and of matchByOffset's steps. */
  sampleRate: number;
  /** The start of each IR, mono; empty for a slot with no IR. */
  waveformA: Float32Array;
  waveformB: Float32Array;
  /** The gain the cab's Normalize applies to each. */
  normalizeGainA: number;
  normalizeGainB: number;
  /** The IR B offset, in ms, that lines B up with A. */
  alignedOffsetMs: number;
  /** Whether B lines up with A inverted. */
  invertB: boolean;
  /** How alike the two are once lined up, 0..1. */
  match: number;
  onsetAMs: number;
  onsetBMs: number;
  /** The signed match with B moved by each whole sample, -maxOffsetSamples first. */
  maxOffsetSamples: number;
  matchByOffset: number[];
}

export type IrAlignmentResult = { ok: true; analysis: IrAlignmentAnalysis } | { ok: false; error: string };

let requestCounter = 0;
let pending: {
  id: string;
  listener: (result: IrAlignmentResult) => void;
  timer: ReturnType<typeof setTimeout>;
} | null = null;

/** Asks where `irB` lines up with `irA`, each a ref as the node holds it, or for
 * the start of the one IR when the other slot is null. A new request supersedes
 * one still out; its answer is dropped. */
export function requestIrAlignment(
  irA: ResourceRef | null,
  irB: ResourceRef | null,
  listener: (result: IrAlignmentResult) => void,
): void {
  if (pending) {
    clearTimeout(pending.timer);
  }
  requestCounter += 1;
  const id = `ir-alignment-${requestCounter}`;
  const timer = setTimeout(() => {
    if (pending?.id === id) {
      pending = null;
      listener({ ok: false, error: "The engine did not answer" });
    }
  }, IR_ALIGNMENT_TIMEOUT_MS);
  pending = { id, listener, timer };
  sendAnalyzeIrAlignment(id, irA, irB);
}

/** Little-endian float32 samples, base64'd. */
export function decodeFloat32Base64(text: string): Float32Array {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length - (binary.length % 4));
  for (let i = 0; i < bytes.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return new Float32Array(bytes.buffer);
}

function finiteNumber(value: unknown, fallback = Number.NaN): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

/** Reads an "irAlignment" reply; null when it is not a usable analysis. */
export function parseIrAlignment(payload: Record<string, unknown>): IrAlignmentAnalysis | null {
  const sampleRate = finiteNumber(payload.sampleRate);
  const hasA = typeof payload.waveformA === "string";
  const hasB = typeof payload.waveformB === "string";
  if (!(sampleRate > 0) || (!hasA && !hasB)) {
    return null;
  }
  if (!hasA || !hasB) {
    return {
      slots: hasA ? "a" : "b",
      sampleRate,
      waveformA: hasA ? decodeFloat32Base64(payload.waveformA as string) : new Float32Array(0),
      waveformB: hasB ? decodeFloat32Base64(payload.waveformB as string) : new Float32Array(0),
      normalizeGainA: finiteNumber(payload.normalizeGainA, 1),
      normalizeGainB: finiteNumber(payload.normalizeGainB, 1),
      alignedOffsetMs: 0,
      invertB: false,
      match: 0,
      onsetAMs: 0,
      onsetBMs: 0,
      maxOffsetSamples: 0,
      matchByOffset: [0],
    };
  }
  const matchByOffset = payload.matchByOffset;
  const maxOffsetSamples = finiteNumber(payload.maxOffsetSamples, 0);
  const alignedOffsetMs = finiteNumber(payload.alignedOffsetMs);
  if (!Array.isArray(matchByOffset) || matchByOffset.length !== 2 * maxOffsetSamples + 1
    || !matchByOffset.every((value) => typeof value === "number" && Number.isFinite(value))
    || !Number.isFinite(alignedOffsetMs)) {
    return null;
  }
  return {
    slots: "both",
    sampleRate,
    waveformA: decodeFloat32Base64(payload.waveformA as string),
    waveformB: decodeFloat32Base64(payload.waveformB as string),
    normalizeGainA: finiteNumber(payload.normalizeGainA, 1),
    normalizeGainB: finiteNumber(payload.normalizeGainB, 1),
    alignedOffsetMs,
    invertB: payload.invertB === true,
    match: finiteNumber(payload.match, 0),
    onsetAMs: finiteNumber(payload.onsetAMs, 0),
    onsetBMs: finiteNumber(payload.onsetBMs, 0),
    maxOffsetSamples,
    matchByOffset: matchByOffset as number[],
  };
}

/** Handles an "irAlignment" reply. Returns false for one nobody is waiting for. */
export function applyIrAlignment(payload: Record<string, unknown>): boolean {
  if (!pending || payload.requestId !== pending.id) {
    return false;
  }
  const { listener, timer } = pending;
  clearTimeout(timer);
  pending = null;

  if (typeof payload.error === "string") {
    listener({ ok: false, error: payload.error });
    return true;
  }
  const analysis = parseIrAlignment(payload);
  listener(analysis ? { ok: true, analysis } : { ok: false, error: "No analysis came back" });
  return true;
}

// ── What the section draws ────────────────────────────────────────────────

/** The signed match with B moved by `offsetMs`, between whole samples by straight
 * line; held at the ends beyond the analysis's reach. */
export function matchAtOffset(analysis: IrAlignmentAnalysis, offsetMs: number): number {
  const curve = analysis.matchByOffset;
  const position = Math.max(0, Math.min(curve.length - 1,
    offsetMs * analysis.sampleRate / 1000 + analysis.maxOffsetSamples));
  const below = Math.floor(position);
  const above = Math.min(curve.length - 1, below + 1);
  return curve[below] + (position - below) * (curve[above] - curve[below]);
}

/** Polarity of each slot as the node's params set it: +1 or -1. */
export function slotPolarities(params: Record<string, number>): { a: number; b: number } {
  return {
    a: (params.slotAPolarity ?? 0) >= 0.5 ? -1 : 1,
    b: (params.slotBPolarity ?? 0) >= 0.5 ? -1 : 1,
  };
}

/** Each slot's gain into the sum, as the cab applies it: blend, level, polarity,
 * Normalize, and the blend's level compensation (IRCabEffect::Process). With one
 * IR the blend does not apply: that IR plays alone, at its own level. */
export function slotGains(params: Record<string, number>, analysis: IrAlignmentAnalysis): { a: number; b: number } {
  const blend = analysis.slots === "both" ? Math.max(0, Math.min(1, params.irBlend ?? 0)) : (analysis.slots === "a" ? 0 : 1);
  const normalize = (params.normalizeIR ?? 1) >= 0.5;
  const compensate = (params.autoGainComp ?? 1) >= 0.5;
  const blendA = 1 - blend;
  const blendB = blend;
  const compensation = compensate ? 1 / Math.sqrt(Math.max(1e-8, blendA * blendA + blendB * blendB)) : 1;
  const polarity = slotPolarities(params);
  const level = (db: number | undefined): number => Math.pow(10, Math.max(-24, Math.min(24, db ?? 0)) / 20);
  return {
    a: blendA * level(params.slotAGain) * polarity.a * (normalize ? analysis.normalizeGainA : 1) * compensation,
    b: blendB * level(params.slotBGain) * polarity.b * (normalize ? analysis.normalizeGainB : 1) * compensation,
  };
}

/** The frequencies a response is drawn at: log-spaced, 20 Hz to 20 kHz. */
export function responseFrequencies(points = 160): number[] {
  return Array.from({ length: points }, (_, i) => 20 * Math.pow(1000, i / (points - 1)));
}

/** Each band's response is the power average of this many points across it, as
 * the engine's SmoothedMagnitudeDb averages them. */
const POINTS_PER_BAND = 7;
const BAND_OCTAVES = 1 / 6;
/** The part of each IR the response is taken from, faded out over its last fifth. */
const RESPONSE_WINDOW_MS = 50;

/** Each IR's complex response at every point of every band, worked out once per
 * analysis: summing them with an offset is then a rotation per point. */
export interface AlignmentSpectra {
  frequencies: number[];
  /** Per band, per point: the frequency, and A's and B's response there (re, im). */
  pointHz: Float64Array;
  aRe: Float64Array;
  aIm: Float64Array;
  bRe: Float64Array;
  bIm: Float64Array;
}

function windowed(samples: Float32Array, sampleRate: number): Float64Array {
  const length = Math.min(samples.length, Math.max(1, Math.round(RESPONSE_WINDOW_MS * sampleRate / 1000)));
  const fadeStart = length - Math.floor(length / 5);
  const out = new Float64Array(length);
  for (let n = 0; n < length; n += 1) {
    const gain = n >= fadeStart && length > fadeStart
      ? 0.5 * (1 + Math.cos(Math.PI * (n - fadeStart) / (length - fadeStart)))
      : 1;
    out[n] = samples[n] * gain;
  }
  return out;
}

export function buildAlignmentSpectra(analysis: IrAlignmentAnalysis, frequencies = responseFrequencies()): AlignmentSpectra {
  const count = frequencies.length * POINTS_PER_BAND;
  const spectra: AlignmentSpectra = {
    frequencies,
    pointHz: new Float64Array(count),
    aRe: new Float64Array(count),
    aIm: new Float64Array(count),
    bRe: new Float64Array(count),
    bIm: new Float64Array(count),
  };
  const a = windowed(analysis.waveformA, analysis.sampleRate);
  const b = windowed(analysis.waveformB, analysis.sampleRate);
  const nyquist = analysis.sampleRate / 2;

  frequencies.forEach((centre, band) => {
    for (let point = 0; point < POINTS_PER_BAND; point += 1) {
      const offsetOctaves = BAND_OCTAVES * (point / (POINTS_PER_BAND - 1) - 0.5);
      const hz = Math.min(centre * Math.pow(2, offsetOctaves), nyquist);
      const index = band * POINTS_PER_BAND + point;
      // A unit phasor rotated per sample, rather than a sin and cos per sample.
      const stepRe = Math.cos(-2 * Math.PI * hz / analysis.sampleRate);
      const stepIm = Math.sin(-2 * Math.PI * hz / analysis.sampleRate);
      const response = (samples: Float64Array): [number, number] => {
        let re = 0;
        let im = 0;
        let phaseRe = 1;
        let phaseIm = 0;
        for (let n = 0; n < samples.length; n += 1) {
          re += samples[n] * phaseRe;
          im += samples[n] * phaseIm;
          const nextRe = phaseRe * stepRe - phaseIm * stepIm;
          phaseIm = phaseRe * stepIm + phaseIm * stepRe;
          phaseRe = nextRe;
        }
        return [re, im];
      };
      const [aRe, aIm] = response(a);
      const [bRe, bIm] = response(b);
      spectra.pointHz[index] = hz;
      spectra.aRe[index] = aRe;
      spectra.aIm[index] = aIm;
      spectra.bRe[index] = bRe;
      spectra.bIm[index] = bIm;
    }
  });
  return spectra;
}

export interface CombinedResponse {
  frequencies: number[];
  /** The two summed with B moved by the offset, in dB. */
  summedDb: number[];
  /** The two added as if they could not interfere (their powers summed): where
   * the summed curve falls below this, they are cancelling. */
  unrelatedDb: number[];
}

/** A mic-position slot's magnitude at `hz`: an off-axis HF shelf and a close-mic low-mid
 * peak, exactly as IRCabEffect::ProcessMicPositionSlotA/B design them (UpdateMicCoefficients),
 * applied to that slot alone before it joins the blend. */
function micPositionMagnitude(radial: number, proximity: number, hz: number, sampleRate: number): number {
  const radialGainDb = Math.max(0, Math.min(1, radial)) * -12;
  const proximityGainDb = Math.max(0, Math.min(1, proximity)) * 6;
  const shelf = bandMagnitude(hz, { freq: 4000, gainDb: radialGainDb, q: shelfQFromSlope(0.7, radialGainDb), shelfType: "high" }, sampleRate);
  const peak = bandMagnitude(hz, { freq: 150, gainDb: proximityGainDb, q: 1.0 }, sampleRate);
  return shelf * peak;
}

/** The magnitude at `hz` of what IRCabEffect::Process applies after the blend: Low Cut,
 * High Cut (ProcessCabFilters, UpdateCabFilterCoefficients), then Air (ProcessAirSample,
 * UpdateAirCoefficients) in whichever of its three modes is set. Shared by both slots, since
 * it runs on their sum. */
function cabPostFilterMagnitude(params: Record<string, number>, hz: number, sampleRate: number): number {
  let magnitude = 1;
  const lowCutHz = params.lowCutHz ?? 20;
  const highCutHz = params.highCutHz ?? 20000;
  const nyquist = sampleRate / 2;
  if (lowCutHz > 20.5) {
    magnitude *= highPassMagnitude(hz, lowCutHz, BUTTERWORTH_Q, sampleRate);
  }
  if (highCutHz < 19999.5 && highCutHz < nyquist - 100) {
    magnitude *= lowPassMagnitude(hz, highCutHz, BUTTERWORTH_Q, sampleRate);
  }
  const air = Math.max(0, Math.min(1, params.air ?? 0));
  if (air > 0.0001) {
    const airMode = Math.round(params.airMode ?? 0);
    if (airMode !== 1) {
      const gainDb = air * 12;
      magnitude *= bandMagnitude(hz, { freq: 7000, gainDb, q: shelfQFromSlope(0.7, gainDb), shelfType: "high" }, sampleRate);
    }
    if (airMode !== 0) {
      const gainDb = air * 8;
      magnitude *= bandMagnitude(hz, { freq: 3600, gainDb, q: 1.8 }, sampleRate);
    }
  }
  return magnitude;
}

/** A and B summed with these gains and B moved by `offsetMs`, shaped the way the node's
 * current Low Cut/High Cut/Air/Mic Position controls shape it (IRCabEffect::Process):
 * Mic Position per slot before the sum, Low Cut/High Cut/Air after it. `params` and
 * `sampleRate` default to off/48kHz so existing callers without them are unaffected. */
export function combinedResponse(
  spectra: AlignmentSpectra,
  gains: { a: number; b: number },
  offsetMs: number,
  params: Record<string, number> = {},
  sampleRate = 48000,
): CombinedResponse {
  const bands = spectra.frequencies.length;
  const summedDb = new Array<number>(bands);
  const unrelatedDb = new Array<number>(bands);
  const offsetSeconds = offsetMs / 1000;
  const micOn = (params.micEmulation ?? 0) >= 0.5;
  for (let band = 0; band < bands; band += 1) {
    let summed = 0;
    let unrelated = 0;
    for (let point = 0; point < POINTS_PER_BAND; point += 1) {
      const index = band * POINTS_PER_BAND + point;
      const hz = spectra.pointHz[index];
      const post = cabPostFilterMagnitude(params, hz, sampleRate);
      const gainA = gains.a * post * (micOn ? micPositionMagnitude(params.micRadialA ?? 0, params.micProximityA ?? 0, hz, sampleRate) : 1);
      const gainB = gains.b * post * (micOn ? micPositionMagnitude(params.micRadialB ?? 0, params.micProximityB ?? 0, hz, sampleRate) : 1);
      // B played later by the offset: its response turns by -2*pi*f*offset.
      const angle = -2 * Math.PI * hz * offsetSeconds;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      const aRe = gainA * spectra.aRe[index];
      const aIm = gainA * spectra.aIm[index];
      const bRe = gainB * (spectra.bRe[index] * cos - spectra.bIm[index] * sin);
      const bIm = gainB * (spectra.bRe[index] * sin + spectra.bIm[index] * cos);
      summed += (aRe + bRe) ** 2 + (aIm + bIm) ** 2;
      unrelated += aRe * aRe + aIm * aIm + bRe * bRe + bIm * bIm;
    }
    summedDb[band] = 10 * Math.log10(Math.max(summed / POINTS_PER_BAND, 1e-20));
    unrelatedDb[band] = 10 * Math.log10(Math.max(unrelated / POINTS_PER_BAND, 1e-20));
  }
  return { frequencies: spectra.frequencies, summedDb, unrelatedDb };
}
