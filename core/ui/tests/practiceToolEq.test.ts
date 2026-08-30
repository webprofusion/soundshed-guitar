import { describe, expect, it } from "vitest";
import {
  PRACTICE_TOOL_EQ_PARAM_KEYS,
  createDefaultPracticeToolEq,
  createDefaultPracticeToolEqParams,
  isPracticeToolEqShaping,
  sanitizePracticeToolEq,
} from "../ts/practiceTool/eq.js";

describe("createDefaultPracticeToolEqParams", () => {
  it("covers every key the engine's EQ accepts", () => {
    const params = createDefaultPracticeToolEqParams();
    expect(PRACTICE_TOOL_EQ_PARAM_KEYS.length).toBe(12);
    PRACTICE_TOOL_EQ_PARAM_KEYS.forEach((key) => {
      expect(typeof params[key]).toBe("number");
    });
  });

  it("matches ParametricEQEffect's own band defaults, so a fresh EQ is flat", () => {
    // Keeping these in step with core/src/dsp/effects/ParametricEQEffect.h is
    // what stops switching the EQ on from audibly changing anything.
    expect(createDefaultPracticeToolEqParams()).toMatchObject({
      lowGain: 0, lowFreq: 100, lowQ: 0.707,
      lowMidGain: 0, lowMidFreq: 400, lowMidQ: 1,
      highMidGain: 0, highMidFreq: 2000, highMidQ: 1,
      highGain: 0, highFreq: 8000, highQ: 0.707,
    });
  });

  it("starts switched off", () => {
    expect(createDefaultPracticeToolEq().enabled).toBe(false);
  });
});

describe("sanitizePracticeToolEq", () => {
  it("returns the flat default for anything unusable", () => {
    const flat = createDefaultPracticeToolEqParams();
    for (const value of [undefined, null, 42, "eq", []]) {
      expect(sanitizePracticeToolEq(value)).toEqual({ enabled: false, params: flat });
    }
  });

  it("keeps the values it recognises and defaults the rest", () => {
    const result = sanitizePracticeToolEq({
      enabled: true,
      params: { lowGain: -6.5, lowMidFreq: 750 },
    });
    expect(result.enabled).toBe(true);
    expect(result.params.lowGain).toBe(-6.5);
    expect(result.params.lowMidFreq).toBe(750);
    expect(result.params.highFreq).toBe(8000);
  });

  it("rejects non-finite and non-numeric params rather than passing them to the engine", () => {
    const result = sanitizePracticeToolEq({
      params: { lowGain: Number.NaN, lowFreq: Number.POSITIVE_INFINITY, highGain: "3" },
    });
    expect(result.params.lowGain).toBe(0);
    expect(result.params.lowFreq).toBe(100);
    expect(result.params.highGain).toBe(0);
  });

  it("treats any non-true `enabled` as off", () => {
    expect(sanitizePracticeToolEq({ enabled: "yes" }).enabled).toBe(false);
    expect(sanitizePracticeToolEq({ enabled: 1 }).enabled).toBe(false);
    expect(sanitizePracticeToolEq({ enabled: true }).enabled).toBe(true);
  });

  it("ignores keys the engine's EQ would not accept", () => {
    const result = sanitizePracticeToolEq({ params: { bogusGain: 9 } });
    expect(result.params.bogusGain).toBeUndefined();
  });
});

describe("isPracticeToolEqShaping", () => {
  it("is false while switched off, however the bands are set", () => {
    const eq = createDefaultPracticeToolEq();
    eq.params.lowGain = 9;
    expect(isPracticeToolEqShaping(eq)).toBe(false);
  });

  it("is false when switched on but flat — the button should not claim otherwise", () => {
    const eq = createDefaultPracticeToolEq();
    eq.enabled = true;
    expect(isPracticeToolEqShaping(eq)).toBe(false);
  });

  it("is true once any band has real gain, in either direction", () => {
    for (const gain of [3, -3]) {
      const eq = { ...createDefaultPracticeToolEq(), enabled: true };
      eq.params.highMidGain = gain;
      expect(isPracticeToolEqShaping(eq)).toBe(true);
    }
  });

  it("ignores a frequency or Q moved with the gain left at zero", () => {
    const eq = { ...createDefaultPracticeToolEq(), enabled: true };
    eq.params.lowFreq = 220;
    eq.params.lowQ = 4;
    expect(isPracticeToolEqShaping(eq)).toBe(false);
  });
});
