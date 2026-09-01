import { describe, expect, it } from "vitest";
import {
  TIME_SIGNATURES,
  beatLevels,
  defaultBeatPattern,
  groupHeads,
  nextBeatLevel,
  normaliseBeatPattern,
  parseGrouping,
  patternFromLevels,
} from "../ts/metronomeMeters.js";

/**
 * These mirror the engine's own rules in MetronomeSupport.h — the UI derives a
 * pattern before the round trip lands, so the two must agree exactly. The
 * expectations here are deliberately the same cases as
 * core/tests/MetronomeServiceTests.cpp.
 */

describe("defaultBeatPattern", () => {
  it("accents beat one of a simple meter", () => {
    expect(defaultBeatPattern(4, 4, "")).toBe("HLLL");
    expect(defaultBeatPattern(5, 4, "")).toBe("HLLLL");
  });

  it("gives the group heads of a compound meter a medium accent", () => {
    expect(defaultBeatPattern(6, 8, "")).toBe("HLLMLL");
    expect(defaultBeatPattern(9, 8, "")).toBe("HLLMLLMLL");
    expect(defaultBeatPattern(12, 8, "")).toBe("HLLMLLMLLMLL");
  });

  it("follows an explicit grouping", () => {
    expect(defaultBeatPattern(7, 8, "2+2+3")).toBe("HLMLMLL");
    expect(defaultBeatPattern(7, 8, "3+2+2")).toBe("HLLMLML");
    expect(defaultBeatPattern(5, 8, "3+2")).toBe("HLLML");
  });

  it("ignores a grouping that does not add up to the bar", () => {
    expect(defaultBeatPattern(7, 8, "2+2")).toBe(defaultBeatPattern(7, 8, ""));
  });
});

describe("parseGrouping", () => {
  it("splits a grouping that fills the bar", () => {
    expect(parseGrouping("2+2+3", 7)).toEqual([2, 2, 3]);
  });

  it("accepts commas and spaces as separators", () => {
    expect(parseGrouping("3, 2", 5)).toEqual([3, 2]);
  });

  it("drops a grouping that misses the bar", () => {
    expect(parseGrouping("2+2", 7)).toEqual([]);
    expect(parseGrouping("2+2+3", 4)).toEqual([]);
  });

  it("drops zero groups and junk", () => {
    expect(parseGrouping("3+0+4", 7)).toEqual([]);
    expect(parseGrouping("2+x", 7)).toEqual([]);
  });
});

describe("normaliseBeatPattern", () => {
  it("repeats a short pattern across the bar", () => {
    expect(normaliseBeatPattern("HL", 4, 4, "")).toBe("HLHL");
  });

  it("cuts a pattern longer than the bar", () => {
    expect(normaliseBeatPattern("HLLLSSS", 4, 4, "")).toBe("HLLL");
  });

  it("reads the legacy silence characters", () => {
    expect(normaliseBeatPattern("H-.L", 4, 4, "")).toBe("HSSL");
  });

  it("falls back to the meter default when empty", () => {
    expect(normaliseBeatPattern("", 6, 8, "")).toBe("HLLMLL");
  });

  it("ignores characters it does not know", () => {
    expect(normaliseBeatPattern("H?L?L?L?", 4, 4, "")).toBe("HLLL");
  });
});

describe("beat levels", () => {
  it("round-trips a pattern through its levels", () => {
    const levels = beatLevels("HLMS", 4, 4, "");
    expect(levels).toEqual(["accent", "normal", "medium", "off"]);
    expect(patternFromLevels(levels)).toBe("HLMS");
  });

  it("cycles accent to medium to normal to off and back", () => {
    expect(nextBeatLevel("accent")).toBe("medium");
    expect(nextBeatLevel("medium")).toBe("normal");
    expect(nextBeatLevel("normal")).toBe("off");
    expect(nextBeatLevel("off")).toBe("accent");
  });
});

describe("groupHeads", () => {
  it("marks where each cluster after the first starts", () => {
    expect([...groupHeads(7, 8, "2+2+3")]).toEqual([2, 4]);
    expect([...groupHeads(6, 8, "")]).toEqual([3]);
  });

  it("has nothing to mark in an even meter", () => {
    expect(groupHeads(4, 4, "").size).toBe(0);
  });
});

describe("TIME_SIGNATURES", () => {
  it("gives every option a unique id", () => {
    const ids = TIME_SIGNATURES.map((option) => option.id);
    expect(new Set(ids).size).toBe(ids.length);
  });

  it("only carries groupings that fill their bar", () => {
    for (const option of TIME_SIGNATURES) {
      if (!option.grouping) continue;
      expect(parseGrouping(option.grouping, option.num)).not.toEqual([]);
    }
  });

  it("offers the meters the picker advertises", () => {
    const ids = TIME_SIGNATURES.map((option) => option.id);
    expect(ids).toContain("4/4");
    expect(ids).toContain("13/4");
    expect(ids).toContain("12/8");
    expect(ids).toContain("7/8:2+2+3");
  });
});
