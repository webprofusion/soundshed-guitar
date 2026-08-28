import { describe, expect, it } from "vitest";
import { LOOP_NAME_TEMPLATES, suggestLoopTemplateName } from "../ts/practiceTool.js";

describe("suggestLoopTemplateName", () => {
  it("suffixes the first pick of a template with 1", () => {
    expect(suggestLoopTemplateName("Verse", [])).toBe("Verse 1");
  });

  it("increments the suffix each time the same template is picked again", () => {
    const existing = ["Verse 1"];
    expect(suggestLoopTemplateName("Verse", existing)).toBe("Verse 2");
  });

  it("fills the lowest free suffix rather than always incrementing to the max", () => {
    const existing = ["Verse 1", "Verse 3"];
    expect(suggestLoopTemplateName("Verse", existing)).toBe("Verse 2");
  });

  it("keeps separate templates independent of one another", () => {
    const existing = ["Verse 1", "Verse 2", "Chorus 1"];
    expect(suggestLoopTemplateName("Chorus", existing)).toBe("Chorus 2");
    expect(suggestLoopTemplateName("Solo", existing)).toBe("Solo 1");
  });

  it("ignores unrelated loop names and names for other templates with a shared prefix", () => {
    const existing = ["Verse", "Verse (intro)", "Pre-Chorus 1"];
    // Plain "Verse" (no numeric suffix) and "Verse (intro)" don't match the
    // "Verse <n>" pattern, so they don't reserve a number.
    expect(suggestLoopTemplateName("Verse", existing)).toBe("Verse 1");
  });

  it("returns every declared template as a non-empty string", () => {
    expect(LOOP_NAME_TEMPLATES.length).toBeGreaterThan(0);
    LOOP_NAME_TEMPLATES.forEach((template) => {
      expect(suggestLoopTemplateName(template, [])).toBe(`${template} 1`);
    });
  });
});
