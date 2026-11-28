/**
 * Save and Save As are spelled out in the preset toolbar. Save was a bare floppy disk and Save As
 * was only in the "..." menu, so neither was easy to find. The toolbar's Save As does what the
 * menu's does, and follows the same rules for when it is available.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

const showNotification = vi.fn();

vi.mock("../ts/notifications.js", () => ({
  clearNotification: vi.fn(),
  showNotification: (...args: unknown[]) => showNotification(...args),
}));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));

const TOOLBAR = readFileSync(join(__dirname, "..", "ui-components", "preset-toolbar-row.html"), "utf8");

const { uiState } = await import("../ts/state.js");
const { updatePresetActionButtons } = await import("../ts/presets/toolbar.js");
const { initializeSaveAsButton } = await import("../ts/presets/saveModal.js");

const button = (id: string) => document.getElementById(id) as HTMLButtonElement;
const label = (id: string) => button(id).querySelector(".preset-action-btn-label")?.textContent?.trim();

function preset(id: string): Preset {
  return { id, name: id, category: "User", version: 2, graph: { nodes: [], edges: [] } } as unknown as Preset;
}

function activate(id: string, isNew = false): void {
  uiState.activePresetId = id;
  uiState.activePresetIsNew = isNew;
  uiState.presetCache = new Map([[id, preset(id)]]);
  updatePresetActionButtons();
}

beforeEach(() => {
  document.body.innerHTML = TOOLBAR;
  uiState.compositeEditMode = false;
  uiState.presetDirty = false;
});

describe("the preset toolbar's Save and Save As", () => {
  it("name themselves with a visible label, not an icon alone", () => {
    expect(label("preset-save-btn")).toBe("Save");
    expect(label("preset-save-as-toolbar-btn")).toBe("Save As");
  });

  it("are both available for one of your own presets", () => {
    activate("user-my-preset");
    expect(button("preset-save-btn").disabled).toBe(false);
    expect(button("preset-save-as-toolbar-btn").hidden).toBe(false);
    expect(button("preset-save-as-toolbar-btn").disabled).toBe(false);
  });

  it("leave Save As as the way to keep a copy of a factory preset, and say so", () => {
    activate("factory-clean");
    expect(button("preset-save-btn").disabled).toBe(true);
    expect(button("preset-save-btn").title).toContain("Save As");
    expect(button("preset-save-as-toolbar-btn").disabled).toBe(false);
  });

  it("offer no Save As for a new preset that has not been given its details", () => {
    activate("user-new-draft", true);
    expect(button("preset-save-as-toolbar-btn").hidden).toBe(true);
    expect(button("preset-save-as-toolbar-btn").disabled).toBe(true);
    expect(button("preset-save-as-btn").hidden).toBe(true);
  });

  it("both run Save As: the toolbar's and the menu's", () => {
    uiState.compositeEditMode = true; // Save As refuses with a notice, which shows it ran
    uiState.compositeEditPreset = preset("__composite_edit_comp-1__");
    initializeSaveAsButton();
    showNotification.mockReset();

    button("preset-save-as-toolbar-btn").click();
    expect(showNotification).toHaveBeenCalledTimes(1);
    button("preset-save-as-btn").click();
    expect(showNotification).toHaveBeenCalledTimes(2);
  });
});
