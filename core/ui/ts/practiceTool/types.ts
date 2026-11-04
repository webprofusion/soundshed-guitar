/**
 * Practice Tool state shapes. Split out of ts/types.ts, which re-exports every
 * name here so importers keep using `./types.js` and never learn about the
 * split — the same facade rule the runtime modules follow.
 */

/** The track settings a loop plays with — the four faders. */
export interface PracticeToolLoopSettings {
  gain: number;
  balance: number;
  speed: number;
  pitchSemitones: number;
}

export interface PracticeToolLoopRegion {
  id: string;
  name: string;
  startSec: number;
  endSec: number;
  /** Applied whenever the loop is selected, and kept up to date as the faders
   * move while it is active. Absent on loops saved before loops had their own
   * settings: those take on whatever is current the first time they're picked. */
  settings?: PracticeToolLoopSettings;
}

/**
 * Backing-track EQ: the same four-band ParametricEQEffect the signal path uses,
 * applied to the practice-tool stream only. Unlike loops, the engine *is* the
 * source of truth for the audio — this mirrors what was last sent to it
 * (`setPracticeToolEq`), keyed by the effect's own parameter names ("lowGain",
 * "highFreq", …) so no translation table sits between the two.
 */
export interface PracticeToolEqState {
  enabled: boolean;
  params: Record<string, number>;
}

/**
 * UI-owned Practice Tool (Jam panel) state. `loops` and `activeLoopId` are
 * pure client-side bookkeeping — the engine has no concept of a loop library,
 * it only ever knows the currently-active region's bounds and whether looping
 * is on (see setPracticeToolLoopRegion/setPracticeToolLooping in bridge.ts).
 */
export interface PracticeToolState {
  filePath: string;
  title: string;
  durationSec: number;
  positionSec: number;
  waveformPeaksL: number[];
  waveformPeaksR: number[];
  loops: PracticeToolLoopRegion[];
  activeLoopId: string | null;
  looping: boolean;
  playing: boolean;
  speed: number;
  pitchSemitones: number;
  gain: number;
  balance: number; // -1 (full left) .. 0 (center) .. 1 (full right)
  eq: PracticeToolEqState;
}

/** A web page kept with a project — the tablature, a lesson video, the lyrics.
 * Only http(s): those are all the editor will hand to the system browser. */
export interface PracticeToolProjectLink {
  id: string;
  /** May be empty; the row then shows the URL's host. */
  title: string;
  url: string;
}

/**
 * One point that pins the score to the reference: where a bar (in a given pass
 * through its repeats) starts in the reference's own time. Field for field
 * alphaTab's `FlatSyncPoint`, so it is handed to `score.applyFlatSyncPoints`
 * as it is. The reference's time is its media time, not wall-clock time, so the
 * points survive a change of Speed.
 */
export interface PracticeToolTabSyncPoint {
  barIndex: number;
  /** 0 on the first pass through the bar, 1 on the second (a repeat), … */
  barOccurence: number;
  /** Where in the bar, 0..1. The tab view only sets bar starts (0). */
  barPosition: number;
  millisecondOffset: number;
}

/**
 * A score shown in the tab view, and how it lines up with one reference. The
 * file itself is in the engine's document store under `tabId` (its content hash,
 * see PracticeTabMessages.h); this is only the pointer and the sync.
 */
export interface PracticeToolTabBinding {
  tabId: string;
  fileName: string;
  /** Which of the score's tracks is shown. */
  trackIndex: number;
  syncPoints: PracticeToolTabSyncPoint[];
}

/** A YouTube video used as the reference in place of a backing track. */
export interface PracticeToolYouTubeReference {
  videoId: string;
  /** As YouTube reported it once the video loaded; may be empty. */
  title: string;
}

/**
 * A saved Practice Tool project: the backing track that was loaded, the loops
 * defined on it, the fader settings, and — if the user opted in at save time —
 * the tone preset that was selected. Purely client-side, stored via
 * `updateAppSetting` under `practiceTool.projects`; the engine knows nothing of it.
 */
export interface PracticeToolProject {
  id: string;
  name: string;
  /** As reported by `practiceToolFileLoaded` — a real path for a browsed file,
   * but only a bare filename for one dropped in (WebView2 never exposes the
   * path), which is why recall falls back to matching the loaded track. */
  filePath: string;
  fileTitle: string;
  durationSec: number;
  loops: PracticeToolLoopRegion[];
  activeLoopId: string | null;
  gain: number;
  balance: number;
  speed: number;
  pitchSemitones: number;
  /** Absent in projects saved before the backing-track EQ existed — recalling
   * one of those leaves the EQ flat and off rather than inventing a curve. */
  eq?: PracticeToolEqState;
  presetId?: string;
  /** Captured at save time so the row still reads sensibly if the preset is gone. */
  presetName?: string;
  /** Edited on the saved project directly (no Save needed), and carried over
   * when Save overwrites it. Absent until the first link is added. */
  links?: PracticeToolProjectLink[];
  /** The tab and its sync to this project's reference. Absent when no tab was open. */
  tab?: PracticeToolTabBinding;
  /** Set when the project's reference is a YouTube video. `filePath` is then
   * empty unless a backing track was also loaded. */
  youtube?: PracticeToolYouTubeReference;
  savedAt: number;
}
