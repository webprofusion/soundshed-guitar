import { describe, expect, it } from "vitest";
import {
  collectPresetTags,
  describePresetCount,
  flattenPresetFolders,
  isLocalPresetSort,
  presetInitials,
  sortLocalPresets,
} from "../ts/tonesPanel/localPresetsModel.js";
import type { Preset, PresetFolder } from "../ts/types.js";

const preset = (id: string, name: string, tags: string[] = []): Preset => ({ id, name, tags });

describe("sortLocalPresets", () => {
  const presets = [preset("c", "Crunch 10"), preset("a", "clean"), preset("b", "Crunch 2")];

  it("orders by name, numbers as numbers, without touching the input", () => {
    const input = presets.slice();
    expect(sortLocalPresets(input, "name", () => null).map((entry) => entry.id)).toEqual(["a", "b", "c"]);
    expect(input.map((entry) => entry.id)).toEqual(["c", "a", "b"]);
  });

  it("keeps the given order for Recents", () => {
    expect(sortLocalPresets(presets, "name", () => null, true).map((entry) => entry.id)).toEqual(["c", "a", "b"]);
  });

  it("puts the highest rated first and the unrated last, by name within a rating", () => {
    const ratings: Record<string, number> = { c: 5, b: 5 };
    const sorted = sortLocalPresets(presets, "rating", (id) => ratings[id] ?? null);
    expect(sorted.map((entry) => entry.id)).toEqual(["b", "c", "a"]);
  });

  it("sorts Recents by rating too when asked", () => {
    const sorted = sortLocalPresets(presets, "rating", (id) => (id === "a" ? 3 : null), true);
    expect(sorted[0].id).toBe("a");
  });
});

describe("collectPresetTags", () => {
  it("lists each tag once, in name order, skipping blanks", () => {
    const presets = [preset("a", "A", ["Metal", " lead "]), preset("b", "B", ["clean", "Metal", ""]), { id: "c", name: "C" }];
    expect(collectPresetTags(presets)).toEqual(["clean", "lead", "Metal"]);
  });
});

describe("presetInitials", () => {
  it("takes two words' initials, or one word's first two characters", () => {
    expect(presetInitials("plexi crunch lead")).toBe("PC");
    expect(presetInitials("  Twin  ")).toBe("TW");
    expect(presetInitials("5150")).toBe("51");
    expect(presetInitials("01 David Gilmour - PULSE Sustain")).toBe("DG");
    expect(presetInitials("2dor - Marshall JCM800")).toBe("2M");
    expect(presetInitials("12 34")).toBe("34");
    expect(presetInitials("---")).toBe("?");
    expect(presetInitials("")).toBe("?");
  });
});

describe("flattenPresetFolders", () => {
  it("lists parents before their children with their depth", () => {
    const folders: PresetFolder[] = [
      { id: "1", name: "Amps", presetIds: [], children: [{ id: "2", name: "British", presetIds: [], children: [{ id: "3", name: "Plexi", presetIds: [], children: [] }] }] },
      { id: "4", name: "Bass", presetIds: [], children: [] },
    ];
    expect(flattenPresetFolders(folders)).toEqual([
      { id: "1", name: "Amps", depth: 0 },
      { id: "2", name: "British", depth: 1 },
      { id: "3", name: "Plexi", depth: 2 },
      { id: "4", name: "Bass", depth: 0 },
    ]);
  });
});

describe("describePresetCount", () => {
  it("says how many, and how many of how many once narrowed", () => {
    expect(describePresetCount(12, 12)).toBe("12 presets");
    expect(describePresetCount(1, 1)).toBe("1 preset");
    expect(describePresetCount(3, 12)).toBe("3 of 12 presets");
    expect(describePresetCount(0, 0)).toBe("0 presets");
  });
});

describe("isLocalPresetSort", () => {
  it("accepts only the sorts the tab offers", () => {
    expect(isLocalPresetSort("name")).toBe(true);
    expect(isLocalPresetSort("rating")).toBe(true);
    expect(isLocalPresetSort("recent")).toBe(false);
  });
});
