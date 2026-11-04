/**
 * The backing track's playhead between engine reports.
 *
 * The engine reports the transport a dozen times a second; in between, the
 * position is dead-reckoned from the last report, the time it arrived and the
 * Speed. Split out of practiceTool.ts so the tab view (practiceTool/tab/) can
 * follow the same position the waveform draws, and hear about every play, pause,
 * seek and load through `onPlayheadChange`, without importing the facade.
 */

import { uiState } from "../state.js";

let baseSec = 0;
let baseMs = 0;
let rate = 1;
let animFrame: number | null = null;
const listeners = new Set<() => void>();
let seeker: ((sec: number) => void) | null = null;

/** Re-anchors the dead reckoning at `sec`, now; `speed` when the rate changed too.
 * Every caller is a transport event, so this is also what listeners hear. */
export function setPlayheadBase(sec: number, speed?: number): void {
  baseSec = sec;
  baseMs = performance.now();
  if (speed !== undefined) {
    rate = speed;
  }
  for (const listener of listeners) {
    listener();
  }
}

/** The track position now, in seconds, as the waveform draws it. */
export function getInterpolatedPositionSec(): number {
  const player = uiState.practiceTool;
  if (!player) {
    return 0;
  }
  if (!player.playing) {
    return player.positionSec;
  }
  const projected = baseSec + ((performance.now() - baseMs) / 1000) * rate;
  return Math.max(0, Math.min(player.durationSec, projected));
}

/** Called after every play, pause, seek, Speed change and file load. Returns the unsubscribe. */
export function onPlayheadChange(listener: () => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function stopPlayheadAnim(): void {
  if (animFrame !== null) {
    cancelAnimationFrame(animFrame);
    animFrame = null;
  }
}

/** Runs `onFrame` every animation frame while the track plays. */
export function startPlayheadAnim(onFrame: () => void): void {
  if (animFrame !== null) {
    return;
  }
  const step = () => {
    if (!uiState.practiceTool?.playing) {
      animFrame = null;
      return;
    }
    onFrame();
    animFrame = requestAnimationFrame(step);
  };
  animFrame = requestAnimationFrame(step);
}

/** Registered once by practiceTool.ts with its own seek (engine, playhead and redraw). */
export function setPracticeToolSeeker(fn: (sec: number) => void): void {
  seeker = fn;
}

export function requestPracticeToolSeek(sec: number): void {
  seeker?.(sec);
}
