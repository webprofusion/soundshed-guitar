/**
 * The Scale Guide's Play (docs/plans/scale-guide.md): the engine sounds the chosen position through
 * the preset and says when each note falls; this follows along on the neck. A note clicked on the
 * neck is too short to follow, so it is sent and forgotten.
 */

import { postMessage } from "../bridge.js";
import type { IncomingPayload } from "../messages/types.js";

/** The engine's id for a played position, which sets its reports apart from a demo clip's. */
export const SCALE_GUIDE_PREVIEW_ID = "scale-guide";

const PLAYING_CLASS = "sg-dot-playing";

interface TimedNote {
  string: number;
  fret: number;
  startMs: number;
  lengthMs: number;
}

let playing = false;
let timers: number[] = [];

function neckDot(note: TimedNote): Element | null {
  return document.querySelector(`#sg-neck .sg-dot[data-string="${note.string}"][data-fret="${note.fret}"]`);
}

/** Every play control, the one beside Listen and the tab's, shows Stop while anything plays. */
export function showScaleGuidePlaying(): void {
  document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-play]").forEach((button) => {
    button.classList.toggle("active", playing);
    button.setAttribute("aria-pressed", String(playing));
    button.textContent = playing ? "Stop" : "Play";
  });
  document.getElementById("sg-summary-main")?.classList.toggle("sg-summary-playing", playing);
}

function clearFollowing(): void {
  timers.forEach((timer) => window.clearTimeout(timer));
  timers = [];
  document.querySelectorAll(`#sg-neck .${PLAYING_CLASS}`).forEach((dot) => dot.classList.remove(PLAYING_CLASS));
}

export function isScaleGuidePlaying(): boolean {
  return playing;
}

export function onScaleGuidePlaybackStarted(): void {
  playing = true;
  showScaleGuidePlaying();
}

export function onScaleGuidePlaybackEnded(): void {
  playing = false;
  clearFollowing();
  showScaleGuidePlaying();
}

/** When each note falls: its dot lights for as long as it sounds, or a run's next note. */
export function onScaleGuidePlayback(payload: IncomingPayload): void {
  clearFollowing();

  for (const note of (payload as { notes?: TimedNote[] }).notes ?? []) {
    timers.push(window.setTimeout(() => neckDot(note)?.classList.add(PLAYING_CLASS), note.startMs));
    timers.push(window.setTimeout(() => neckDot(note)?.classList.remove(PLAYING_CLASS), note.startMs + note.lengthMs));
  }
}

/** One press of a play control: sounds what it asks for, or stops whatever is sounding. */
export function toggleScaleGuidePlay(action: "play" | "playScale", request: unknown): void {
  if (playing) {
    stopScaleGuidePlayback();
  } else {
    postMessage({ type: "scaleGuide", action, request });
  }
}

/** A chord box clicked: its shape, strummed on the request's strings. */
export function playScaleGuideChord(request: unknown, frets: number[] | undefined): void {
  if (frets?.length) {
    postMessage({ type: "scaleGuide", action: "playChord", request, frets });
  }
}

/** A scale's summary plays it, one octave up and back: it reads as a button only then. */
export function markSummaryPlayable(playable: boolean): void {
  const main = document.getElementById("sg-summary-main");

  if (main) {
    main.toggleAttribute("data-sg-play-scale", playable);
    main.classList.toggle("sg-summary-playable", playable);
    main.setAttribute("role", playable ? "button" : "group");
    main.tabIndex = playable ? 0 : -1;
    main.title = playable ? "Hear the scale, one octave up and back" : "";
  }
}

export function stopScaleGuidePlayback(): void {
  if (playing) {
    postMessage({ type: "scaleGuide", action: "stop" });
  }
}

export function playScaleGuideNote(midi: number): void {
  if (Number.isFinite(midi)) {
    postMessage({ type: "scaleGuide", action: "playNote", midi });
  }
}

export function resetPlaybackForTests(): void {
  playing = false;
  clearFollowing();
}
