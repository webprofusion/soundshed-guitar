/**
 * The looper strip: the Loop button appears with the Looper flag in the standalone app, the
 * engine is switched on and off with the flag, the strip sends the transport, the modes and the
 * layer edits, shows a count-in in large type, and collapses at compact density.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/dialogs.js", () => ({ showConfirm: vi.fn(async () => true) }));

document.body.innerHTML = `
  <div class="looper-strip" id="looper-strip" hidden></div>
  <button id="footer-looper-btn" type="button" hidden></button>
  <button id="footer-metronome-btn" type="button"></button>
`;

type Sent = Record<string, unknown> & { type: string };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { uiState } = await import("../ts/state.js");
const { FEATURE_FLAGS_CHANGED_EVENT } = await import("../ts/featureFlags.js");
const { initializeLooper } = await import("../ts/looper.js");
const { onLooperLayers, onLooperProjects, onLooperState } = await import("../ts/messages/looperHandlers.js");
const { showConfirm } = await import("../ts/dialogs.js");
const { showNotification } = await import("../ts/notifications.js");

const strip = document.getElementById("looper-strip") as HTMLElement;
const button = document.getElementById("footer-looper-btn") as HTMLButtonElement;

function setFlag(on: boolean): void {
  uiState.appSettings = { ...(uiState.appSettings ?? {}), "features.looper.enabled": on };
  document.dispatchEvent(new Event(FEATURE_FLAGS_CHANGED_EVENT));
}

function state(fields: Record<string, unknown> = {}, settings: Record<string, unknown> = {}): void {
  onLooperState({
    type: "looperState",
    available: true,
    enabled: true,
    state: "empty",
    playing: false,
    positionSec: 0,
    lengthSec: 0,
    canUndo: false,
    canRedo: false,
    layerCount: 0,
    memoryMb: 70,
    tempoLocked: false,
    settings: { enabled: true, start: "press", length: "free", bars: 4, countInBars: 1, ...settings },
    ...fields,
  });
}

initializeLooper();

describe("the looper strip", () => {
  beforeEach(() => {
    sent = [];
  });

  it("asks for the looper's state when it starts", () => {
    initializeLooper();
    expect(sent.some((m) => m.type === "getLooperState")).toBe(true);
  });

  it("switches the engine on with the flag, and shows the Loop button", () => {
    setFlag(true);
    state({ enabled: false }, { enabled: false });
    expect(sent).toContainEqual({ type: "setLooper", enabled: true });
    expect(button.hidden).toBe(false);
    expect(strip.hidden).toBe(true);
  });

  it("opens from the Loop button and sends the transport", () => {
    setFlag(true);
    state();
    button.click();
    expect(strip.hidden).toBe(false);
    const rec = strip.querySelector<HTMLButtonElement>('[data-looper-command="recPlayDub"]');
    expect(rec?.textContent).toBe("Rec");
    rec?.click();
    expect(sent).toContainEqual({ type: "looperCommand", command: "recPlayDub" });
  });

  it("sends the record modes and the bar count", () => {
    state({}, { length: "bars", bars: 4 });
    strip.querySelector<HTMLButtonElement>('[data-looper-set="start"][data-value="armed"]')?.click();
    strip.querySelector<HTMLButtonElement>('[data-looper-bars="1"]')?.click();
    expect(sent).toContainEqual({ type: "setLooper", start: "armed" });
    expect(sent).toContainEqual({ type: "setLooper", bars: 5 });
  });

  it("switches the loop's click, which is the metronome", () => {
    state({ click: false });
    const toggle = () => strip.querySelector<HTMLButtonElement>("[data-looper-click]");
    expect(toggle()?.getAttribute("aria-pressed")).toBe("false");
    toggle()?.click();
    expect(sent).toContainEqual({ type: "setMetronome", enabled: true });
    state({ click: true });
    expect(toggle()?.getAttribute("aria-pressed")).toBe("true");
    toggle()?.click();
    expect(sent).toContainEqual({ type: "setMetronome", enabled: false });
  });

  it("shows the count-in in large type and the state the loop is in", () => {
    state({ state: "countIn", countInBeatsLeft: 3 }, { length: "bars" });
    expect(strip.querySelector(".looper-countin")?.textContent).toBe("3");
    state({ state: "playing", playing: true, lengthSec: 8, positionSec: 2, bars: 4, barIndex: 2, layerCount: 1 });
    expect(strip.dataset.looperState).toBe("playing");
    expect(strip.querySelector(".looper-time")?.textContent).toBe("0:02.0 / 0:08.0");
    expect(strip.querySelector(".looper-bar")?.textContent).toBe("bar 2 of 4");
    expect(strip.querySelector('[data-looper-command="recPlayDub"]')?.textContent).toBe("Dub");
  });

  it("edits a layer from the layers panel", () => {
    state({ state: "playing", playing: true, lengthSec: 8, layerCount: 1 });
    onLooperLayers({
      type: "looperLayers",
      layers: [{ id: "L1", title: "Layer 1", order: 1, level: 1, balance: 0, muted: false, solo: false, tone: { presetName: "Clean <Amp>" } }],
    });
    strip.querySelector<HTMLButtonElement>("[data-looper-layers]")?.click();
    const row = strip.querySelector<HTMLElement>('.looper-layer-row[data-layer-id="L1"]');
    expect(row?.querySelector(".looper-tone-chip")?.innerHTML).toContain("Clean &lt;Amp&gt;");
    row?.querySelector<HTMLButtonElement>("[data-looper-layer-mute]")?.click();
    const level = row?.querySelector<HTMLInputElement>("[data-looper-layer-level]");

    if (level) {
      level.value = "0.5";
      level.dispatchEvent(new Event("input", { bubbles: true }));
    }

    row?.querySelector<HTMLButtonElement>("[data-looper-layer-remove]")?.click();
    expect(sent).toContainEqual({ type: "setLooperLayer", layerId: "L1", muted: true });
    expect(sent).toContainEqual({ type: "setLooperLayer", layerId: "L1", level: 0.5 });
    expect(sent).toContainEqual({ type: "removeLooperLayer", layerId: "L1" });
  });

  it("saves the loop under the name typed, and opens, deletes and exports", async () => {
    state({ state: "playing", playing: true, lengthSec: 8, layerCount: 1, dirty: true });
    onLooperProjects({ type: "looperProjects", projects: [{ id: "p1", title: "Verse", lengthSec: 8, layerCount: 2 }] });
    const title = strip.querySelector<HTMLInputElement>("[data-looper-title]");

    if (title) {
      title.value = "Chorus";
    }

    strip.querySelector<HTMLButtonElement>("[data-looper-save]")?.click();
    expect(sent).toContainEqual({ type: "saveLooperProject", title: "Chorus" });

    const row = strip.querySelector<HTMLElement>('.looper-project-row[data-project-id="p1"]');
    expect(row?.textContent).toContain("Verse");
    row?.querySelector<HTMLButtonElement>("[data-looper-open]")?.click();
    await Promise.resolve();
    await Promise.resolve();
    expect(vi.mocked(showConfirm)).toHaveBeenCalled();
    expect(sent).toContainEqual({ type: "loadLooperProject", projectId: "p1" });
    row?.querySelector<HTMLButtonElement>("[data-looper-delete]")?.click();
    await Promise.resolve();
    await Promise.resolve();
    expect(sent).toContainEqual({ type: "deleteLooperProject", projectId: "p1" });

    onLooperLayers({ type: "looperLayers", layers: [{ id: "L1", title: "Layer 1", order: 1, tone: {} }] });
    if (strip.querySelector(".looper-layers")?.hasAttribute("hidden")) {
      strip.querySelector<HTMLButtonElement>("[data-looper-layers]")?.click();
    }
    strip.querySelector<HTMLButtonElement>('[data-looper-layer-export="riff"]')?.click();
    expect(sent).toContainEqual({ type: "exportLooperLayer", layerId: "L1", as: "riff" });
  });

  it("re-amps a layer, reverts it, re-amps them all, and adds a riff as a layer", () => {
    uiState.riffLibrary = { path: "", riffs: [{ id: "r1", title: "Intro riff", categories: [], tags: [], takes: [{ id: "t1" } as never] }] };
    state({ state: "playing", playing: true, lengthSec: 8, layerCount: 1 });
    onLooperLayers({
      type: "looperLayers",
      layers: [{ id: "L1", title: "Layer 1", order: 1, wetSource: "reamp", canRevert: true, tone: { presetName: "Lead" } }],
    });

    if (strip.querySelector(".looper-layers")?.hasAttribute("hidden")) {
      strip.querySelector<HTMLButtonElement>("[data-looper-layers]")?.click();
    }

    strip.querySelector<HTMLButtonElement>("[data-looper-layer-retone]")?.click();
    strip.querySelector<HTMLButtonElement>("[data-looper-layer-revert]")?.click();
    strip.querySelector<HTMLButtonElement>("[data-looper-retone-all]")?.click();
    expect(sent).toContainEqual({ type: "retoneLooperLayer", layerIds: ["L1"] });
    expect(sent).toContainEqual({ type: "retoneLooperLayer", layerIds: ["L1"], revert: true });
    expect(sent).toContainEqual({ type: "retoneLooperLayer", layerIds: "all" });

    onLooperLayers({
      type: "looperLayers",
      layers: [{ id: "L1", title: "Layer 1", order: 1, retone: { progress: 0.4 }, tone: { presetName: "Lead" } }],
    });
    const chip = strip.querySelector<HTMLButtonElement>("[data-looper-layer-retone]");
    expect(chip?.textContent).toContain("Re-amping 40%");
    expect(chip?.disabled).toBe(true);

    onLooperProjects({ type: "looperProjects", projects: [] });
    strip.querySelector<HTMLButtonElement>('[data-looper-add-riff="t1"]')?.click();
    expect(sent).toContainEqual({ type: "addRiffAsLooperLayer", takeId: "t1" });
  });

  it("keeps Redo, Clear, the modes and Save in a More sheet at compact density", async () => {
    const { applyDensityPreference } = await import("../ts/compactMode.js");
    const sheet = () => strip.querySelector<HTMLElement>(".looper-more-sheet");
    const more = () => strip.querySelector<HTMLButtonElement>("[data-looper-more]");
    state({ state: "playing", playing: true, lengthSec: 8, layerCount: 1, canRedo: true }, { length: "bars", bars: 4 });
    expect(more()).toBeNull();

    applyDensityPreference("compact");
    expect(more()?.getAttribute("aria-label")).toBe("More looper controls");
    // Drawn once, in the sheet rather than the row.
    for (const selector of ['[data-looper-command="redo"]', '[data-looper-command="clear"]', ".looper-modes", "[data-looper-save]", "[data-looper-projects]"]) {
      expect(strip.querySelectorAll(selector)).toHaveLength(1);
      expect(sheet()?.querySelector(selector)).not.toBeNull();
    }

    more()?.click();
    expect(strip.classList.contains("is-more-open")).toBe(true);
    expect(more()?.getAttribute("aria-expanded")).toBe("true");

    // Using a control in the sheet leaves it open, through the redraw the engine's answer causes.
    strip.querySelector<HTMLButtonElement>('[data-looper-bars="1"]')?.click();
    expect(sent).toContainEqual({ type: "setLooper", bars: 5 });
    state({ state: "playing", playing: true, lengthSec: 8, layerCount: 1 }, { length: "bars", bars: 5 });
    expect(strip.classList.contains("is-more-open")).toBe(true);
    expect(more()?.getAttribute("aria-expanded")).toBe("true");

    // A tap elsewhere closes it, and so does Escape.
    document.body.click();
    expect(strip.classList.contains("is-more-open")).toBe(false);
    more()?.click();
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    expect(strip.classList.contains("is-more-open")).toBe(false);

    // Save closes it.
    more()?.click();
    strip.querySelector<HTMLButtonElement>("[data-looper-save]")?.click();
    expect(sent.some((m) => m.type === "saveLooperProject")).toBe(true);
    expect(strip.classList.contains("is-more-open")).toBe(false);

    // Leaving compact closes it and puts everything back on the row.
    more()?.click();
    applyDensityPreference("full");
    expect(strip.classList.contains("is-more-open")).toBe(false);
    expect(more()).toBeNull();
    expect(strip.querySelector('.looper-transport [data-looper-command="redo"]')).not.toBeNull();
    applyDensityPreference("auto");
  });

  it("explains a refused press once", () => {
    state({ notice: "noSpare" });
    state({ notice: "noSpare" });
    expect(vi.mocked(showNotification)).toHaveBeenCalledTimes(1);
  });

  it("switches the engine off and hides everything with the flag", () => {
    setFlag(false);
    state();
    expect(sent).toContainEqual({ type: "setLooper", enabled: false });
    expect(button.hidden).toBe(true);
    expect(strip.hidden).toBe(true);
  });
});
