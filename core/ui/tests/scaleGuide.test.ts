import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { ScaleGuideShape, ScaleGuideView } from "../ts/scaleGuide/types";

const { postMessage } = vi.hoisted(() => ({ postMessage: vi.fn() }));
const { settingsStore } = vi.hoisted(() => ({ settingsStore: new Map<string, unknown>() }));
vi.mock("../ts/bridge.js", () => ({ postMessage, setAppSetting: vi.fn() }));
vi.mock("../ts/appSettingsStore.js", () => ({
  getAppSetting: (key: string) => settingsStore.get(key) ?? null,
  updateAppSetting: (key: string, value: unknown) => settingsStore.set(key, value),
}));

import { boxBaseFret, renderChordBoxSvg } from "../ts/scaleGuide/chordBox";
import { dotLabel, neckDrawingWidth, noteMarkersSvg, noteX, renderNeckSvg } from "../ts/scaleGuide/neck";
import { onScaleGuideCatalog, onScaleGuideReading, onScaleGuideView, renderScaleGuidePanel, resetScaleGuideForTests } from "../ts/scaleGuide/panel";
import { normalizeProgressions, PROGRESSIONS_SETTING, romanToken } from "../ts/scaleGuide/progressions";
import { DEFAULT_SETTINGS, normalizeSettings, SCALE_GUIDE_SETTING } from "../ts/scaleGuide/state";

const JAM_PANEL = readFileSync(join(__dirname, "..", "ui-components", "panels", "jam-panel.html"), "utf8");

const OPEN_C: ScaleGuideShape = { frets: [0, 0, 0, 3], fingers: [0, 0, 0, 1], barres: [], position: 3, caged: "" };

function ukuleleView(overrides: Partial<ScaleGuideView> = {}): ScaleGuideView {
  return {
    mode: "chord",
    key: 0,
    instrument: "ukulele",
    tuning: { id: "uke", name: "Ukulele" },
    frets: 12,
    capo: 0,
    strings: [
      { openMidi: 67, name: "G" },
      { openMidi: 60, name: "C" },
      { openMidi: 64, name: "E" },
      { openMidi: 69, name: "A" },
    ],
    title: "C",
    position: 0,
    flats: false,
    tones: [
      { name: "C", interval: "R", degree: 1, pitchClass: 0, root: true },
      { name: "E", interval: "3", degree: 3, pitchClass: 4, root: false },
      { name: "G", interval: "5", degree: 5, pitchClass: 7, root: false },
    ],
    dots: [
      { string: 0, fret: 0, tone: 2, name: "G", interval: "5", degree: 5, root: false, inPosition: true },
      { string: 1, fret: 0, tone: 0, name: "C", interval: "R", degree: 1, root: true, inPosition: true },
      { string: 3, fret: 3, tone: 0, name: "C", interval: "R", degree: 1, root: true, inPosition: true },
      { string: 3, fret: 7, tone: 1, name: "E", interval: "3", degree: 3, root: false, inPosition: false },
    ],
    shapes: [OPEN_C],
    chord: { id: "maj", name: "C", quality: "Major", formula: "R 3 5" },
    ...overrides,
  };
}

describe("scale guide neck", () => {
  it("draws a dot per tone, the root marked and out-of-position ones dimmed", () => {
    const svg = renderNeckSvg(ukuleleView(), { labels: "name", leftHanded: false });
    expect(svg.match(/class="sg-dot[ "]/g)?.length).toBe(4);
    expect(svg.match(/sg-dot-root/g)?.length).toBe(2);
    expect(svg).toContain("sg-dot-dim");
    expect(svg.match(/class="sg-string[ "]/g)?.length).toBe(4);
  });

  it("labels dots by name, interval, degree or not at all", () => {
    const dot = ukuleleView().dots[3];
    expect(dotLabel(dot, "name")).toBe("E");
    expect(dotLabel(dot, "interval")).toBe("3");
    expect(dotLabel(dot, "degree")).toBe("3");
    expect(dotLabel(dot, "none")).toBe("");
  });

  it("puts the lowest string at the bottom and mirrors a left-handed neck", () => {
    const right = renderNeckSvg(ukuleleView(), { labels: "none", leftHanded: false });
    const left = renderNeckSvg(ukuleleView(), { labels: "none", leftHanded: true });
    const x = noteX(3, 12);
    expect(right).toContain(`cx="${Math.round(x * 10) / 10}"`);
    expect(left).toContain(`cx="${Math.round((1000 - x) * 10) / 10}"`);
  });

  it("spreads the frets to fill a wide space and keeps the dots their size", () => {
    const narrow = renderNeckSvg(ukuleleView(), { labels: "none", leftHanded: false });
    const wide = renderNeckSvg(ukuleleView(), { labels: "none", leftHanded: false, width: 2400 });
    const height = /viewBox="0 0 1000 ([0-9]+)"/.exec(narrow)?.[1];
    expect(wide).toContain(`viewBox="0 0 2400 ${height}"`);
    expect(wide).toContain('r="13"');
    expect(wide).toContain(`cx="${Math.round(noteX(3, 12, 2400) * 10) / 10}"`);
  });

  it("draws no narrower than it can hold, and mirrors at any width", () => {
    expect(neckDrawingWidth()).toBe(1000);
    expect(neckDrawingWidth(640)).toBe(1000);
    expect(neckDrawingWidth(1501.4)).toBe(1501);
    const left = renderNeckSvg(ukuleleView(), { labels: "none", leftHanded: true, width: 2400 });
    expect(left).toContain(`cx="${Math.round((2400 - noteX(3, 12, 2400)) * 10) / 10}"`);
  });

  it("puts LISTEN's rings on the same frets as the drawn neck", () => {
    const options = { labels: "none", leftHanded: false, width: 2400 } as const;
    const rings = noteMarkersSvg(ukuleleView(), options, ukuleleView().strings[0].openMidi + 3, "sg-miss");
    expect(rings).toContain(`cx="${Math.round(noteX(3, 12, 2400) * 10) / 10}"`);
  });

  it("marks a chord shape's muted strings and leaves their open dots out", () => {
    const shape: ScaleGuideShape = { frets: [-1, 0, 0, 3], fingers: [0, 0, 0, 1], barres: [], position: 3, caged: "" };
    const svg = renderNeckSvg(ukuleleView({ position: 1, shapes: [shape] }), { labels: "name", leftHanded: false });
    expect(svg).toContain("sg-mute");
    expect(svg.match(/class="sg-dot[ "]/g)?.length).toBe(3);
  });

  it("escapes text from the model", () => {
    const view = ukuleleView({ title: '"><img src=x>' });
    view.strings[0].name = "<b>";
    const svg = renderNeckSvg(view, { labels: "name", leftHanded: false });
    expect(svg).not.toContain("<b>");
    expect(svg).not.toContain("<img");
  });
});

describe("scale guide chord box", () => {
  it("starts at the nut when the shape fits, else at its lowest fret", () => {
    expect(boxBaseFret(OPEN_C, 0)).toBe(1);
    expect(boxBaseFret({ ...OPEN_C, frets: [7, 9, 9, 8] }, 0)).toBe(7);
    expect(boxBaseFret({ ...OPEN_C, frets: [2, 2, 2, 4] }, 2)).toBe(3);
  });

  it("draws open and muted strings, finger numbers, a barre and the starting fret", () => {
    const open = renderChordBoxSvg(OPEN_C, ukuleleView(), { leftHanded: false });
    expect(open.match(/sg-box-open/g)?.length).toBe(3);
    expect(open).toContain('class="sg-box-nut"');
    expect(open).toContain(">1</text>");

    const barre: ScaleGuideShape = {
      frets: [-1, 8, 10, 10, 10, 8],
      fingers: [0, 1, 3, 3, 3, 1],
      barres: [{ fret: 8, fromString: 1, toString: 5, finger: 1 }, { fret: 10, fromString: 2, toString: 4, finger: 3 }],
      position: 8,
      caged: "A",
    };
    const sixString = ukuleleView({
      strings: [40, 45, 50, 55, 59, 64].map((openMidi) => ({ openMidi, name: "" })),
    });
    const svg = renderChordBoxSvg(barre, sixString, { leftHanded: false });
    expect(svg.match(/sg-box-barre/g)?.length).toBe(2);
    expect(svg).toContain("sg-box-mute");
    expect(svg).toContain('class="sg-box-base"');
    expect(svg).toContain("A shape");
    expect(svg).not.toContain('class="sg-box-nut"');
  });
});

describe("scale guide settings", () => {
  it("falls back to the defaults for anything missing or malformed", () => {
    expect(normalizeSettings(null)).toEqual(DEFAULT_SETTINGS);
    expect(normalizeSettings({ key: 15, mode: "piano", scale: "<x>", capo: -3, labels: "interval", leftHanded: true })).toEqual({
      ...DEFAULT_SETTINGS,
      key: 11,
      capo: 0,
      labels: "interval",
      leftHanded: true,
    });
  });
});

describe("scale guide progressions", () => {
  it("writes chords as the roman numerals the engine reads", () => {
    expect(romanToken(0, "maj")).toBe("I");
    expect(romanToken(10, "maj")).toBe("bVII");
    expect(romanToken(2, "m7")).toBe("ii7");
    expect(romanToken(11, "m7b5")).toBe("viiø7");
    expect(romanToken(6, "dim")).toBe("bv°");
    expect(romanToken(0, "5")).toBe("I5");
    expect(romanToken(5, "mMaj7")).toBe("ivmaj7");
  });

  it("keeps only well-formed saved progressions", () => {
    expect(
      normalizeProgressions([
        { id: "p1", name: " Mine ", chords: ["I", "bVII", "<script>"] },
        { id: "p1", name: "Duplicate", chords: ["I"] },
        { id: "bad id", chords: ["I"] },
        { id: "p2", chords: [] },
        "junk",
      ]),
    ).toEqual([{ id: "p1", name: "Mine", chords: ["I", "bVII"] }]);
  });
});

describe("scale guide panel", () => {
  beforeEach(() => {
    resetScaleGuideForTests();
    postMessage.mockClear();
    settingsStore.clear();
    document.body.innerHTML = JAM_PANEL;
    document.getElementById("panel-jam")?.classList.add("active");
    document.getElementById("jam-section-panel-scales")?.removeAttribute("hidden");
  });

  function lastViewRequest(): { requestId: number; request: Record<string, unknown> } {
    const calls = postMessage.mock.calls.map(([message]) => message as Record<string, unknown>);
    const views = calls.filter((message) => message.type === "scaleGuide" && message.action === "view");
    return views[views.length - 1] as unknown as { requestId: number; request: Record<string, unknown> };
  }

  it("asks for the catalogue once and a view each time it is shown", () => {
    renderScaleGuidePanel();
    renderScaleGuidePanel();
    const catalogs = postMessage.mock.calls.filter(([message]) => (message as { action?: string }).action === "catalog");
    expect(catalogs).toHaveLength(1);
    expect(lastViewRequest().request).toMatchObject({ mode: "scale", key: 4, scale: "minpent", tuning: "std" });
    expect(document.querySelectorAll("#sg-key option")).toHaveLength(12);
    expect((document.getElementById("sg-key") as HTMLSelectElement).value).toBe("4");
  });

  it("draws only the latest answer", () => {
    renderScaleGuidePanel();
    const first = lastViewRequest().requestId;
    const key = document.getElementById("sg-key") as HTMLSelectElement;
    key.value = "9";
    key.dispatchEvent(new Event("change"));
    const second = lastViewRequest().requestId;
    expect(second).toBeGreaterThan(first);
    expect(lastViewRequest().request.key).toBe(9);

    onScaleGuideView({ type: "scaleGuideView", requestId: first, view: ukuleleView({ title: "Stale" }) });
    expect(document.getElementById("sg-neck")?.innerHTML).not.toContain("sg-neck-svg");

    onScaleGuideView({ type: "scaleGuideView", requestId: second, view: ukuleleView() });
    expect(document.getElementById("sg-neck")?.innerHTML).toContain("sg-neck-svg");
    expect(document.getElementById("sg-title")?.textContent).toBe("Ukulele");
    expect(document.querySelectorAll("#sg-boxes [data-sg-position]")).toHaveLength(1);
    expect(document.getElementById("sg-root-badge")?.textContent).toBe("C");
    expect(document.getElementById("sg-summary-label")?.textContent).toBe("Chord");
    expect(document.getElementById("sg-summary-name")?.textContent).toBe("C · Major");
    expect(document.querySelectorAll("#sg-info .sg-pill")).toHaveLength(3);
  });

  it("sums up a scale with its notes to the octave and its chords", () => {
    renderScaleGuidePanel();
    const scale = ukuleleView({
      mode: "scale",
      title: "C Major (Ionian)",
      shapes: undefined,
      chord: undefined,
      positions: [{ kind: "box", fretFrom: 7, fretTo: 10 }],
      scale: {
        id: "major",
        name: "Major (Ionian)",
        steps: "W W H W W W H",
        formula: "R 2 3 4 5 6 7",
        diatonic: [
          { name: "C", numeral: "I", quality: "maj", rootPitchClass: 0 },
          { name: "Dm", numeral: "ii", quality: "m", rootPitchClass: 2 },
        ],
      },
    });
    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view: scale });
    expect(document.getElementById("sg-summary-name")?.textContent).toBe("Major (Ionian)");
    expect(document.getElementById("sg-tones")?.textContent?.split(/\s+/)).toEqual(["C", "E", "G", "C"]);
    expect(document.querySelectorAll("#sg-info [data-sg-chord-root]")).toHaveLength(2);
    expect(document.querySelectorAll("#sg-positions [data-sg-position]")).toHaveLength(2);

    (document.querySelector('#sg-info [data-sg-chord-root="2"]') as HTMLElement).click();
    expect(lastViewRequest().request).toMatchObject({ mode: "chord", key: 2, chord: "m", position: 0 });
  });

  it("highlights a chosen position before the engine answers", () => {
    renderScaleGuidePanel();
    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view: ukuleleView() });
    (document.querySelector('#sg-positions [data-sg-position="1"]') as HTMLElement).click();
    expect(document.querySelector('#sg-positions [data-sg-position="1"]')?.classList.contains("active")).toBe(true);
    expect(document.querySelector('#sg-positions [data-sg-position="0"]')?.classList.contains("active")).toBe(false);
    expect(lastViewRequest().request.position).toBe(1);
  });

  it("saves a change, and a new mode starts on the whole neck", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, position: 3 });
    renderScaleGuidePanel();
    (document.querySelector('[data-sg-mode="chord"]') as HTMLElement).click();
    expect(settingsStore.get(SCALE_GUIDE_SETTING)).toMatchObject({ mode: "chord", position: 0 });
    expect(lastViewRequest().request).toMatchObject({ mode: "chord", position: 0 });
  });

  it("redraws labels without asking the engine again", () => {
    renderScaleGuidePanel();
    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view: ukuleleView() });
    const before = postMessage.mock.calls.length;
    (document.querySelector('[data-sg-labels="name"]') as HTMLElement).click();
    expect(postMessage.mock.calls.length).toBe(before);
  });

  function progressionView(step: number): ScaleGuideView {
    const f: ScaleGuideShape = { frets: [2, 0, 1, 0], fingers: [2, 0, 1, 0], barres: [], position: 1, caged: "" };
    return ukuleleView({
      position: 1,
      progression: {
        id: "mine",
        name: "Mine",
        step,
        next: step === 0 ? "F" : "C",
        chords: [
          { name: "C", numeral: "I", rootPitchClass: 0, shape: OPEN_C },
          { name: "F", numeral: "IV", rootPitchClass: 5, shape: f },
        ],
      },
    });
  }

  function choose(id: string, value: string): void {
    const select = document.getElementById(id) as HTMLSelectElement;
    select.value = value;
    select.dispatchEvent(new Event("change"));
  }

  const CATALOG = {
    type: "scaleGuideCatalog",
    keys: ["C", "C♯/D♭", "D", "D♯/E♭", "E", "F", "F♯/G♭", "G", "G♯/A♭", "A", "A♯/B♭", "B"],
    scales: [{ id: "minpent", name: "Minor Pentatonic", group: "Common", formula: "R ♭3 4 5 ♭7" }],
    chords: [
      { id: "maj", name: "Major", symbol: "" },
      { id: "m7", name: "Minor 7", symbol: "m7" },
    ],
    progressions: [{ id: "classicrock", name: "Classic rock", numerals: ["I", "♭VII", "IV", "I"] }],
    tunings: [{ id: "std", name: "Standard E", instrument: "guitar", strings: [40, 45, 50, 55, 59, 64], stringNames: [] }],
  };

  it("plays a built-in progression and steps it, the next chord led from the shape showing", () => {
    renderScaleGuidePanel();
    onScaleGuideCatalog(CATALOG);
    (document.querySelector('[data-sg-mode="chord"]') as HTMLElement).click();
    choose("sg-progression", "b:classicrock");
    expect(lastViewRequest().request).toMatchObject({ mode: "chord", progression: "classicrock", step: 0 });
    expect(document.getElementById("sg-chord-field")?.hidden).toBe(true);
    expect(document.getElementById("sg-key-label")?.textContent).toBe("Key");

    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view: progressionView(0) });
    expect(document.getElementById("sg-steps-row")?.hidden).toBe(false);
    expect(document.querySelectorAll("#sg-steps .sg-step")).toHaveLength(2);
    expect(document.querySelectorAll("#sg-boxes .sg-strip-box")).toHaveLength(2);

    (document.querySelector('[data-sg-step="next"]') as HTMLElement).click();
    expect(lastViewRequest().request).toMatchObject({ step: 1, previousFrets: OPEN_C.frets });

    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view: progressionView(1) });
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowRight" }));
    expect(lastViewRequest().request.step).toBe(0);

    // A box in the strip shows that chord in exactly the shape it draws.
    (document.querySelector('#sg-boxes [data-sg-goto="1"]') as HTMLElement).click();
    expect(lastViewRequest().request).toMatchObject({ step: 1, previousFrets: [2, 0, 1, 0] });
  });

  it("writes, saves and deletes a progression of the player's own", () => {
    renderScaleGuidePanel();
    onScaleGuideCatalog(CATALOG);
    (document.querySelector('[data-sg-mode="chord"]') as HTMLElement).click();
    choose("sg-progression", "+new");
    expect(document.getElementById("sg-editor")?.hidden).toBe(false);

    choose("sg-editor-degree", "0");
    (document.getElementById("sg-editor-add") as HTMLElement).click();
    choose("sg-editor-degree", "10");
    (document.getElementById("sg-editor-add") as HTMLElement).click();
    expect(lastViewRequest().request).toMatchObject({ progressionChords: ["I", "bVII"] });
    expect(document.querySelectorAll("#sg-editor-chords .sg-editor-pill")).toHaveLength(2);

    const name = document.getElementById("sg-editor-name") as HTMLInputElement;
    name.value = "Rock";
    name.dispatchEvent(new Event("input"));
    (document.getElementById("sg-editor-save") as HTMLElement).click();
    expect(settingsStore.get(PROGRESSIONS_SETTING)).toEqual([{ id: "p1", name: "Rock", chords: ["I", "bVII"] }]);
    expect(settingsStore.get(SCALE_GUIDE_SETTING)).toMatchObject({ progression: "u:p1" });
    expect(lastViewRequest().request).toMatchObject({ progressionChords: ["I", "bVII"], progressionName: "Rock" });
    expect(document.getElementById("sg-editor")?.hidden).toBe(true);

    (document.getElementById("sg-progression-edit") as HTMLElement).click();
    (document.getElementById("sg-editor-delete") as HTMLElement).click();
    expect(settingsStore.get(PROGRESSIONS_SETTING)).toEqual([]);
    expect(settingsStore.get(SCALE_GUIDE_SETTING)).toMatchObject({ progression: "" });
  });

  it("edits a copy of a built-in progression, never the built-in", () => {
    renderScaleGuidePanel();
    onScaleGuideCatalog(CATALOG);
    (document.querySelector('[data-sg-mode="chord"]') as HTMLElement).click();
    choose("sg-progression", "b:classicrock");
    (document.getElementById("sg-progression-edit") as HTMLElement).click();
    expect((document.getElementById("sg-editor-name") as HTMLInputElement).value).toBe("Copy of Classic rock");
    expect(lastViewRequest().request).toMatchObject({ progressionChords: ["I", "bVII", "IV", "I"] });
    expect(document.getElementById("sg-editor-delete")?.hidden).toBe(true);
  });

  it("listens whenever the guide is showing, with chroma in Chords mode, and has no switch for it", () => {
    const listens = (): Array<Record<string, unknown>> =>
      postMessage.mock.calls.map(([m]) => m as Record<string, unknown>).filter((m) => m.type === "tuner" && m.action === "listen");
    renderScaleGuidePanel();
    expect(listens().at(-1)).toMatchObject({ enabled: true, chroma: false });
    expect(document.getElementById("sg-listen")).toBeNull();
    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view: ukuleleView({ mode: "scale", shapes: undefined }) });

    // Nothing heard yet: no readout saying it is listening.
    expect(document.getElementById("sg-live")?.hidden).toBe(true);

    // The played note (G4, the ukulele's open high G) lights where it sits once it reads twice.
    const reading = { type: "tunerUpdate", detected: true, noteName: "G", octave: 4, levelDb: -20 };
    expect(onScaleGuideReading(reading)).toBe(true);
    onScaleGuideReading(reading);
    expect(document.querySelectorAll("#sg-neck .sg-dot.sg-hit").length).toBeGreaterThan(0);
    expect(document.getElementById("sg-live")?.textContent).toContain("of the scale");

    // A note with no dot (A4, not in C E G) gets a ring instead.
    const offScale = { type: "tunerUpdate", detected: true, noteName: "A", octave: 4, levelDb: -20 };
    onScaleGuideReading(offScale);
    onScaleGuideReading(offScale);
    expect(document.querySelectorAll("#sg-neck .sg-live-layer .sg-miss").length).toBeGreaterThan(0);
    expect(document.getElementById("sg-live")?.textContent).toContain("outside the");

    (document.querySelector('[data-sg-mode="chord"]') as HTMLElement).click();
    expect(listens().at(-1)).toMatchObject({ enabled: true, chroma: true });

    // Hidden (another Jam section): the next reading stops the listening.
    document.getElementById("jam-section-panel-scales")?.setAttribute("hidden", "");
    onScaleGuideReading(reading);
    expect(listens().at(-1)).toMatchObject({ enabled: false });
  });

  it("steps a progression on when its chord rings clean", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "chord", progression: "b:pop", position: 1 });
    renderScaleGuidePanel();
    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view: progressionView(0) });
    expect(document.getElementById("sg-match")?.hidden).toBe(false);
    const clean = { type: "tunerUpdate", detected: false, levelDb: -18, chroma: [0.4, 0, 0, 0, 0.3, 0, 0, 0.28, 0, 0, 0, 0.02] };
    const before = lastViewRequest().requestId;

    for (let i = 0; i < 5; i += 1) {
      onScaleGuideReading({ ...clean, levelDb: i === 0 ? -18 : -19 });
    }

    expect(lastViewRequest().requestId).toBeGreaterThan(before);
    expect(lastViewRequest().request).toMatchObject({ step: 1, previousFrets: OPEN_C.frets });
    expect(document.getElementById("sg-live")?.textContent).toContain("Clean");
  });

  it("fills the pickers from the catalogue", () => {
    renderScaleGuidePanel();
    onScaleGuideCatalog({
      type: "scaleGuideCatalog",
      keys: ["C", "C♯/D♭", "D", "D♯/E♭", "E", "F", "F♯/G♭", "G", "G♯/A♭", "A", "A♯/B♭", "B"],
      scales: [{ id: "minpent", name: "Minor Pentatonic", group: "Common", formula: "R ♭3 4 5 ♭7" }],
      chords: [{ id: "maj", name: "Major", symbol: "" }],
      progressions: [],
      tunings: [{ id: "std", name: "Standard E", instrument: "guitar", strings: [40, 45, 50, 55, 59, 64], stringNames: [] }],
    });
    expect((document.getElementById("sg-scale") as HTMLSelectElement).value).toBe("minpent");
    expect(document.querySelector("#sg-tuning optgroup")?.getAttribute("label")).toBe("Guitar");
    expect((document.getElementById("sg-capo") as HTMLSelectElement).options).toHaveLength(13);
  });
});
