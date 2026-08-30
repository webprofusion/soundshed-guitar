/**
 * Backing-track EQ state: defaults and validation.
 *
 * The DSP is the same four-band `ParametricEQEffect` the signal path uses, so
 * this speaks the effect's own parameter names ("lowGain", "highFreq", …) end
 * to end — the curve editor, the saved project and the `setPracticeToolEq`
 * message all carry the identical flat dict, with no translation table in
 * between to drift.
 *
 * Deliberately free of any bridge or DOM import, so state.ts can seed the
 * default from here — eq -> bridge -> state would otherwise close a cycle. The
 * sends live in ./eqSend.js and the modal in ./eqModal.js; neither of those
 * imports practiceTool.ts either.
 */

import { EQ_BAND_KEYS, EQ_BAND_RANGES, EQ_FREQ_DEFAULTS } from "../eqCurve.js";
import type { PracticeToolEqState } from "../types.js";

/** Every parameter key the engine's EQ accepts, in band order. */
export const PRACTICE_TOOL_EQ_PARAM_KEYS: readonly string[] = EQ_BAND_KEYS.flatMap((keys) =>
  keys.q ? [keys.gain, keys.freq, keys.q] : [keys.gain, keys.freq]);

/** Flat, matching ParametricEQEffect's own band defaults — a fresh EQ is the
 * identity curve, so switching it on before touching anything is inaudible. */
export function createDefaultPracticeToolEqParams(): Record<string, number> {
  const params: Record<string, number> = {};
  EQ_BAND_KEYS.forEach((keys, index) => {
    params[keys.gain] = 0;
    params[keys.freq] = EQ_FREQ_DEFAULTS[index];
    if (keys.q) {
      params[keys.q] = EQ_BAND_RANGES[index].qDefault;
    }
  });
  return params;
}

export function createDefaultPracticeToolEq(): PracticeToolEqState {
  return { enabled: false, params: createDefaultPracticeToolEqParams() };
}

/**
 * Narrows a stored (or hand-edited) EQ blob back to usable state, filling any
 * key it is missing from the defaults. Returns the flat default for anything
 * unrecognisable rather than null, because every caller would otherwise have
 * to invent the same fallback.
 */
export function sanitizePracticeToolEq(value: unknown): PracticeToolEqState {
  const fallback = createDefaultPracticeToolEq();
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return fallback;
  }
  const record = value as { enabled?: unknown; params?: unknown };
  const storedParams = (record.params && typeof record.params === "object" && !Array.isArray(record.params))
    ? record.params as Record<string, unknown>
    : {};

  for (const key of PRACTICE_TOOL_EQ_PARAM_KEYS) {
    const stored = storedParams[key];
    if (typeof stored === "number" && Number.isFinite(stored)) {
      fallback.params[key] = stored;
    }
  }
  return { enabled: record.enabled === true, params: fallback.params };
}

/** True when the EQ is on and actually shaping something — used for the panel
 * button's "this is doing work" state, so a switched-on-but-flat EQ reads as
 * the no-op it is. */
export function isPracticeToolEqShaping(eq: PracticeToolEqState): boolean {
  return eq.enabled && EQ_BAND_KEYS.some((keys) => Math.abs(eq.params[keys.gain] ?? 0) > 0.05);
}
