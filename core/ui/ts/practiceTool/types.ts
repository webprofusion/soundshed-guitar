/**
 * Practice Tool state shapes. Split out of ts/types.ts, which re-exports every
 * name here so importers keep using `./types.js` and never learn about the
 * split — the same facade rule the runtime modules follow.
 */

export interface PracticeToolLoopRegion {
  id: string;
  name: string;
  startSec: number;
  endSec: number;
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

/**
 * A saved Practice Tool project: the backing track that was loaded, the loops
 * defined on it, the fader settings, and — if the user opted in at save time —
 * the tone preset that was selected. Purely client-side, stored via
 * `setAppSetting` under `practiceTool.projects`; the engine knows nothing of it.
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
  savedAt: number;
}
