import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

const postMessage = vi.fn();
const showConfirm = vi.fn<(message: string, title?: string) => Promise<boolean>>();

vi.mock("../ts/bridge.js", () => ({ postMessage: (...args: unknown[]) => postMessage(...args) }));
vi.mock("../ts/dialogs.js", () => ({ showConfirm: (message: string, title?: string) => showConfirm(message, title) }));
vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));
vi.mock("../ts/dataLibraries.js", () => ({ REMOTE_BASE_URL: "", getDefaultPresets: () => [] }));
vi.mock("../ts/presets/favorites.js", () => ({ setFavoriteToggleState: vi.fn() }));
vi.mock("../ts/presets/toolbar.js", () => ({ updatePresetActionButtons: vi.fn() }));
vi.mock("../ts/presets/history.js", () => ({ recordPresetInHistory: vi.fn() }));
vi.mock("../ts/presets/filter.js", () => ({
  renderActivePreset: vi.fn(),
  updatePresetDropdownSelection: vi.fn(),
}));

const { uiState } = await import("../ts/state.js");
const { setStoredPresetIds } = await import("../ts/presetLibraryStore.js");
const { applySetlistCursorFromBackend, selectSetlistSlot, setActiveSetlist } = await import("../ts/presets/setlists.js");
const { applyPresetFromLibrary } = await import("../ts/presets/load.js");

function preset(id: string): Preset {
  return {
    id,
    name: id,
    category: "User",
    version: 2,
    graph: { nodes: [{ id: "in", type: "input", params: {} }, { id: "out", type: "output", params: {} }], edges: [] },
  } as unknown as Preset;
}

function sent(type: string): unknown[] {
  return postMessage.mock.calls.map(([message]) => message).filter((message) => (message as { type?: string }).type === type);
}

beforeEach(() => {
  postMessage.mockReset();
  showConfirm.mockReset();
  uiState.setlists = [{ id: "set-1", name: "Set 1", slots: [{ presetId: "wah-tone" }, { presetId: "clean" }] }] as never;
  uiState.activeSetlistId = "set-1";
  uiState.activePresetId = "wah-tone";
  uiState.mixer = { activePresetIds: ["wah-tone"], presets: {}, masterGain: 1, mixGainDb: 0 } as never;
  uiState.presetDirty = true;
  uiState.presetLoadingId = null;
  uiState.presetCache = new Map([["wah-tone", preset("wah-tone")], ["clean", preset("clean")]]);
  uiState.activePresetSceneId = null;
  setStoredPresetIds([]);
});

describe("setlist slots", () => {
  it("a slot for the preset being edited moves the cursor without asking or waiting for a load", async () => {
    await selectSetlistSlot(0);

    expect(showConfirm).not.toHaveBeenCalled();
    expect(sent("setSetlistCursor")).toEqual([{ type: "setSetlistCursor", cursorIndex: 0 }]);
    expect(uiState.presetLoadingId).toBeNull();
    expect(uiState.presetDirty).toBe(true);
  });

  it("a slot for another preset still asks before discarding edits, then loads it", async () => {
    showConfirm.mockResolvedValue(true);

    await selectSetlistSlot(1);

    expect(showConfirm).toHaveBeenCalledTimes(1);
    expect(sent("setSetlistCursor")).toHaveLength(1);
    expect(uiState.presetLoadingId).toBe("clean");
  });

  it("the preset's own slot is still a load while Multi-Rig plays others beside it", async () => {
    uiState.mixer!.activePresetIds = ["wah-tone", "clean"];

    await selectSetlistSlot(0);

    expect(uiState.presetLoadingId).toBe("wah-tone");
  });

  it("the cursor report ends a loading state no load will follow", () => {
    uiState.presetLoadingId = "wah-tone";
    applySetlistCursorFromBackend(0, "wah-tone", "set-1");
    expect(uiState.presetLoadingId).toBeNull();
  });

  it("the cursor report leaves a switch still in flight alone", () => {
    uiState.presetLoadingId = "clean";
    applySetlistCursorFromBackend(1, "clean", "set-1");
    expect(uiState.presetLoadingId).toBe("clean");
  });
});

describe("loading a preset from the library", () => {
  it("asks before reloading the preset being edited over its unsaved changes", async () => {
    showConfirm.mockResolvedValue(false);

    await applyPresetFromLibrary("wah-tone");

    expect(showConfirm).toHaveBeenCalledTimes(1);
    expect(sent("loadPreset")).toHaveLength(0);
    expect(uiState.presetDirty).toBe(true);
  });

  it("reloads it once the discard is confirmed", async () => {
    showConfirm.mockResolvedValue(true);

    await applyPresetFromLibrary("wah-tone");

    expect(sent("loadPreset")).toHaveLength(1);
    expect(uiState.presetDirty).toBe(false);
  });

  it("does not ask when there is nothing unsaved", async () => {
    uiState.presetDirty = false;

    await applyPresetFromLibrary("wah-tone");

    expect(showConfirm).not.toHaveBeenCalled();
    expect(sent("loadPreset")).toHaveLength(1);
  });
});

describe("loading a preset the engine stores", () => {
  beforeEach(() => {
    uiState.presetDirty = false;
    setStoredPresetIds(["wah-tone", "clean", "listed-only"]);
    uiState.presetCache.set("listed-only", { id: "listed-only", name: "Listed only", category: "Factory" } as Preset);
    uiState.presetCache.set("shared", preset("shared"));
    uiState.activePresetSceneId = "scene-2";
  });

  it("sends only its id, drawing the cached copy at once", async () => {
    await applyPresetFromLibrary("clean");

    // "clean" has no scene-2, so it opens on its first scene, as a body load would.
    expect(sent("loadPreset")).toEqual([{ type: "loadPreset", presetId: "clean", sceneId: "scene-1" }]);
    expect(uiState.activePresetId).toBe("clean");
    expect(uiState.activePresetDraft?.id).toBe("clean");
    expect(uiState.presetLoadingId).toBe("clean");
  });

  it("with only the library's summary, keeps the playing preset on screen until the engine answers", async () => {
    await applyPresetFromLibrary("listed-only");

    expect(sent("loadPreset")).toEqual([{ type: "loadPreset", presetId: "listed-only", sceneId: "scene-2" }]);
    expect(uiState.activePresetId).toBe("wah-tone");
    expect(uiState.presetLoadingId).toBe("listed-only");
  });

  it("still sends the body of a preset the engine does not store", async () => {
    await applyPresetFromLibrary("shared");

    const [message] = sent("loadPreset") as Array<{ presetId?: string; preset?: Preset }>;
    expect(message.preset?.id).toBe("shared");
  });
});

describe("choosing a setlist", () => {
  it("asks the engine to activate it, without re-sending the setlists", () => {
    uiState.setlists = [...uiState.setlists, { id: "set-2", name: "Set 2", slots: [] }] as never;
    uiState.setlistCursorIndex = 1;

    setActiveSetlist("set-2");

    expect(sent("selectSetlist")).toEqual([{ type: "selectSetlist", setlistId: "set-2" }]);
    expect(sent("setSetlists")).toHaveLength(0);
    expect(uiState.activeSetlistId).toBe("set-2");
    expect(uiState.setlistCursorIndex).toBe(0);
  });
});
