/**
 * The engine's preset list carries each preset's tags and description. The library filters
 * and searches on them, so they have to survive the trip into the UI's state: before they
 * did, the tag filter bar only found presets that had already been opened.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

window.IPlugSendMsg = () => {};

async function load() {
  vi.resetModules();
  const { uiState } = await import("../ts/state.js");
  const { onPresetList } = await import("../ts/messages/presetHandlers.js");
  const { activeTagFilters, filterPresets } = await import("../ts/presets/filter.js");
  return { uiState, onPresetList, activeTagFilters, filterPresets };
}

const LIST = {
  presets: [
    { id: "a", name: "Tight Rhythm", category: "Factory", source: "factory", tags: ["high-gain", "rhythm"], description: "chug" },
    { id: "b", name: "Singing Lead", category: "Factory", source: "factory", tags: ["high-gain", "lead"] },
    { id: "c", name: "Warm Clean", category: "Factory", source: "factory", tags: ["clean"] },
    { id: "d", name: "Untagged", category: "User", source: "user" },
  ],
};

describe("the preset list", () => {
  beforeEach(() => {
    document.body.innerHTML = "";
  });

  it("keeps the tags and description the engine sends", async () => {
    const { uiState, onPresetList } = await load();
    onPresetList(LIST);
    const byId = new Map(uiState.presets.map((preset) => [preset.id, preset]));
    expect(byId.get("a")?.tags).toEqual(["high-gain", "rhythm"]);
    expect(byId.get("a")?.description).toBe("chug");
    expect(byId.get("c")?.tags).toEqual(["clean"]);
    expect(byId.get("d")?.tags ?? []).toEqual([]);
  });

  it("so presets that were never opened can be told apart by tag", async () => {
    const { uiState, onPresetList } = await load();
    onPresetList(LIST);
    const tagged = (tags: string[]) =>
      uiState.presets.filter((preset) => tags.every((tag) => (preset.tags ?? []).includes(tag))).map((preset) => preset.id);
    expect(tagged(["high-gain"])).toEqual(["a", "b"]);
    expect(tagged(["high-gain", "lead"])).toEqual(["b"]);
    expect(tagged(["clean"])).toEqual(["c"]);
  });

  it("and a second list does not lose the tags of a preset already cached", async () => {
    const { uiState, onPresetList } = await load();
    onPresetList(LIST);
    onPresetList({ presets: [{ id: "b", name: "Singing Lead", category: "Factory", source: "factory" }] });
    expect(uiState.presets.find((preset) => preset.id === "b")?.tags).toEqual(["high-gain", "lead"]);
  });
});
