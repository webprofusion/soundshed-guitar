/**
 * The Scale Guide's Play (docs/plans/scale-guide.md): the engine sounds the chosen position through
 * the preset and says when each note falls; this follows along on the neck and in the tab, and
 * steps a progression's arpeggios from chord to chord as they come. A note clicked on the neck is
 * too short to follow, so it is sent and forgotten.
 */

import { postMessage } from "../bridge.js";
import type { IncomingPayload } from "../messages/types.js";

/** The engine's id for a played position, which sets its reports apart from a demo clip's. */
export const SCALE_GUIDE_PREVIEW_ID = "scale-guide";

const PLAYING_CLASS = "sg-dot-playing";
const TAB_PLAYING_CLASS = "sg-tab-playing";
/** A progression's next chord is asked for this long before its first note, so the neck is drawn in time. */
const STEP_LEAD_MS = 80;

interface TimedNote {
  string: number;
  fret: number;
  startMs: number;
  lengthMs: number;
  /** Which step of the tab this is, and which chord of a progression: -1 or absent for neither. */
  step?: number;
  chord?: number;
}

let playing = false;
let timers: number[] = [];
/** How loud Play's guitar is, 0 to 1, sent with everything played. */
let volume = 1;

/** The slider beside Play: shows the saved level and keeps it for what is played next. */
export function showPlayVolume(percent: number): void {
  volume = Math.min(100, Math.max(0, percent)) / 100;
  const slider = document.getElementById("sg-volume") as HTMLInputElement | null;

  if (slider && document.activeElement !== slider) {
    slider.value = String(percent);
  }

  slider?.style.setProperty("--sg-volume", `${Math.round(volume * 100)}%`);
}

export function bindPlayVolume(changed: (percent: number) => void): void {
  document.getElementById("sg-volume")?.addEventListener("input", (event) => changed(Number((event.target as HTMLInputElement).value)));
}
let showChord: ((chord: number) => void) | null = null;

/** How a progression's arpeggios move the guide on as they play: panel.ts gives its step. */
export function followProgressionWith(show: (chord: number) => void): void {
  showChord = show;
}

function tabNote(note: TimedNote): Element | null {
  return (note.step ?? -1) >= 0 ? document.querySelector(`#sg-arp-tab [data-sg-tab-step="${note.step}"]`) : null;
}

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
  document.querySelectorAll(`#sg-arp-tab .${TAB_PLAYING_CLASS}`).forEach((note) => note.classList.remove(TAB_PLAYING_CLASS));
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
  let chord = -1;
  // Play waits for the metronome's bar line when it is clicking: so does the following.
  const { notes = [], delayMs = 0 } = payload as { notes?: TimedNote[]; delayMs?: number };

  for (const note of notes) {
    const lit = (on: boolean) => (): void => {
      neckDot(note)?.classList.toggle(PLAYING_CLASS, on);
      tabNote(note)?.classList.toggle(TAB_PLAYING_CLASS, on);
    };

    if (note.chord !== undefined && note.chord >= 0 && note.chord !== chord) {
      const next = note.chord;
      chord = next;
      timers.push(window.setTimeout(() => showChord?.(next), Math.max(0, delayMs + note.startMs - STEP_LEAD_MS)));
    }

    timers.push(window.setTimeout(lit(true), delayMs + note.startMs));
    timers.push(window.setTimeout(lit(false), delayMs + note.startMs + note.lengthMs));
  }
}

/** One press of a play control: sounds what it asks for, or stops whatever is sounding. */
export function toggleScaleGuidePlay(action: "play" | "playScale", request: unknown): void {
  if (playing) {
    stopScaleGuidePlayback();
  } else {
    postMessage({ type: "scaleGuide", action, request, volume });
  }
}

/** A bar of a progression's tab clicked: that chord's arpeggio alone, in place of whatever is sounding. */
export function playScaleGuideBar(request: unknown, bar: number): void {
  postMessage({ type: "scaleGuide", action: "play", request, bar, volume });
}

/** A chord box clicked: its shape, strummed on the request's strings. */
export function playScaleGuideChord(request: unknown, frets: number[] | undefined): void {
  if (frets?.length) {
    postMessage({ type: "scaleGuide", action: "playChord", request, frets, volume });
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
    postMessage({ type: "scaleGuide", action: "playNote", midi, volume });
  }
}

export function resetPlaybackForTests(): void {
  playing = false;
  clearFollowing();
}
