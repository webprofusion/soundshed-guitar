import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GuideRun, ProgressionRun, ScaleGuideDot, ScaleGuideSettings, ScaleGuideView } from "../ts/scaleGuide/types";

const { postMessage } = vi.hoisted(() => ({ postMessage: vi.fn() }));
const { settingsStore } = vi.hoisted(() => ({ settingsStore: new Map<string, unknown>() }));
vi.mock("../ts/bridge.js", () => ({ postMessage, setAppSetting: vi.fn() }));
vi.mock("../ts/appSettingsStore.js", () => ({
  getAppSetting: (key: string) => settingsStore.get(key) ?? null,
  updateAppSetting: (key: string, value: unknown) => settingsStore.set(key, value),
}));

import { renderRunTabSvg } from "../ts/scaleGuide/runTab";
import { effectiveLabels } from "../ts/scaleGuide/arpeggioView";
import { dotLabel, renderNeckSvg } from "../ts/scaleGuide/neck";
import { onScaleGuideReading, onScaleGuideView, renderScaleGuidePanel, resetScaleGuideForTests } from "../ts/scaleGuide/panel";
import { onScaleGuidePlayback, onScaleGuidePlaybackEnded, onScaleGuidePlaybackStarted } from "../ts/scaleGuide/playback";
import { DEFAULT_SETTINGS, normalizeSettings, SCALE_GUIDE_SETTING, viewRequest } from "../ts/scaleGuide/state";

const JAM_PANEL = readFileSync(join(__dirname, "..", "ui-components", "panels", "jam-panel.html"), "utf8");

/** A minor's three-string sweep: 14/13/12 17 on the G, B and high E strings. */
const RUN: GuideRun = {
  name: "Root position",
  start: "R",
  fretFrom: 12,
  fretTo: 17,
  fromString: 3,
  toString: 5,
  notes: [
    { string: 3, fret: 14, tone: 0, name: "A", interval: "R", root: true, finger: 3 },
    { string: 4, fret: 13, tone: 1, name: "C", interval: "♭3", root: false, finger: 2 },
    { string: 5, fret: 12, tone: 2, name: "E", interval: "5", root: false, finger: 1 },
    { string: 5, fret: 17, tone: 0, name: "A", interval: "R", root: true, finger: 4 },
  ],
  steps: [
    { note: 0, stroke: "down" },
    { note: 1, stroke: "down" },
    { note: 2, stroke: "down" },
    { note: 3, stroke: "hammer" },
    { note: 2, stroke: "pull" },
    { note: 1, stroke: "up" },
    { note: 0, stroke: "up" },
  ],
};

function dot(string: number, fret: number, overrides: Partial<ScaleGuideDot> = {}): ScaleGuideDot {
  return { string, fret, tone: 0, name: "A", interval: "R", degree: 1, root: true, inPosition: true, ...overrides };
}

function arpeggioView(overrides: Partial<ScaleGuideView> = {}): ScaleGuideView {
  return {
    mode: "arpeggio",
    key: 9,
    instrument: "guitar",
    tuning: { id: "std", name: "Standard E" },
    frets: 24,
    capo: 0,
    strings: ["E", "A", "D", "G", "B", "E"].map((name, index) => ({ openMidi: 40 + index * 5, name })),
    title: "Am arpeggio",
    position: 1,
    flats: false,
    tones: [
      { name: "A", interval: "R", degree: 1, pitchClass: 9, root: true },
      { name: "C", interval: "♭3", degree: 3, pitchClass: 0, root: false },
      { name: "E", interval: "5", degree: 5, pitchClass: 4, root: false },
    ],
    dots: [dot(3, 14, { order: 1, finger: 3 }), dot(0, 5, { inPosition: false })],
    chord: { id: "m", name: "Am", quality: "Minor", formula: "R ♭3 5" },
    arpeggio: { style: "sweep", strings: 3, stringChoices: [3, 4, 5, 6], advice: "One pick stroke.", empty: "No playable run.", runs: [RUN] },
    run: RUN,
    ...overrides,
  };
}

/** Am then G, three strings each: G's shape two frets down, slid into along the G string. */
const WHOLE: ProgressionRun = {
  name: "Mine",
  slides: true,
  fretFrom: 10,
  fretTo: 17,
  notes: [
    ...RUN.notes,
    { string: 3, fret: 12, tone: 0, name: "G", interval: "R", root: true, finger: 3 },
    { string: 4, fret: 12, tone: 1, name: "B", interval: "3", root: false, finger: 2 },
    { string: 5, fret: 10, tone: 2, name: "D", interval: "5", root: false, finger: 1 },
  ],
  steps: [
    ...RUN.steps,
    { note: 4, stroke: "slide" },
    { note: 5, stroke: "down" },
    { note: 6, stroke: "down" },
    { note: 5, stroke: "up" },
    { note: 4, stroke: "up" },
  ],
  bars: [
    { chord: 0, name: "Am", numeral: "i", from: 0, count: 7, run: "Root position", fretFrom: 12, fretTo: 17 },
    { chord: 1, name: "G", numeral: "♭VII", from: 7, count: 5, run: "Root position", fretFrom: 10, fretTo: 12 },
  ],
};

function progressionView(step = 0, overrides: Partial<ScaleGuideView> = {}): ScaleGuideView {
  return arpeggioView({
    title: step === 0 ? "Am arpeggio" : "G arpeggio",
    progression: {
      id: "custom",
      name: "Mine",
      step,
      next: step === 0 ? "G" : "Am",
      chords: [
        { name: "Am", numeral: "i", rootPitchClass: 9, shape: null, position: 1 },
        { name: "G", numeral: "♭VII", rootPitchClass: 7, shape: null, position: 2 },
      ],
      anchorStep: 0,
      anchorPosition: 1,
    },
    position: step === 0 ? 1 : 2,
    progressionRun: WHOLE,
    ...overrides,
  });
}

describe("arpeggio tab", () => {
  const svg = renderRunTabSvg(RUN, arpeggioView(), "Am arpeggio, Root position");

  it("writes every step out in order, the frets up and back", () => {
    const frets = [...svg.matchAll(/class="sg-tab-fret[^"]*"[^>]*>(\d+)</g)].map((match) => Number(match[1]));
    expect(frets).toEqual([14, 13, 12, 17, 12, 13, 14]);
  });

  it("marks the pick strokes and legato", () => {
    expect(svg.match(/class="sg-tab-stroke"/g)).toHaveLength(5);
    expect(svg.match(/class="sg-tab-legato"[^>]*>h</g)).toHaveLength(1);
    expect(svg.match(/class="sg-tab-legato"[^>]*>p</g)).toHaveLength(1);
  });

  it("puts the highest string on top and marks the roots", () => {
    const y = (fret: number): number => Number(new RegExp(`y="([\\d.]+)"[^>]*>${fret}<`).exec(svg)?.[1]);
    expect(y(12)).toBeLessThan(y(13));
    expect(y(13)).toBeLessThan(y(14));
    expect(svg).toContain('class="sg-tab-fret sg-tab-root"');
  });

  it("names the run for a screen reader", () => {
    expect(svg).toContain('aria-label="Am arpeggio, Root position: A fret 14 Down-stroke');
  });
});

describe("progression tab", () => {
  const flowed = (width: number, active = 0): string => renderRunTabSvg(WHOLE, progressionView(), "Mine", { bars: WHOLE.bars, width, active });
  const wide = flowed(900);
  const frets = (svg: string): number[] => [...svg.matchAll(/class="sg-tab-fret[^"]*"[^>]*>(\d+)</g)].map((match) => Number(match[1]));
  const count = (svg: string, pattern: RegExp): number => svg.match(pattern)?.length ?? 0;
  const stringNames = (svg: string): number => count(svg, /class="sg-tab-name"/g);

  it("is one diagram: every step in order, the string names once", () => {
    expect(count(wide, /<svg/g)).toBe(1);
    expect(frets(wide)).toEqual([14, 13, 12, 17, 12, 13, 14, 12, 12, 10, 12, 12]);
    expect(stringNames(wide)).toBe(6);
    expect(wide).toContain('data-sg-tab-step="11"');
  });

  it("ends each chord with a bar line, the last one heavier, and names each chord", () => {
    expect(count(wide, /class="sg-tab-barline[ "]/g)).toBe(2);
    expect(count(wide, /sg-tab-barline-end/g)).toBe(1);
    expect([...wide.matchAll(/class="sg-tab-chord"[^>]*>([^<]+)</g)].map((match) => match[1])).toEqual(["Am", "G"]);
  });

  it("makes each chord's bar a group to click, the chord showing marked", () => {
    expect([...wide.matchAll(/<g class="(sg-tab-bar[^"]*)" data-sg-goto="(\d)"/g)].map((match) => `${match[2]}:${match[1]}`)).toEqual(["0:sg-tab-bar active", "1:sg-tab-bar"]);
    expect(flowed(900, 1)).toContain('<g class="sg-tab-bar active" data-sg-goto="1"');
  });

  it("runs on to a new line where the width is used up, bars kept whole", () => {
    // 30 + 7 notes of 30 + a bar line fits 300; the second bar's five do not fit beside it.
    const narrow = flowed(300);
    expect(stringNames(narrow)).toBe(12);
    expect(frets(narrow)).toEqual(frets(wide));
    expect(count(narrow, /<g class="sg-tab-bar/g)).toBe(2);
    const height = (svg: string): number => Number(/height="(\d+)"/.exec(svg)?.[1]);
    expect(height(narrow)).toBeGreaterThan(1.9 * height(wide));
  });

  it("splits a bar too long for any line, naming the chord once", () => {
    const tiny = flowed(100);
    expect(frets(tiny)).toEqual(frets(wide));
    expect(count(tiny, /<g class="sg-tab-bar/g)).toBeGreaterThan(2);
    expect(count(tiny, /class="sg-tab-chord"/g)).toBe(2);
    expect(count(tiny, /class="sg-tab-barline[ "]/g)).toBe(2);
  });

  it("draws a slide as its two halves, either side of the bar line", () => {
    expect(count(wide, /class="sg-tab-slide"/g)).toBe(2);
    expect(wide).toMatch(/class="sg-tab-legato"[^>]*>sl\.</);
    expect(wide).toContain("G fret 12 Slide");
  });

  it("slants the line the way the fret goes: down from 14 to 12", () => {
    const [, y1, y2] = /class="sg-tab-slide" x1="[\d.]+" y1="([\d.]+)" x2="[\d.]+" y2="([\d.]+)"/.exec(wide) ?? [];
    expect(Number(y2)).toBeGreaterThan(Number(y1));
  });

  it("keeps a short bar for a chord with no run", () => {
    const bars = [WHOLE.bars[0], { chord: 1, name: "G", numeral: "♭VII", from: 7, count: 0 }];
    const svg = renderRunTabSvg({ notes: WHOLE.notes, steps: WHOLE.steps.slice(0, 7) }, progressionView(), "Mine", { bars, width: 900, active: 0 });
    expect(svg).toContain(">no run<");
    expect(count(svg, /<g class="sg-tab-bar/g)).toBe(2);
  });
});

describe("arpeggio neck", () => {
  it("draws the run's path through its notes", () => {
    const svg = renderNeckSvg(arpeggioView(), { labels: "name", leftHanded: false });
    expect(svg.match(/<polyline class="sg-run-path" points="([^"]+)"/)?.[1].split(" ")).toHaveLength(4);
  });

  it("has no path over the whole neck", () => {
    expect(renderNeckSvg(arpeggioView({ position: 0 }), { labels: "name", leftHanded: false })).not.toContain("sg-run-path");
  });

  it("labels the run by order and finger, leaving the rest of the neck blank", () => {
    expect(dotLabel(dot(3, 14, { order: 1, finger: 3 }), "order")).toBe("1");
    expect(dotLabel(dot(3, 14, { order: 1, finger: 3 }), "finger")).toBe("3");
    expect(dotLabel(dot(5, 0, { order: 2, finger: 0 }), "finger")).toBe("0");
    expect(dotLabel(dot(0, 5), "order")).toBe("");
  });

  it("names the notes when no run is chosen to number", () => {
    const svg = renderNeckSvg(arpeggioView({ position: 0, dots: [dot(0, 5, { name: "A" })] }), { labels: "order", leftHanded: false });
    expect(svg).toMatch(/>A<\/text><\/g>/);
  });
});

describe("arpeggio settings", () => {
  it("keeps good choices and fixes bad ones", () => {
    expect(normalizeSettings({ mode: "arpeggio", labels: "order", arpeggioStyle: "two", arpeggioStrings: 6 })).toMatchObject({
      mode: "arpeggio",
      labels: "order",
      arpeggioStyle: "two",
      arpeggioStrings: 6,
    });
    expect(normalizeSettings({ arpeggioStyle: "nonsense", arpeggioStrings: 99 })).toMatchObject({ arpeggioStyle: "sweep", arpeggioStrings: 8 });
    expect(normalizeSettings({})).toMatchObject({ arpeggioStyle: "sweep", arpeggioStrings: 0, arpeggioSlides: false });
    expect(normalizeSettings({ arpeggioSlides: true }).arpeggioSlides).toBe(true);
    expect(normalizeSettings({ arpeggioSlides: "yes" }).arpeggioSlides).toBe(false);
  });

  it("asks the engine for them", () => {
    const settings: ScaleGuideSettings = { ...DEFAULT_SETTINGS, mode: "arpeggio", arpeggioStyle: "box", arpeggioStrings: 4 };
    expect(viewRequest(settings)).toMatchObject({ mode: "arpeggio", arpeggioStyle: "box", arpeggioStrings: 4, arpeggioSlides: false });
  });

  it("shows notes in place of a run's labels on the other tabs", () => {
    expect(effectiveLabels({ ...DEFAULT_SETTINGS, labels: "finger", mode: "scale" })).toBe("name");
    expect(effectiveLabels({ ...DEFAULT_SETTINGS, labels: "finger", mode: "arpeggio" })).toBe("finger");
    expect(effectiveLabels({ ...DEFAULT_SETTINGS, labels: "degree", mode: "chord" })).toBe("degree");
  });
});

describe("arpeggios panel", () => {
  beforeEach(() => {
    postMessage.mockClear();
    settingsStore.clear();
    resetScaleGuideForTests();
    document.body.innerHTML = JAM_PANEL;
    document.getElementById("jam-section-panel-scales")?.removeAttribute("hidden");
    document.getElementById("panel-jam")?.classList.add("active");
  });

  function lastViewRequest(): { requestId: number; request: Record<string, unknown> } {
    const call = postMessage.mock.calls.filter(([message]) => message.type === "scaleGuide" && message.requestId !== undefined && message.action !== "catalog").at(-1);
    return call?.[0];
  }

  function answer(view: ScaleGuideView): void {
    onScaleGuideView({ type: "scaleGuideView", requestId: lastViewRequest().requestId, view });
  }

  function hidden(id: string): boolean {
    return document.getElementById(id)?.hasAttribute("hidden") ?? true;
  }

  function click(selector: string): void {
    (document.querySelector(selector) as HTMLElement).click();
  }

  it("opens on the first run, with the chord picker and no scale picker", () => {
    renderScaleGuidePanel();
    click('[data-sg-mode="arpeggio"]');
    expect(settingsStore.get(SCALE_GUIDE_SETTING)).toMatchObject({ mode: "arpeggio", position: 1 });
    expect(lastViewRequest().request).toMatchObject({ mode: "arpeggio", position: 1, arpeggioStyle: "sweep" });
    answer(arpeggioView());
    expect(hidden("sg-arp-row")).toBe(false);
    expect(hidden("sg-chord-field")).toBe(false);
    expect(hidden("sg-scale-field")).toBe(true);
    expect(hidden("sg-progression-field")).toBe(false);
    expect(document.getElementById("sg-key-label")?.textContent).toBe("Root");
  });

  describe("with a progression", () => {
    beforeEach(() => {
      settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", progression: "b:pop", position: 1 });
      renderScaleGuidePanel();
    });

    it("asks for the progression in its key, in place of one chord", () => {
      expect(lastViewRequest().request).toMatchObject({ mode: "arpeggio", progression: "pop", step: 0, anchorStep: 0, position: 1 });
      answer(progressionView());
      expect(hidden("sg-chord-field")).toBe(true);
      expect(hidden("sg-progression-field")).toBe(false);
      expect(hidden("sg-steps-row")).toBe(false);
      expect(document.getElementById("sg-key-label")?.textContent).toBe("Key");
      expect(document.querySelectorAll("#sg-steps .sg-step")).toHaveLength(2);
      expect(hidden("sg-boxes")).toBe(true);
    });

    it("writes the whole progression out as one diagram, a bar for each chord, the one showing marked", () => {
      answer(progressionView());
      expect(document.querySelectorAll("#sg-arp-tab svg.sg-tab-svg")).toHaveLength(1);
      const bars = [...document.querySelectorAll<SVGGElement>("#sg-arp-tab .sg-tab-bar")];
      expect(bars.map((bar) => bar.querySelector(".sg-tab-chord")?.childNodes[0]?.textContent)).toEqual(["Am", "G"]);
      expect(bars.map((bar) => bar.classList.contains("active"))).toEqual([true, false]);
      expect(bars[1].querySelector("title")?.textContent).toContain("Root position, frets 10–12");
      expect(document.querySelectorAll("#sg-arp-tab [data-sg-tab-step]")).toHaveLength(12);
      expect(document.getElementById("sg-arp-tab")?.textContent).toContain("Frets 10–17");
    });

    it("keeps the tab, and Play, while the whole neck shows", () => {
      answer(progressionView(0, { position: 0, run: undefined }));
      expect(hidden("sg-arp-tab")).toBe(false);
      expect((document.getElementById("sg-play") as HTMLButtonElement).disabled).toBe(false);
    });

    function clickBar(chord: number): void {
      document.querySelector(`#sg-arp-tab .sg-tab-bar[data-sg-goto="${chord}"] .sg-tab-fret`)?.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    }

    it("steps to a bar's chord and plays that bar when it is clicked, leaving the chosen run where it was", () => {
      answer(progressionView());
      clickBar(1);
      expect(lastViewRequest().request).toMatchObject({ step: 1, anchorStep: 0, position: 1 });
      const played = postMessage.mock.calls.map(([message]) => message).filter((message) => message.action === "play");
      expect(played).toHaveLength(1);
      expect(played[0]).toMatchObject({ type: "scaleGuide", bar: 1, request: { mode: "arpeggio", progression: "pop", step: 1, anchorStep: 0 } });
      answer(progressionView(1));
      expect(document.querySelector('#sg-arp-tab .sg-tab-bar[data-sg-goto="1"]')?.classList.contains("active")).toBe(true);
      expect(document.querySelector('#sg-positions [data-sg-position="2"]')).toBeNull();
      // Two chords: stepping on from the second comes round to the first.
      click('[data-sg-step="next"]');
      expect(lastViewRequest().request).toMatchObject({ step: 0, anchorStep: 0, position: 1 });
    });

    it("makes the chord showing the anchor when a run is chosen for it", () => {
      answer(progressionView());
      clickBar(1);
      answer(progressionView(1, { position: 1 }));
      click('#sg-positions [data-sg-position="1"]');
      expect(lastViewRequest().request).toMatchObject({ step: 1, anchorStep: 1, position: 1 });
    });

    it("turns slides on and off from the tab's heading", () => {
      answer(progressionView(0, { progressionRun: { ...WHOLE, slides: false } }));
      expect(document.querySelector("[data-sg-arp-slides]")?.getAttribute("aria-pressed")).toBe("false");
      click("[data-sg-arp-slides]");
      expect(settingsStore.get(SCALE_GUIDE_SETTING)).toMatchObject({ arpeggioSlides: true, position: 1 });
      expect(lastViewRequest().request).toMatchObject({ arpeggioSlides: true, progression: "pop" });
      answer(progressionView());
      expect(document.querySelector("[data-sg-arp-slides]")?.getAttribute("aria-pressed")).toBe("true");
    });

    it("follows Play through the tab and on to each chord", () => {
      vi.useFakeTimers();
      answer(progressionView());
      onScaleGuidePlaybackStarted();
      onScaleGuidePlayback({
        type: "scaleGuidePlayback",
        notes: [
          { string: 3, fret: 14, startMs: 0, lengthMs: 200, step: 0, chord: 0 },
          { string: 3, fret: 12, startMs: 1000, lengthMs: 200, step: 7, chord: 1 },
        ],
      });
      vi.advanceTimersByTime(10);
      expect(document.querySelector('#sg-arp-tab [data-sg-tab-step="0"]')?.classList.contains("sg-tab-playing")).toBe(true);
      const before = lastViewRequest().requestId;
      vi.advanceTimersByTime(950);
      expect(lastViewRequest().requestId).toBeGreaterThan(before);
      expect(lastViewRequest().request).toMatchObject({ step: 1, anchorStep: 0 });
      answer(progressionView(1));
      vi.advanceTimersByTime(100);
      expect(document.querySelector('#sg-arp-tab [data-sg-tab-step="7"]')?.classList.contains("sg-tab-playing")).toBe(true);
      expect(document.querySelector('#sg-arp-tab [data-sg-tab-step="0"]')?.classList.contains("sg-tab-playing")).toBe(false);
      onScaleGuidePlaybackEnded();
      expect(document.querySelector("#sg-arp-tab .sg-tab-playing")).toBeNull();
      vi.useRealTimers();
    });

    it("waits with Play for the metronome's bar line", () => {
      vi.useFakeTimers();
      answer(progressionView());
      onScaleGuidePlaybackStarted();
      onScaleGuidePlayback({ type: "scaleGuidePlayback", delayMs: 800, notes: [{ string: 3, fret: 14, startMs: 0, lengthMs: 200, step: 0, chord: 0 }] });
      const first = (): boolean => document.querySelector('#sg-arp-tab [data-sg-tab-step="0"]')?.classList.contains("sg-tab-playing") ?? false;
      vi.advanceTimersByTime(700);
      expect(first()).toBe(false);
      vi.advanceTimersByTime(150);
      expect(first()).toBe(true);
      onScaleGuidePlaybackEnded();
      vi.useRealTimers();
    });
  });

  it("shows the run's tab, position hints, formula and how to play it", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    expect(hidden("sg-arp-tab")).toBe(false);
    expect(document.querySelector("#sg-arp-tab svg.sg-tab-svg")).not.toBeNull();
    expect(document.getElementById("sg-arp-tab")?.textContent).toContain("Frets 12–17");
    expect(document.querySelector('#sg-positions [data-sg-position="1"]')?.textContent).toBe("1R");
    expect(document.getElementById("sg-summary-label")?.textContent).toBe("Arpeggio");
    expect(document.getElementById("sg-summary-name")?.textContent).toBe("Am · Minor");
    expect(document.getElementById("sg-reference")?.textContent).toBe("One pick stroke.");
    expect(document.querySelectorAll("#sg-info .sg-pill")).toHaveLength(3);
  });

  it("writes a scale position out as tab too, and has none for the whole neck", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "scale", position: 2 });
    renderScaleGuidePanel();
    answer(arpeggioView({ mode: "scale", position: 2, run: { ...RUN, name: "Position 2" } }));
    expect(hidden("sg-arp-tab")).toBe(false);
    expect(document.getElementById("sg-arp-tab")?.textContent).toContain("Position 2");
    answer(arpeggioView({ mode: "scale", position: 0, run: undefined }));
    expect(hidden("sg-arp-tab")).toBe(true);
  });

  it("offers Play for a chosen position only", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView({ position: 0, run: undefined }));
    expect((document.getElementById("sg-play") as HTMLButtonElement).disabled).toBe(true);
    answer(arpeggioView());
    expect((document.getElementById("sg-play") as HTMLButtonElement).disabled).toBe(false);
  });

  it("plays the position, turns to Stop, and comes back when it ends", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    click("#sg-play");
    const play = postMessage.mock.calls.map(([message]) => message).find((message) => message.action === "play");
    expect(play?.request).toMatchObject({ mode: "arpeggio", position: 1 });

    const button = document.getElementById("sg-play");
    onScaleGuidePlaybackStarted();
    expect(button?.textContent).toBe("Stop");
    click("#sg-play");
    expect(postMessage.mock.calls.at(-1)?.[0]).toMatchObject({ type: "scaleGuide", action: "stop" });
    onScaleGuidePlaybackEnded();
    expect(button?.textContent).toBe("Play");
  });

  it("plays the run from the tab's own Play, and every play control shows Stop", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    click("#sg-arp-tab .sg-tab-play");
    expect(postMessage.mock.calls.at(-1)?.[0]).toMatchObject({ type: "scaleGuide", action: "play", request: { mode: "arpeggio", position: 1 } });

    onScaleGuidePlaybackStarted();
    expect(document.querySelector("#sg-arp-tab .sg-tab-play")?.textContent).toBe("Stop");
    expect(document.getElementById("sg-play")?.textContent).toBe("Stop");
    // A redrawn card keeps showing what is sounding.
    answer(arpeggioView());
    expect(document.querySelector("#sg-arp-tab .sg-tab-play")?.getAttribute("aria-pressed")).toBe("true");
    click("#sg-arp-tab .sg-tab-play");
    expect(postMessage.mock.calls.at(-1)?.[0]).toMatchObject({ type: "scaleGuide", action: "stop" });
    onScaleGuidePlaybackEnded();
    expect(document.querySelector("#sg-arp-tab .sg-tab-play")?.textContent).toBe("Play");
  });

  it("plays a scale's example from its summary, even over the whole neck", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "scale", position: 0 });
    renderScaleGuidePanel();
    const scale = { id: "minpent", name: "Minor Pentatonic", steps: "3 2 2 3 2", formula: "R ♭3 4 5 ♭7", diatonic: [] };
    answer(arpeggioView({ mode: "scale", position: 0, scale, chord: undefined, arpeggio: undefined, run: undefined }));
    const summary = document.getElementById("sg-summary-main");
    expect(summary?.getAttribute("role")).toBe("button");
    summary?.querySelector(".sg-summary-name")?.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    expect(postMessage.mock.calls.at(-1)?.[0]).toMatchObject({ type: "scaleGuide", action: "playScale", request: { mode: "scale", position: 0 } });

    summary?.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    expect(postMessage.mock.calls.filter(([message]) => message.action === "playScale")).toHaveLength(2);

    // Only a scale's summary plays.
    answer(arpeggioView());
    expect(summary?.hasAttribute("data-sg-play-scale")).toBe(false);
    postMessage.mockClear();
    summary?.click();
    expect(postMessage).not.toHaveBeenCalled();
  });

  it("shows a chord box's shape and strums it", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "chord", position: 0 });
    renderScaleGuidePanel();
    const shapes = [
      { frets: [-1, 3, 2, 0, 1, 0], fingers: [0, 3, 2, 0, 1, 0], barres: [], position: 1, caged: "C" },
      { frets: [8, 10, 10, 9, 8, 8], fingers: [1, 3, 4, 2, 1, 1], barres: [], position: 8, caged: "E" },
    ];
    answer(arpeggioView({ mode: "chord", position: 0, shapes, arpeggio: undefined, run: undefined }));
    click('#sg-boxes .sg-box[data-sg-position="2"]');
    const calls = postMessage.mock.calls.map(([message]) => message);
    expect(calls.find((message) => message.action === "playChord")).toMatchObject({ frets: [8, 10, 10, 9, 8, 8], request: { mode: "chord" } });
    expect(lastViewRequest().request).toMatchObject({ mode: "chord", position: 2 });
  });

  it("lights each note as it falls", () => {
    vi.useFakeTimers();
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    onScaleGuidePlaybackStarted();
    onScaleGuidePlayback({ type: "scaleGuidePlayback", notes: [{ string: 3, fret: 14, startMs: 100, lengthMs: 200 }] });
    const dotAt = (): Element | null => document.querySelector('.sg-dot[data-string="3"][data-fret="14"]');
    expect(dotAt()?.classList.contains("sg-dot-playing")).toBe(false);
    vi.advanceTimersByTime(150);
    expect(dotAt()?.classList.contains("sg-dot-playing")).toBe(true);
    vi.advanceTimersByTime(200);
    expect(dotAt()?.classList.contains("sg-dot-playing")).toBe(false);
    onScaleGuidePlaybackEnded();
    vi.useRealTimers();
  });

  it("sounds a note clicked on the neck", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    document.querySelector('.sg-dot[data-string="3"] circle')?.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    expect(postMessage.mock.calls.at(-1)?.[0]).toEqual({ type: "scaleGuide", action: "playNote", midi: 69, volume: 1 });
  });

  it("offers the string counts, and a new style or count goes back to the first run", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 3 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    expect([...document.querySelectorAll("[data-sg-arp-strings]")].map((button) => button.textContent)).toEqual(["3", "4", "5", "6"]);
    expect(document.querySelector('[data-sg-arp-strings="3"]')?.classList.contains("active")).toBe(true);
    click('[data-sg-arp-strings="6"]');
    expect(lastViewRequest().request).toMatchObject({ arpeggioStrings: 6, position: 1 });
    click('[data-sg-arp-style="two"]');
    expect(lastViewRequest().request).toMatchObject({ arpeggioStyle: "two", position: 1 });
  });

  it("has no string choice for a box", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", arpeggioStyle: "box", position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    expect(hidden("sg-arp-strings-field")).toBe(true);
    expect(document.querySelector('[data-sg-arp-style="box"]')?.getAttribute("aria-pressed")).toBe("true");
  });

  it("says why when there is nothing to play, and shows no tab", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio" });
    renderScaleGuidePanel();
    const arpeggio = { ...arpeggioView().arpeggio!, runs: [] };
    answer(arpeggioView({ position: 0, arpeggio, run: undefined }));
    expect(document.querySelector("#sg-neck .sg-note")?.textContent).toBe("No playable run.");
    expect(hidden("sg-arp-tab")).toBe(true);
  });

  it("offers Order and Fingers on the Arpeggios tab only", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, labels: "order" });
    renderScaleGuidePanel();
    expect(document.querySelector('[data-sg-labels="order"]')?.hasAttribute("hidden")).toBe(true);
    expect(document.querySelector('[data-sg-labels="name"]')?.classList.contains("active")).toBe(true);
    click('[data-sg-mode="arpeggio"]');
    expect(document.querySelector('[data-sg-labels="order"]')?.hasAttribute("hidden")).toBe(false);
    expect(document.querySelector('[data-sg-labels="order"]')?.classList.contains("active")).toBe(true);
    click('[data-sg-mode="chord"]');
    expect(document.querySelector('[data-sg-labels="finger"]')?.hasAttribute("hidden")).toBe(true);
  });

  it("follows the played note, as Scales does", () => {
    settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", listen: true, position: 1 });
    renderScaleGuidePanel();
    answer(arpeggioView());
    const listen = postMessage.mock.calls.map(([message]) => message).find((message) => message.type === "tuner");
    expect(listen).toMatchObject({ action: "listen", enabled: true, chroma: false });
    const reading = { detected: true, noteName: "A", octave: 3, frequency: 220, centOffset: 0, confidence: 1, levelDb: -20 };
    expect(onScaleGuideReading({ type: "tunerUpdate", ...reading })).toBe(true);
    expect(onScaleGuideReading({ type: "tunerUpdate", ...reading })).toBe(true);
    expect(document.getElementById("sg-live")?.textContent).toContain("of the arpeggio");
  });

  describe("marking a heard note", () => {
    // The same A twice on the neck: in the run on the G string, and outside it on the B string.
    const twice = [dot(3, 14, { order: 1, finger: 3 }), dot(4, 9, { inPosition: false })];
    const reading = { type: "tunerUpdate", detected: true, noteName: "A", octave: 4, frequency: 440, centOffset: 0, confidence: 1, levelDb: -20 };
    const hit = (string: number): boolean => document.querySelector(`#sg-neck .sg-dot[data-string="${string}"]`)?.classList.contains("sg-hit") ?? false;

    function hear(listenEverywhere: boolean): void {
      settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", listen: true, position: 1, listenEverywhere });
      renderScaleGuidePanel();
      answer(arpeggioView({ dots: twice }));
      onScaleGuideReading(reading);
      onScaleGuideReading(reading);
    }

    it("marks only its place in the chosen run", () => {
      hear(false);
      expect([hit(3), hit(4)]).toEqual([true, false]);
    });

    it("marks every place it is on the neck when asked to", () => {
      hear(true);
      expect([hit(3), hit(4)]).toEqual([true, true]);
      expect((document.getElementById("sg-listen-everywhere") as HTMLInputElement).checked).toBe(true);
    });

    it("marks nothing it hears while the guide itself plays, unless the whole neck is asked for", () => {
      hear(false);
      onScaleGuidePlaybackStarted();
      onScaleGuideReading({ ...reading, octave: 3 });
      onScaleGuideReading({ ...reading, octave: 3 });
      onScaleGuideReading(reading);
      onScaleGuideReading(reading);
      expect([hit(3), hit(4)]).toEqual([false, false]);
      onScaleGuidePlaybackEnded();
    });

    it("is switched from the instrument menu, without asking the engine again", () => {
      hear(false);
      const asked = lastViewRequest().requestId;
      const box = document.getElementById("sg-listen-everywhere") as HTMLInputElement;
      box.checked = true;
      box.dispatchEvent(new Event("change", { bubbles: true }));
      expect(settingsStore.get(SCALE_GUIDE_SETTING)).toMatchObject({ listenEverywhere: true });
      expect(lastViewRequest().requestId).toBe(asked);
      expect([hit(3), hit(4)]).toEqual([true, true]);
    });
  });

  describe("Play's volume", () => {
    const plays = (): Array<Record<string, unknown>> => postMessage.mock.calls.map(([message]) => message).filter((message) => message.action === "play");

    it("is full at first and goes with everything played", () => {
      settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
      renderScaleGuidePanel();
      answer(arpeggioView());
      expect((document.getElementById("sg-volume") as HTMLInputElement).value).toBe("100");
      click("#sg-play");
      expect(plays().at(-1)).toMatchObject({ volume: 1 });
    });

    it("is kept when the slider moves, and shown again next time", () => {
      settingsStore.set(SCALE_GUIDE_SETTING, { ...DEFAULT_SETTINGS, mode: "arpeggio", position: 1 });
      renderScaleGuidePanel();
      answer(arpeggioView());
      const asked = lastViewRequest().requestId;
      const slider = document.getElementById("sg-volume") as HTMLInputElement;
      slider.value = "40";
      slider.dispatchEvent(new Event("input", { bubbles: true }));
      expect(settingsStore.get(SCALE_GUIDE_SETTING)).toMatchObject({ playVolume: 40 });
      expect(lastViewRequest().requestId).toBe(asked);
      click("#sg-play");
      expect(plays().at(-1)).toMatchObject({ volume: 0.4 });
      expect(normalizeSettings({ playVolume: 400 }).playVolume).toBe(100);
      expect(normalizeSettings({ playVolume: "loud" }).playVolume).toBe(100);
    });
  });
});
