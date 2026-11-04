import { beforeEach, describe, expect, it } from "vitest";
import { uiState } from "../ts/state.js";
import { createDefaultPracticeToolEq } from "../ts/practiceTool/eq.js";
import type { PracticeToolTabSyncPoint } from "../ts/types.js";
import {
  buildPlaybackOrder,
  formatSyncTime,
  nextSlot,
  removeBarStart,
  sanitizeSyncPoints,
  setBarStart,
  shiftSyncPoints,
  slotForBarNearTick,
  sortSyncPoints,
} from "../ts/practiceTool/tab/syncPoints.js";
import { parseYouTubeStartSeconds, parseYouTubeVideoId } from "../ts/practiceTool/tab/youtubeUrl.js";
import { capturePracticeToolProject, parsePracticeToolProject } from "../ts/practiceTool/projects.js";
import {
  getReferenceKey,
  loadBindingForReference,
  parseTabBinding,
  persistBindingForReference,
  setReferenceKind,
  setTabBinding,
  setYouTubeReference,
} from "../ts/practiceTool/tab/tabState.js";

/** Bars 0..4 where 1..2 repeat once: 0 1 2 1 2 3 4, each 960 ticks. */
const REPEATED = buildPlaybackOrder(
  [0, 1, 2, 1, 2, 3, 4].map((index, position) => ({ masterBar: { index }, start: position * 960 }))
);

function point(barIndex: number, barOccurence: number, millisecondOffset: number): PracticeToolTabSyncPoint {
  return { barIndex, barOccurence, barPosition: 0, millisecondOffset };
}

describe("playback order", () => {
  it("numbers each pass through a repeated bar", () => {
    expect(REPEATED.map((slot) => `${slot.barIndex}.${slot.occurrence}`)).toEqual(["0.0", "1.0", "2.0", "1.1", "2.1", "3.0", "4.0"]);
  });

  it("steps through the passes in the order they play", () => {
    expect(nextSlot(REPEATED, 2, 0)).toMatchObject({ barIndex: 1, occurrence: 1 });
    expect(nextSlot(REPEATED, 4, 0)).toBeNull();
    expect(nextSlot(REPEATED, 9, 0)).toBeNull();
  });

  it("takes a clicked repeated bar as the pass nearest the cursor", () => {
    expect(slotForBarNearTick(REPEATED, 1, 0)).toMatchObject({ occurrence: 0 });
    expect(slotForBarNearTick(REPEATED, 1, 4000)).toMatchObject({ occurrence: 1 });
    expect(slotForBarNearTick(REPEATED, 7, 0)).toBeNull();
  });
});

describe("setBarStart", () => {
  it("adds a point and keeps the list in playback order", () => {
    let points = setBarStart(REPEATED, [], { barIndex: 3, occurrence: 0 }, 10_000);
    points = setBarStart(REPEATED, points, { barIndex: 1, occurrence: 1 }, 6_000);
    points = setBarStart(REPEATED, points, { barIndex: 0, occurrence: 0 }, 500.4);
    expect(points).toEqual([point(0, 0, 500), point(1, 1, 6_000), point(3, 0, 10_000)]);
  });

  it("replaces the point already on that pass", () => {
    const points = setBarStart(REPEATED, [point(1, 0, 2_000)], { barIndex: 1, occurrence: 0 }, 2_100);
    expect(points).toEqual([point(1, 0, 2_100)]);
  });

  it("keeps a second pass separate from the first", () => {
    const points = setBarStart(REPEATED, [point(1, 0, 2_000)], { barIndex: 1, occurrence: 1 }, 6_000);
    expect(points).toEqual([point(1, 0, 2_000), point(1, 1, 6_000)]);
  });

  it("drops points that would make time run backwards", () => {
    const before = [point(0, 0, 0), point(1, 0, 2_000), point(2, 0, 4_000), point(3, 0, 10_000)];
    // Bar 1's second pass marked at 3 s: bar 2 at 4 s (earlier in playback) now contradicts it.
    const points = setBarStart(REPEATED, before, { barIndex: 1, occurrence: 1 }, 3_000);
    expect(points).toEqual([point(0, 0, 0), point(1, 0, 2_000), point(1, 1, 3_000), point(3, 0, 10_000)]);
  });

  it("ignores a pass the score doesn't have, and drops stored ones", () => {
    expect(setBarStart(REPEATED, [], { barIndex: 3, occurrence: 1 }, 1_000)).toEqual([]);
    expect(sortSyncPoints(REPEATED, [point(9, 0, 1_000), point(0, 0, 0)])).toEqual([point(0, 0, 0)]);
  });

  it("removes one pass's point", () => {
    expect(removeBarStart([point(1, 0, 2_000), point(1, 1, 6_000)], { barIndex: 1, occurrence: 1 })).toEqual([point(1, 0, 2_000)]);
  });
});

describe("shiftSyncPoints", () => {
  it("moves every point, never before zero", () => {
    expect(shiftSyncPoints([point(0, 0, 30), point(1, 0, 2_000)], -10)).toEqual([point(0, 0, 20), point(1, 0, 1_990)]);
    expect(shiftSyncPoints([point(0, 0, 5), point(1, 0, 2_000)], -10)).toEqual([point(0, 0, 0), point(1, 0, 1_995)]);
    expect(shiftSyncPoints([], 10)).toEqual([]);
  });
});

describe("sanitizeSyncPoints", () => {
  it("keeps well-formed points and drops the rest", () => {
    expect(sanitizeSyncPoints([
      point(0, 0, 0),
      { barIndex: -1, barOccurence: 0, barPosition: 0, millisecondOffset: 0 },
      { barIndex: 1.5, barOccurence: 0, barPosition: 0, millisecondOffset: 0 },
      { barIndex: 2, barOccurence: 0, barPosition: 3, millisecondOffset: 10 },
      { barIndex: 3, barOccurence: 0, barPosition: 0, millisecondOffset: Number.NaN },
      "junk",
    ])).toEqual([point(0, 0, 0), { barIndex: 2, barOccurence: 0, barPosition: 1, millisecondOffset: 10 }]);
    expect(sanitizeSyncPoints({})).toEqual([]);
  });

  it("formats sync times", () => {
    expect(formatSyncTime(65_432)).toBe("1:05.43");
    expect(formatSyncTime(0)).toBe("0:00.00");
  });
});

describe("parseYouTubeVideoId", () => {
  it.each([
    ["dQw4w9WgXcQ", "dQw4w9WgXcQ"],
    ["https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42s", "dQw4w9WgXcQ"],
    ["youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ"],
    ["https://youtu.be/dQw4w9WgXcQ?si=abc", "dQw4w9WgXcQ"],
    ["https://m.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ"],
    ["https://music.youtube.com/watch?v=dQw4w9WgXcQ&list=x", "dQw4w9WgXcQ"],
    ["https://www.youtube.com/shorts/dQw4w9WgXcQ", "dQw4w9WgXcQ"],
    ["https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ", "dQw4w9WgXcQ"],
    ["https://www.youtube.com/live/dQw4w9WgXcQ", "dQw4w9WgXcQ"],
  ])("reads %s", (input, expected) => {
    expect(parseYouTubeVideoId(input)).toBe(expected);
  });

  it.each([
    "",
    "not a link",
    "https://www.youtube.com/playlist?list=PL123",
    "https://example.com/watch?v=dQw4w9WgXcQ",
    "https://www.youtube.com/watch?v=short",
    "javascript:alert(1)",
  ])("refuses %s", (input) => {
    expect(parseYouTubeVideoId(input)).toBeNull();
  });

  it("reads a start time", () => {
    expect(parseYouTubeStartSeconds("https://youtu.be/dQw4w9WgXcQ?t=90")).toBe(90);
    expect(parseYouTubeStartSeconds("https://youtu.be/dQw4w9WgXcQ?t=1m30s")).toBe(90);
    expect(parseYouTubeStartSeconds("https://www.youtube.com/watch?v=dQw4w9WgXcQ&start=12")).toBe(12);
    expect(parseYouTubeStartSeconds("https://youtu.be/dQw4w9WgXcQ")).toBe(0);
  });
});

describe("tab bindings", () => {
  beforeEach(() => {
    uiState.appSettings = {};
    uiState.practiceTool = {
      filePath: "C:\\Music\\Song.wav",
      title: "Song.wav",
      durationSec: 200,
      positionSec: 0,
      waveformPeaksL: [],
      waveformPeaksR: [],
      loops: [],
      activeLoopId: null,
      looping: false,
      playing: false,
      speed: 1,
      pitchSemitones: 0,
      gain: 1,
      balance: 0,
      eq: createDefaultPracticeToolEq(),
    };
    setReferenceKind("track");
    setYouTubeReference(null);
    setTabBinding(null, { persist: false });
  });

  it("remembers the tab against the track it was synced to", () => {
    setTabBinding({ tabId: "abc", fileName: "song.gp5", trackIndex: 1, syncPoints: [point(0, 0, 1_200)] });
    expect(getReferenceKey()).toBe("track:c:\\music\\song.wav");
    expect(loadBindingForReference("track:c:\\music\\song.wav")).toEqual({
      tabId: "abc",
      fileName: "song.gp5",
      trackIndex: 1,
      syncPoints: [point(0, 0, 1_200)],
    });
    setTabBinding(null);
    expect(loadBindingForReference("track:c:\\music\\song.wav")).toBeNull();
  });

  it("keys a YouTube reference by its video", () => {
    setReferenceKind("youtube");
    setYouTubeReference({ videoId: "dQw4w9WgXcQ", title: "" });
    setTabBinding({ tabId: "abc", fileName: "song.gp5", trackIndex: 0, syncPoints: [] });
    persistBindingForReference();
    expect(loadBindingForReference("yt:dQw4w9WgXcQ")?.tabId).toBe("abc");
  });

  it("refuses a stored binding with no file", () => {
    expect(parseTabBinding({ fileName: "x.gp" })).toBeNull();
    expect(parseTabBinding({ tabId: "abc", trackIndex: -2, syncPoints: "junk" })).toEqual({
      tabId: "abc",
      fileName: "Tablature",
      trackIndex: 0,
      syncPoints: [],
    });
  });

  it("saves and restores a project's tab", () => {
    const tab = { tabId: "abc", fileName: "song.gp5", trackIndex: 2, syncPoints: [point(0, 0, 900)] };
    const project = capturePracticeToolProject("Song", { includePreset: false, tab: { tab } });
    expect(project?.tab).toEqual(tab);
    expect(parsePracticeToolProject(JSON.parse(JSON.stringify(project)))?.tab).toEqual(tab);
  });

  it("saves a project that follows only a YouTube video", () => {
    uiState.practiceTool!.filePath = "";
    expect(capturePracticeToolProject("Video", { includePreset: false })).toBeNull();
    const project = capturePracticeToolProject("Video", {
      includePreset: false,
      tab: { youtube: { videoId: "dQw4w9WgXcQ", title: "A video" } },
    });
    expect(project).toMatchObject({ filePath: "", fileTitle: "Song.wav", youtube: { videoId: "dQw4w9WgXcQ", title: "A video" } });
    const parsed = parsePracticeToolProject(JSON.parse(JSON.stringify(project)));
    expect(parsed?.youtube?.videoId).toBe("dQw4w9WgXcQ");
    expect(parsePracticeToolProject({ id: "p", name: "No reference", filePath: "" })).toBeNull();
    expect(parsePracticeToolProject({ id: "p", name: "Bad video", filePath: "", youtube: { videoId: "x" } })).toBeNull();
  });
});
