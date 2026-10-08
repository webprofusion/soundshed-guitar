import type { PracticeToolLoopRegion } from "../types.js";

/**
 * Jump Back: return playback to where the current pass started, so a phrase
 * can be tried again without stopping or reaching for the waveform.
 *
 * "Where it started" is the active loop's start, unless the user has since
 * clicked a point inside that loop — practising the second half of a solo
 * means coming back to the middle of it, not its top. With no loop active it
 * is the last point clicked on the waveform, or the start of the track.
 *
 * The point is per session and only ever set by a click. Activating a loop
 * (selecting it, playing its row, creating it, recalling a project) and
 * loading a file clear it, so a stale point from a previous loop never
 * overrides the new loop's start.
 */

type LoopBounds = Pick<PracticeToolLoopRegion, "startSec" | "endSec">;

let clickedSec: number | null = null;

export function setJumpBackPoint(sec: number): void {
  clickedSec = Math.max(0, sec);
}

export function clearJumpBackPoint(): void {
  clickedSec = null;
}

/** Where Jump Back goes. A clicked point the loop's edges have since been
 * dragged past no longer counts; the loop's start does. */
export function getJumpBackTargetSec(activeLoop: LoopBounds | null): number {
  const fallback = activeLoop ? activeLoop.startSec : 0;
  if (clickedSec === null) {
    return fallback;
  }
  if (activeLoop && (clickedSec < activeLoop.startSec || clickedSec >= activeLoop.endSec)) {
    return fallback;
  }
  return clickedSec;
}

/** The clicked point while Jump Back would use it in place of the loop's start
 * (or the top of the track), for the waveform to mark; otherwise null. */
export function getJumpBackMarkerSec(activeLoop: LoopBounds | null): number | null {
  const target = getJumpBackTargetSec(activeLoop);
  return target === (activeLoop ? activeLoop.startSec : 0) ? null : target;
}

function getJumpBackButton(): HTMLButtonElement | null {
  return document.getElementById("practice-tool-jump-back") as HTMLButtonElement | null;
}

export function renderJumpBackButton(hasAudio: boolean): void {
  const btn = getJumpBackButton();
  if (btn) {
    btn.disabled = !hasAudio;
  }
}

/** `jumpTo` does the seek: the transport and playhead live with the panel. */
export function bindJumpBackButton(getActiveLoop: () => LoopBounds | null, jumpTo: (sec: number) => void): void {
  const btn = getJumpBackButton();
  if (!btn || btn.dataset.bound === "true") {
    return;
  }
  btn.dataset.bound = "true";
  btn.addEventListener("click", () => jumpTo(getJumpBackTargetSec(getActiveLoop())));
}
