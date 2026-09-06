import { describe, expect, it } from "vitest";
import type { CompositePreset } from "../ts/types.js";
import {
  collectCompositePresetTags,
  compositePresetMatchesMixer,
  filterCompositePresets,
  resolveCompositeSlotNames,
} from "../ts/multiPresetMixerSupport.js";

function makeComposite(overrides: Partial<CompositePreset> & { id: string }): CompositePreset {
  return {
    name: overrides.id,
    description: "",
    tags: [],
    slots: [],
    mixGainDb: 0,
    ...overrides,
  } as CompositePreset;
}

const cleanAndCrunch = makeComposite({
  id: "clean-crunch",
  name: "Clean + Crunch",
  description: "Stereo pair for the verse",
  tags: ["Live", "blues"],
  slots: [
    { slotId: "p1", presetId: "preset-clean", mix: 1, pan: -0.5, mute: false, solo: false },
    { slotId: "p2", presetId: "preset-crunch", mix: 0.8, pan: 0.5, mute: false, solo: false },
  ],
});

const wallOfFuzz = makeComposite({
  id: "fuzz-wall",
  name: "Wall of Fuzz",
  tags: ["metal"],
  slots: [
    { slotId: "p1", presetId: "preset-fuzz", mix: 1, pan: 0, mute: false, solo: false },
    { slotId: "p2", presetId: "preset-octave", mix: 1, pan: 0, mute: false, solo: false },
  ],
});

const names: Record<string, string> = {
  "preset-clean": "Deluxe Clean",
  "preset-crunch": "Plexi Crunch",
  "preset-fuzz": "Big Muff",
};

describe("filterCompositePresets", () => {
  it("returns everything for a blank query", () => {
    expect(filterCompositePresets([cleanAndCrunch, wallOfFuzz], "   ").map((cp) => cp.id)).toEqual(["clean-crunch", "fuzz-wall"]);
  });

  it("matches name, description and tags case-insensitively", () => {
    expect(filterCompositePresets([cleanAndCrunch, wallOfFuzz], "FUZZ").map((cp) => cp.id)).toEqual(["fuzz-wall"]);
    expect(filterCompositePresets([cleanAndCrunch, wallOfFuzz], "verse").map((cp) => cp.id)).toEqual(["clean-crunch"]);
    expect(filterCompositePresets([cleanAndCrunch, wallOfFuzz], "live").map((cp) => cp.id)).toEqual(["clean-crunch"]);
  });

  it("matches the names of the presets in each slot when a resolver is given", () => {
    const slotNames = (cp: CompositePreset) => resolveCompositeSlotNames(cp, (id) => names[id]);
    expect(filterCompositePresets([cleanAndCrunch, wallOfFuzz], "plexi", slotNames).map((cp) => cp.id)).toEqual(["clean-crunch"]);
    expect(filterCompositePresets([cleanAndCrunch, wallOfFuzz], "plexi").map((cp) => cp.id)).toEqual([]);
  });
});

describe("compositePresetMatchesMixer", () => {
  it("is true when the mixer holds exactly the saved presets, in any order", () => {
    expect(compositePresetMatchesMixer(cleanAndCrunch, ["preset-crunch", "preset-clean"])).toBe(true);
  });

  it("is false once a slot is added or removed", () => {
    expect(compositePresetMatchesMixer(cleanAndCrunch, ["preset-clean"])).toBe(false);
    expect(compositePresetMatchesMixer(cleanAndCrunch, ["preset-clean", "preset-crunch", "preset-fuzz"])).toBe(false);
    expect(compositePresetMatchesMixer(cleanAndCrunch, [])).toBe(false);
  });

  it("is false when a slot was swapped for another preset", () => {
    expect(compositePresetMatchesMixer(cleanAndCrunch, ["preset-clean", "preset-fuzz"])).toBe(false);
  });
});

describe("resolveCompositeSlotNames", () => {
  it("falls back to the preset id when the preset is gone", () => {
    expect(resolveCompositeSlotNames(wallOfFuzz, (id) => names[id])).toEqual(["Big Muff", "preset-octave"]);
  });
});

describe("collectCompositePresetTags", () => {
  it("normalises, de-duplicates and sorts", () => {
    const tagged = makeComposite({ id: "x", tags: [" Live", "BLUES", "", "live"] });
    expect(collectCompositePresetTags([tagged, wallOfFuzz])).toEqual(["blues", "live", "metal"]);
  });
});
