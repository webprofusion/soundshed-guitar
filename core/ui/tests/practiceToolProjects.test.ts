import { beforeEach, describe, expect, it } from "vitest";
import { uiState } from "../ts/state.js";
import type { PracticeToolProject, PracticeToolState } from "../ts/types.js";
import {
  capturePracticeToolProject,
  consumePendingProjectRecall,
  deletePracticeToolProject,
  findPracticeToolProjectByName,
  getProjectTrackAvailability,
  parsePracticeToolProject,
  readPracticeToolProjects,
  setPendingProjectRecall,
  storePracticeToolProject,
} from "../ts/practiceTool/projects.js";

const PROJECTS_KEY = "practiceTool.projects";

function loadedTrack(overrides: Partial<PracticeToolState> = {}): PracticeToolState {
  return {
    filePath: "C:\\Music\\backing.wav",
    title: "backing.wav",
    durationSec: 180.5,
    positionSec: 0,
    waveformPeaksL: [],
    waveformPeaksR: [],
    loops: [
      { id: "loop-a", name: "Verse 1", startSec: 4, endSec: 12 },
      { id: "loop-b", name: "Solo 1", startSec: 60, endSec: 75 },
    ],
    activeLoopId: "loop-b",
    looping: true,
    playing: false,
    speed: 0.75,
    pitchSemitones: -2,
    gain: 0.8,
    balance: 0.25,
    ...overrides,
  };
}

beforeEach(() => {
  uiState.appSettings = {};
  uiState.practiceTool = loadedTrack();
  uiState.activePresetId = null;
  uiState.presets = [];
  setPendingProjectRecall(null);
});

describe("capturePracticeToolProject", () => {
  it("captures the track, its loops and every fader setting", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false });
    expect(project).not.toBeNull();
    expect(project).toMatchObject({
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      fileTitle: "backing.wav",
      durationSec: 180.5,
      activeLoopId: "loop-b",
      gain: 0.8,
      balance: 0.25,
      speed: 0.75,
      pitchSemitones: -2,
    });
    expect(project?.loops).toHaveLength(2);
    expect(project?.presetId).toBeUndefined();
  });

  it("copies the loops rather than aliasing the live ones", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false });
    uiState.practiceTool!.loops[0].name = "renamed after saving";
    expect(project?.loops[0].name).toBe("Verse 1");
  });

  it("includes the selected preset — with its display name — only when asked", () => {
    uiState.activePresetId = "preset-7";
    uiState.presets = [{ id: "preset-7", name: "Crunch Rhythm" }] as typeof uiState.presets;

    expect(capturePracticeToolProject("Song A", { includePreset: false })?.presetId).toBeUndefined();
    expect(capturePracticeToolProject("Song A", { includePreset: true })).toMatchObject({
      presetId: "preset-7",
      presetName: "Crunch Rhythm",
    });
  });

  it("refuses to capture without a loaded track or a name", () => {
    expect(capturePracticeToolProject("   ", { includePreset: false })).toBeNull();
    uiState.practiceTool = loadedTrack({ filePath: "" });
    expect(capturePracticeToolProject("Song A", { includePreset: false })).toBeNull();
  });

  it("reuses the given id so saving over a project updates it in place", () => {
    const first = capturePracticeToolProject("Song A", { includePreset: false })!;
    storePracticeToolProject(first);
    const second = capturePracticeToolProject("Song A", { includePreset: false, projectId: first.id })!;
    storePracticeToolProject(second);
    expect(readPracticeToolProjects()).toHaveLength(1);
  });
});

describe("project storage", () => {
  it("round-trips through app settings, newest first", () => {
    const older = { ...capturePracticeToolProject("Older", { includePreset: false })!, savedAt: 1_000 };
    const newer = { ...capturePracticeToolProject("Newer", { includePreset: false })!, savedAt: 2_000 };
    storePracticeToolProject(older);
    storePracticeToolProject(newer);

    expect(readPracticeToolProjects().map((project) => project.name)).toEqual(["Newer", "Older"]);
  });

  it("matches saved names case-insensitively so Save offers to overwrite", () => {
    storePracticeToolProject(capturePracticeToolProject("Song A", { includePreset: false })!);
    expect(findPracticeToolProjectByName("  song a  ")?.name).toBe("Song A");
    expect(findPracticeToolProjectByName("Song B")).toBeNull();
  });

  it("deletes by id and reports whether anything was removed", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false })!;
    storePracticeToolProject(project);
    expect(deletePracticeToolProject("no-such-project")).toBe(false);
    expect(deletePracticeToolProject(project.id)).toBe(true);
    expect(readPracticeToolProjects()).toEqual([]);
  });

  it("skips entries a hand-edited settings blob has broken, keeping the rest", () => {
    const good = capturePracticeToolProject("Song A", { includePreset: false })!;
    uiState.appSettings[PROJECTS_KEY] = [
      good,
      { id: "no-name", filePath: "C:\\x.wav" },
      "not an object",
      null,
    ] as unknown as typeof uiState.appSettings[string];

    expect(readPracticeToolProjects().map((project) => project.name)).toEqual(["Song A"]);
  });
});

describe("parsePracticeToolProject", () => {
  it("fills in the settings an older entry never recorded", () => {
    const parsed = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
    });
    expect(parsed).toMatchObject({
      fileTitle: "backing.wav",
      durationSec: 0,
      loops: [],
      activeLoopId: null,
      gain: 1,
      balance: 0,
      speed: 1,
      pitchSemitones: 0,
    });
  });

  it("drops an active loop id that no longer names one of the saved loops", () => {
    const parsed = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      loops: [{ id: "loop-a", name: "Verse 1", startSec: 1, endSec: 2 }],
      activeLoopId: "loop-gone",
    });
    expect(parsed?.activeLoopId).toBeNull();
  });

  it("keeps a preset name only alongside the preset id it describes", () => {
    const orphaned = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      presetName: "Crunch Rhythm",
    });
    expect(orphaned?.presetId).toBeUndefined();
    expect(orphaned?.presetName).toBeUndefined();
  });
});

describe("getProjectTrackAvailability", () => {
  const project = (filePath: string): PracticeToolProject => ({
    ...capturePracticeToolProject("Song A", { includePreset: false })!,
    filePath,
  });

  it("reports the loaded track regardless of path casing", () => {
    expect(getProjectTrackAvailability(project("c:\\music\\BACKING.wav"))).toBe("loaded");
  });

  it("reports a reloadable path for Windows, UNC and POSIX absolutes", () => {
    uiState.practiceTool = loadedTrack({ filePath: "" });
    expect(getProjectTrackAvailability(project("D:/tracks/song.mp3"))).toBe("reload");
    expect(getProjectTrackAvailability(project("\\\\nas\\share\\song.wav"))).toBe("reload");
    expect(getProjectTrackAvailability(project("/home/chris/song.wav"))).toBe("reload");
  });

  it("reports a dropped-in file — recorded as a bare name — as unavailable", () => {
    uiState.practiceTool = loadedTrack({ filePath: "" });
    expect(getProjectTrackAvailability(project("song.wav"))).toBe("unavailable");
  });
});

describe("consumePendingProjectRecall", () => {
  it("hands back the parked project when its own file load lands", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false })!;
    setPendingProjectRecall(project);
    expect(consumePendingProjectRecall("c:\\music\\backing.wav")?.id).toBe(project.id);
  });

  it("drops the recall when the user loads some other file first", () => {
    setPendingProjectRecall(capturePracticeToolProject("Song A", { includePreset: false })!);
    expect(consumePendingProjectRecall("C:\\Music\\something-else.wav")).toBeNull();
  });

  it("is one-shot, so a later load is not silently reconfigured", () => {
    setPendingProjectRecall(capturePracticeToolProject("Song A", { includePreset: false })!);
    expect(consumePendingProjectRecall("C:\\Music\\backing.wav")).not.toBeNull();
    expect(consumePendingProjectRecall("C:\\Music\\backing.wav")).toBeNull();
  });
});
