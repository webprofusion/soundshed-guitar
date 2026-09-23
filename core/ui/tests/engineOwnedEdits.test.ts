/**
 * Edits the engine now makes itself (docs/plans/native-ui.md, "Engine-owned edits"): the web
 * UI names the change and keeps only what it needs to draw at once. These check what goes
 * over the bridge — the real senders, captured at window.IPlugSendMsg — and that nothing
 * sends a whole document back for a one-entry change.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/toneSharingPanel.js", () => ({
  syncToneSharingFavoriteForPreset: () => Promise.resolve(),
  syncToneSharingRatingForPreset: () => Promise.resolve(),
}));
vi.mock("../ts/presets/library.js", () => ({ renderPresetUI: vi.fn() }));
vi.mock("../ts/presets/filter.js", () => ({ filterPresets: vi.fn() }));

const sent: Array<Record<string, unknown>> = [];
window.IPlugSendMsg = (payload: unknown) => {
  sent.push(JSON.parse(String(payload)) as Record<string, unknown>);
};

// controls.ts finds the mute button when it loads.
document.body.innerHTML = `<button id="output-mute-toggle"></button><div id="footer-demo-audio-container"></div>`;

const { uiState } = await import("../ts/state.js");
const favorites = await import("../ts/presets/favorites.js");
const { applyPresetRecentsFromBackend } = await import("../ts/presets/recents.js");
const { getCurrentUiSettings } = await import("../ts/windowSettings.js");
const demo = await import("../ts/demoAudio.js");
const controls = await import("../ts/controls.js");

function ofType(type: string): Array<Record<string, unknown>> {
  return sent.filter((message) => message.type === type);
}

beforeEach(() => {
  sent.length = 0;
});

describe("preset favourites and ratings", () => {
  beforeEach(() => {
    uiState.presetFavorites = new Set(["p1"]);
    uiState.presetRatings = { p1: 3 };
  });

  it("toggles one preset's favourite, not the whole list", () => {
    favorites.toggleFavoritePreset("p2");
    favorites.toggleFavoritePreset("p1");

    expect(ofType("setPresetFavorite")).toEqual([
      { type: "setPresetFavorite", presetId: "p2", favorite: true },
      { type: "setPresetFavorite", presetId: "p1", favorite: false },
    ]);
    expect(ofType("setPresetFavorites")).toHaveLength(0);
    expect([...uiState.presetFavorites]).toEqual(["p2"]);
  });

  it("rates one preset, and clears a rating with 0", () => {
    favorites.setPresetRating("p2", 5);
    favorites.setPresetRating("p1", null);

    expect(ofType("setPresetRating")).toEqual([
      { type: "setPresetRating", presetId: "p2", rating: 5 },
      { type: "setPresetRating", presetId: "p1", rating: 0 },
    ]);
    expect(ofType("setPresetRatings")).toHaveLength(0);
    expect(uiState.presetRatings).toEqual({ p2: 5 });
  });
});

describe("recently played", () => {
  it("takes the engine's list into the UI settings and the blob sent back whole, sending nothing", () => {
    uiState.uiSettings = { zoom: 1.25, presetRecents: ["old"] };

    applyPresetRecentsFromBackend(["r2", "r1", "r2", " ", 7]);

    expect(uiState.uiSettings).toEqual({ zoom: 1.25, presetRecents: ["r2", "r1"] });
    expect(getCurrentUiSettings().presetRecents).toEqual(["r2", "r1"]);
    expect(sent).toHaveLength(0);
  });
});

describe("demo clips", () => {
  beforeEach(() => {
    uiState.demoAudioSelectedId = null;
    uiState.demoAudioRepeat = false;
  });

  it("draws the footer controls once the engine's list arrives", () => {
    demo.applyDemoClips([{ id: "riff-1", title: "Riff 1" }, { id: "" }, { title: "no id" }, { id: "sweep" }]);

    const options = [...document.querySelectorAll<HTMLOptionElement>("#footer-demo-audio-select option")];
    expect(options.map((option) => `${option.value}:${option.textContent}`)).toEqual(["riff-1:Riff 1", "sweep:sweep"]);
  });

  it("plays a clip by id, the engine doing the repeat", async () => {
    demo.applyDemoClips([{ id: "riff-1", title: "Riff 1" }, { id: "sweep", title: "Sweep" }]);
    uiState.demoAudioSelectedId = "sweep";
    uiState.demoAudioRepeat = true;

    await demo.previewSelectedDemoAudio();

    expect(ofType("previewDemoAudio")).toEqual([{ type: "previewDemoAudio", clipId: "sweep", repeat: true }]);
  });

  it("renders a clip by id rather than sending its audio", async () => {
    demo.applyDemoClips([{ id: "riff-1", title: "Riff 1" }]);

    await demo.renderSelectedDemoAudio();

    const [render] = ofType("renderDemoAudio");
    expect(render).toMatchObject({ clipId: "riff-1", title: "Riff 1" });
    expect(render).not.toHaveProperty("audio");
  });
});

describe("output mute", () => {
  it("asks the engine to mute rather than zeroing the master gain, and shows what the engine says", () => {
    const button = document.getElementById("output-mute-toggle") as HTMLButtonElement;
    controls.initializeControls();
    controls.applyOutputMuted(true);
    expect(button.getAttribute("aria-pressed")).toBe("true");

    button.click();

    expect(ofType("setOutputMuted")).toEqual([{ type: "setOutputMuted", muted: false }]);
    expect(ofType("setMasterGain")).toHaveLength(0);
    expect(button.classList.contains("muted")).toBe(false);
  });
});
