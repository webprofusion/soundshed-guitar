import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

const postMessage = vi.fn();
vi.mock("../ts/bridge.js", () => ({
  setAppSetting: (key: string, value: unknown) => postMessage({ type: "setSetting", key, value }),
}));

const { applyEnginePresetDirty, uiState } = await import("../ts/state.js");
const library = await import("../ts/presetLibraryStore.js");
const mixer = await import("../ts/mixerStore.js");
const settings = await import("../ts/appSettingsStore.js");

const preset = (id: string, name = id): Preset => ({ id, name }) as Preset;

beforeEach(() => {
  postMessage.mockClear();
  uiState.presets = [];
  uiState.filteredPresets = [];
  uiState.presetCache = new Map();
  uiState.activePresetId = null;
  uiState.mixer = undefined;
  uiState.appSettings = {};
});

describe("preset library store", () => {
  it("resets the list, the unfiltered view and the cache from an index", () => {
    library.resetLibrary([preset("a"), preset("b")]);
    expect(uiState.presets.map((p) => p.id)).toEqual(["a", "b"]);
    expect(uiState.filteredPresets).not.toBe(uiState.presets);
    expect(uiState.filteredPresets.map((p) => p.id)).toEqual(["a", "b"]);
    expect([...uiState.presetCache.keys()]).toEqual(["a", "b"]);
  });

  it("keeps an existing entry on add-if-missing, but replaces it on upsert", () => {
    library.setLibraryPresets([preset("a", "old")]);
    library.addLibraryPresetIfMissing(preset("a", "new"));
    expect(uiState.presets.map((p) => p.name)).toEqual(["old"]);
    library.upsertLibraryPreset(preset("a", "new"));
    library.upsertLibraryPreset(preset("b"));
    expect(uiState.presets.map((p) => `${p.id}:${p.name}`)).toEqual(["a:new", "b:b"]);
  });

  it("replaces only an entry that is there", () => {
    library.setLibraryPresets([preset("a")]);
    expect(library.replaceLibraryPreset(preset("x"), "missing")).toBe(false);
    expect(library.replaceLibraryPreset(preset("a", "renamed"))).toBe(true);
    expect(uiState.presets.map((p) => p.name)).toEqual(["renamed"]);
  });

  it("moves a preset to the top without duplicating it, leaving the filter alone", () => {
    library.setLibraryPresets([preset("a"), preset("b")]);
    library.putLibraryPresetFirst(preset("b", "saved"));
    expect(uiState.presets.map((p) => `${p.id}:${p.name}`)).toEqual(["b:saved", "a:a"]);
    expect(uiState.filteredPresets.map((p) => p.id)).toEqual(["a", "b"]);
  });

  it("removes presets from the list, the filtered view and the cache together", () => {
    library.resetLibrary([preset("a"), preset("b"), preset("c")]);
    library.removeLibraryPresets(["a", "c"]);
    expect(uiState.presets.map((p) => p.id)).toEqual(["b"]);
    expect(uiState.filteredPresets.map((p) => p.id)).toEqual(["b"]);
    expect([...uiState.presetCache.keys()]).toEqual(["b"]);
  });

  it("caches under a different key when asked", () => {
    library.cachePreset(preset("a"), "slot-1");
    expect(uiState.presetCache.get("slot-1")?.id).toBe("a");
  });

  it("knows which presets the engine can load by id: its list, what it saved, less what is deleted", () => {
    library.setStoredPresetIds(["factory-1", "user-1"]);
    library.markPresetStored("user-2");
    expect(["factory-1", "user-1", "user-2", "shared-1"].map(library.isStoredPreset)).toEqual([true, true, true, false]);

    library.resetLibrary([preset("user-1")]);
    library.removeLibraryPresets(["user-1"]);
    expect(library.isStoredPreset("user-1")).toBe(false);

    library.setStoredPresetIds(["factory-1"]);
    expect(library.isStoredPreset("user-2")).toBe(false);
  });
});

describe("unsaved changes", () => {
  beforeEach(() => {
    uiState.presetDirty = false;
  });

  it("takes the engine's flag for a preset that has just replaced another, absent meaning clean", () => {
    applyEnginePresetDirty(true, true);
    expect(uiState.presetDirty).toBe(true);
    applyEnginePresetDirty(undefined, true);
    expect(uiState.presetDirty).toBe(false);
  });

  it("lets the engine flag the preset on screen dirty, but not clear an edit the UI has just flagged", () => {
    applyEnginePresetDirty(true, false);
    expect(uiState.presetDirty).toBe(true);
    // The engine re-checks on idle, so this false can predate the edit; presetDirtyChanged clears it.
    applyEnginePresetDirty(false, false);
    expect(uiState.presetDirty).toBe(true);
  });
});

describe("mixer store", () => {
  it("creates the mixer on a slot update and gives new slots default settings", () => {
    mixer.setMixerSlots(["a", "b"]);
    expect(uiState.mixer?.activePresetIds).toEqual(["a", "b"]);
    expect(uiState.mixer?.presets.b).toEqual({ id: "b", mix: 1, pan: 0, mute: false, solo: false });
  });

  it("merges an engine update over what each slot already has", () => {
    mixer.setMixerSlots(["a"]);
    mixer.updateMixerSlot("a", { pan: -0.5 });
    mixer.mergeMixerState({ mixGainDb: -6, presets: { a: { mute: true } } });
    expect(uiState.mixer?.presets.a).toEqual({ id: "a", mix: 1, pan: -0.5, mute: true, solo: false });
    expect(uiState.mixer?.mixGainDb).toBe(-6);
    expect(uiState.mixer?.activePresetIds).toEqual(["a"]);
  });

  it("removes a slot with its settings, and ignores slot edits for presets not in it", () => {
    mixer.setMixerSlots(["a", "b"]);
    mixer.removeMixerSlot("a");
    mixer.updateMixerSlot("a", { solo: true });
    expect(uiState.mixer?.activePresetIds).toEqual(["b"]);
    expect(uiState.mixer?.presets.a).toBeUndefined();
  });

  it("does nothing to levels or slots before the engine has sent a mixer", () => {
    mixer.setMixerMixGainDb(-6);
    mixer.addMixerSlot("a", "A");
    expect(uiState.mixer).toBeUndefined();
  });
});

describe("app settings store", () => {
  it("records a change and sends the same value to the engine", () => {
    settings.updateAppSetting("ui.density", "compact");
    expect(settings.getAppSetting("ui.density")).toBe("compact");
    expect(postMessage).toHaveBeenCalledWith({ type: "setSetting", key: "ui.density", value: "compact" });
  });

  it("takes a full set from the engine without sending anything back", () => {
    settings.replaceAppSettings({ a: 1 });
    expect(settings.getAppSetting("a")).toBe(1);
    expect(settings.getAppSetting("missing")).toBeNull();
    expect(postMessage).not.toHaveBeenCalled();
  });

  it("records one setting the engine changed itself without sending it back", () => {
    settings.recordAppSetting("resources.favorites", ["ir-2"]);
    expect(settings.getAppSetting("resources.favorites")).toEqual(["ir-2"]);
    expect(postMessage).not.toHaveBeenCalled();
  });
});
