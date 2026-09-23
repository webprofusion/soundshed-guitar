/**
 * "presetList" also arrives unasked: the engine answers a delete with it, and shared sync
 * brings one. Taking it must not undo what the library is showing.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

window.IPlugSendMsg = () => undefined;

const { uiState } = await import("../ts/state.js");
const { onPresetList } = await import("../ts/messages/presetHandlers.js");
const { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID } = await import("../ts/presets/sorting.js");
const { isStoredPreset } = await import("../ts/presetLibraryStore.js");

const summary = (id: string) => ({ id, name: id, category: "User" });

beforeEach(() => {
  uiState.presets = [];
  uiState.filteredPresets = [];
  uiState.presetCache = new Map();
  uiState.activePresetId = null;
  uiState.activePresetIsNew = false;
  uiState.activePresetFolderId = PRESET_FOLDER_ALL_ID;
  uiState.presetFavorites = new Set();
});

describe("taking the engine's preset list", () => {
  it("keeps the folder the library is showing", () => {
    uiState.activePresetFolderId = PRESET_FOLDER_FAVORITES_ID;
    uiState.presetFavorites = new Set(["b"]);

    onPresetList({ type: "presetList", presets: [summary("a"), summary("b"), summary("c")] });

    expect(uiState.presets.map((preset) => preset.id)).toEqual(["a", "b", "c"]);
    expect(uiState.filteredPresets.map((preset) => preset.id)).toEqual(["b"]);
  });

  it("keeps a new preset listed until it is saved, without taking it for a stored one", () => {
    uiState.presetCache.set("user-new", { id: "user-new", name: "New Preset", category: "User" } as Preset);
    uiState.activePresetId = "user-new";
    uiState.activePresetIsNew = true;

    onPresetList({ type: "presetList", presets: [summary("a")] });

    expect(uiState.presets.map((preset) => preset.id)).toEqual(["user-new", "a"]);
    expect(isStoredPreset("a")).toBe(true);
    expect(isStoredPreset("user-new")).toBe(false);
  });
});
