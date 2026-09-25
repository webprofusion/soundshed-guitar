import { beforeEach, describe, expect, it } from "vitest";
import {
  LAYOUT_PREFERENCES_SETTING,
  STANDARD_LAYOUT_ID,
  buildLayoutMatchText,
  clearLayoutPreferencesForKeys,
  getAvailableLayoutEntries,
  getLayoutPreferenceRules,
  layoutLookupKeysFor,
  removeLayoutPreference,
  resolveLayoutForNode,
  resolveLayoutSelection,
  selectStandardControls,
  setLayoutPreference,
  suggestLayoutKeywords,
} from "../ts/layoutPreferences.js";
import { uiState } from "../ts/state.js";
import type { EffectLayout, LayoutLibraryEntry } from "../ts/layoutTypes.js";

function makeLayout(name: string, effectType = "amp_nam"): EffectLayout {
  return {
    effectType,
    version: 1,
    name,
    dimensions: { width: 400, height: 280 },
    backgrounds: [],
    controls: [],
    textLabels: [],
  };
}

function makeEntry(layoutId: string, name: string, extra?: Partial<LayoutLibraryEntry>): LayoutLibraryEntry {
  return {
    layout: { ...makeLayout(name), layoutId },
    layoutId,
    isDefault: false,
    ...extra,
  };
}

beforeEach(() => {
  uiState.appSettings = {};
  uiState.activePresetId = null;
  uiState.layoutLibrary = {
    byEffectType: {
      amp_nam: [
        makeEntry("tweed", "Tweed Face", { isFactory: true }),
        makeEntry("modern", "Modern Face"),
      ],
      "amp_nam_blend::blend-1": [makeEntry("blend-face", "Blend Face")],
    },
    defaults: {
      amp_nam: "tweed",
    },
    images: [],
  };
});

describe("layoutLookupKeysFor", () => {
  it("puts the blend-specific key first and falls back to the effect type", () => {
    expect(layoutLookupKeysFor("amp_nam_blend", "blend-1")).toEqual([
      "amp_nam_blend::blend-1",
      "amp_nam_blend",
    ]);
    expect(layoutLookupKeysFor("amp_nam")).toEqual(["amp_nam"]);
  });
});

describe("getAvailableLayoutEntries", () => {
  it("collects entries across lookup keys without duplicates", () => {
    const entries = getAvailableLayoutEntries("amp_nam");
    expect(entries.map((e) => e.layoutId)).toEqual(["tweed", "modern"]);
  });

  it("returns an empty list for effects with no layouts", () => {
    expect(getAvailableLayoutEntries("delay_digital")).toEqual([]);
  });
});

describe("resolveLayoutSelection", () => {
  it("falls back to the layout library default when no rules exist", () => {
    const selection = resolveLayoutSelection({ effectType: "amp_nam" });
    expect(selection).toMatchObject({ layoutId: "tweed", source: "libraryDefault" });
  });

  it("returns nothing when the effect has neither rules nor a default", () => {
    expect(resolveLayoutSelection({ effectType: "delay_digital" }))
      .toMatchObject({ layoutId: null, source: "none" });
  });

  it("lets an effect-type rule select the standard controls", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: STANDARD_LAYOUT_ID });
    const selection = resolveLayoutSelection({ effectType: "amp_nam" });
    expect(selection).toMatchObject({ layoutId: STANDARD_LAYOUT_ID, source: "effectType" });
    expect(resolveLayoutForNode({ effectType: "amp_nam" })).toBeNull();
  });

  it("prefers a matching keyword rule over the effect-type rule", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: STANDARD_LAYOUT_ID });
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "modern", keyword: "rectifier" });

    expect(resolveLayoutSelection({ effectType: "amp_nam", matchText: "mesa rectifier lead" }))
      .toMatchObject({ layoutId: "modern", source: "keyword" });
    // A node the keyword does not match still gets the effect-type rule.
    expect(resolveLayoutSelection({ effectType: "amp_nam", matchText: "vox ac30" }))
      .toMatchObject({ layoutId: STANDARD_LAYOUT_ID, source: "effectType" });
  });

  it("uses the longest matching keyword when several apply", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "tweed", keyword: "vox" });
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "modern", keyword: "vox ac30" });

    expect(resolveLayoutSelection({ effectType: "amp_nam", matchText: "vox ac30 top boost" }))
      .toMatchObject({ layoutId: "modern" });
    expect(resolveLayoutSelection({ effectType: "amp_nam", matchText: "vox ac15" }))
      .toMatchObject({ layoutId: "tweed" });
  });

  it("prefers a preset rule over keyword and effect-type rules", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "modern" });
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "tweed", keyword: "rectifier" });
    setLayoutPreference({
      lookupKey: "amp_nam",
      scope: "preset",
      layoutId: STANDARD_LAYOUT_ID,
      presetId: "preset-7",
      presetName: "Lead Tone",
    });

    expect(resolveLayoutSelection({
      effectType: "amp_nam",
      matchText: "mesa rectifier",
      presetId: "preset-7",
    })).toMatchObject({ layoutId: STANDARD_LAYOUT_ID, source: "preset" });

    // Another preset is unaffected.
    expect(resolveLayoutSelection({
      effectType: "amp_nam",
      matchText: "mesa rectifier",
      presetId: "preset-8",
    })).toMatchObject({ layoutId: "tweed", source: "keyword" });
  });

  it("checks the blend key before the effect-type key", () => {
    setLayoutPreference({ lookupKey: "amp_nam_blend", scope: "effectType", layoutId: STANDARD_LAYOUT_ID });
    setLayoutPreference({ lookupKey: "amp_nam_blend::blend-1", scope: "effectType", layoutId: "blend-face" });

    expect(resolveLayoutSelection({ effectType: "amp_nam_blend", blendId: "blend-1" }))
      .toMatchObject({ layoutId: "blend-face" });
    expect(resolveLayoutSelection({ effectType: "amp_nam_blend", blendId: "blend-2" }))
      .toMatchObject({ layoutId: STANDARD_LAYOUT_ID });
  });

  it("resolves a rule's layout id back to the layout object", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "modern" });
    expect(resolveLayoutForNode({ effectType: "amp_nam" })?.name).toBe("Modern Face");
  });

  it("ignores rules pointing at a layout that no longer exists", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "deleted-layout" });
    expect(resolveLayoutForNode({ effectType: "amp_nam" })).toBeNull();
  });
});

describe("selectStandardControls", () => {
  it("overrides the library default with an effect-type rule", () => {
    expect(selectStandardControls({ effectType: "amp_nam" })).toBe(true);
    expect(resolveLayoutForNode({ effectType: "amp_nam" })).toBeNull();
    expect(getLayoutPreferenceRules()).toMatchObject([
      { lookupKey: "amp_nam", scope: "effectType", layoutId: STANDARD_LAYOUT_ID },
    ]);
  });

  it("does nothing when the standard controls are already showing", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: STANDARD_LAYOUT_ID });
    expect(selectStandardControls({ effectType: "amp_nam" })).toBe(false);
    expect(selectStandardControls({ effectType: "delay_digital" })).toBe(false);
    expect(getLayoutPreferenceRules()).toHaveLength(1);
  });

  it("replaces the keyword rule that chose the layout, leaving other matches alone", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "modern", keyword: "rectifier" });
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "tweed" });

    expect(selectStandardControls({ effectType: "amp_nam", matchText: "mesa rectifier lead" })).toBe(true);
    expect(resolveLayoutSelection({ effectType: "amp_nam", matchText: "mesa rectifier lead" }))
      .toMatchObject({ layoutId: STANDARD_LAYOUT_ID, source: "keyword" });
    expect(resolveLayoutForNode({ effectType: "amp_nam", matchText: "vox ac30" })?.name).toBe("Tweed Face");
    expect(getLayoutPreferenceRules()).toHaveLength(2);
  });

  it("replaces the preset rule that chose the layout", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "preset", layoutId: "modern", presetId: "preset-7", presetName: "Lead" });

    expect(selectStandardControls({ effectType: "amp_nam", presetId: "preset-7" })).toBe(true);
    expect(resolveLayoutSelection({ effectType: "amp_nam", presetId: "preset-7" }))
      .toMatchObject({ layoutId: STANDARD_LAYOUT_ID, source: "preset" });
    expect(resolveLayoutForNode({ effectType: "amp_nam", presetId: "preset-8" })?.name).toBe("Tweed Face");
  });

  it("records a blend's choice on the blend key", () => {
    setLayoutPreference({ lookupKey: "amp_nam_blend::blend-1", scope: "effectType", layoutId: "blend-face" });

    expect(selectStandardControls({ effectType: "amp_nam_blend", blendId: "blend-1" })).toBe(true);
    expect(resolveLayoutForNode({ effectType: "amp_nam_blend", blendId: "blend-1" })).toBeNull();
    expect(getLayoutPreferenceRules()).toMatchObject([
      { lookupKey: "amp_nam_blend::blend-1", scope: "effectType", layoutId: STANDARD_LAYOUT_ID },
    ]);
  });
});

describe("rule storage", () => {
  it("persists rules into app settings", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "modern" });
    const stored = uiState.appSettings?.[LAYOUT_PREFERENCES_SETTING];
    expect(Array.isArray(stored)).toBe(true);
    expect(getLayoutPreferenceRules()).toHaveLength(1);
  });

  it("replaces rather than duplicates a rule for the same scope", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "modern" });
    setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "tweed" });
    const rules = getLayoutPreferenceRules();
    expect(rules).toHaveLength(1);
    expect(rules[0].layoutId).toBe("tweed");
  });

  it("keeps keyword rules with different keywords apart", () => {
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "modern", keyword: "AC30" });
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "tweed", keyword: "twin" });
    expect(getLayoutPreferenceRules()).toHaveLength(2);
    // Keywords are normalized to lower case for matching.
    expect(getLayoutPreferenceRules()[0].keyword).toBe("ac30");
  });

  it("removes a single rule and clears by key", () => {
    const rule = setLayoutPreference({ lookupKey: "amp_nam", scope: "effectType", layoutId: "modern" });
    setLayoutPreference({ lookupKey: "amp_nam", scope: "keyword", layoutId: "tweed", keyword: "twin" });
    removeLayoutPreference(rule.id);
    expect(getLayoutPreferenceRules()).toHaveLength(1);
    clearLayoutPreferencesForKeys(["amp_nam"]);
    expect(getLayoutPreferenceRules()).toHaveLength(0);
  });

  it("drops malformed persisted entries", () => {
    uiState.appSettings = {
      [LAYOUT_PREFERENCES_SETTING]: [
        { lookupKey: "amp_nam", scope: "effectType", layoutId: "modern", id: "ok" },
        { lookupKey: "amp_nam", scope: "keyword", layoutId: "modern" }, // missing keyword
        { scope: "effectType", layoutId: "modern" }, // missing lookupKey
        "nonsense",
      ],
    };
    const rules = getLayoutPreferenceRules();
    expect(rules).toHaveLength(1);
    expect(rules[0].id).toBe("ok");
  });
});

describe("keyword helpers", () => {
  it("builds a lower-cased haystack, skipping empty parts", () => {
    expect(buildLayoutMatchText(["Mesa Rectifier", null, "  ", "NAM Model"]))
      .toBe("mesa rectifier nam model");
  });

  it("suggests distinctive make/model tokens", () => {
    const suggestions = suggestLayoutKeywords("fender twin reverb amp model 1959");
    expect(suggestions).toContain("fender");
    expect(suggestions).toContain("twin");
    // Generic words and bare numbers are not useful match keys.
    expect(suggestions).not.toContain("amp");
    expect(suggestions).not.toContain("model");
    expect(suggestions).not.toContain("1959");
  });
});
