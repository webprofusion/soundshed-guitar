import { describe, expect, it } from "vitest";
import { resolveImportedPresetName } from "../ts/presets.js";

describe("resolveImportedPresetName", () => {
  it("prefers the Tone Sharing API title for item imports", () => {
    expect(resolveImportedPresetName(
      { id: "preset-1", name: "Archived Title" } as any,
      { source: "toneSharingApi", itemId: "item-1", titleHint: "API Title" },
    )).toBe("API Title");
  });

  it("keeps the archive title for non-item Tone Sharing imports", () => {
    expect(resolveImportedPresetName(
      { id: "preset-1", name: "Archived Title" } as any,
      { source: "toneSharingApi", packId: "pack-1", titleHint: "Pack Title" },
    )).toBe("Archived Title");
  });
});
