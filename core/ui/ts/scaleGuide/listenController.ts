/**
 * The Scale Guide's LISTEN, wired to the panel through `ListenHost` (panel.ts): asks the engine to
 * listen whenever the guide is on screen (`tuner {action: "listen"}`; there is no switch), follows the
 * played note in Scales and Arpeggios mode, and in Chords mode turns chroma readings into clean strums that step a
 * progression on. listen.ts holds the logic, listenView.ts the page writes.
 */

import { postMessage } from "../bridge.js";
import type { IncomingPayload } from "../messages/types.js";
import { matchChord, NoteFollower, readingMidi, StrumGate } from "./listen.js";
import { noteReadout, setReadout, showMatch, showNotes } from "./listenView.js";
import { isScaleGuidePlaying, isScaleGuideSounding } from "./playback.js";
import type { ScaleGuideSettings, ScaleGuideView } from "./types.js";

export interface ListenHost {
  settings(): ScaleGuideSettings;
  view(): ScaleGuideView | null;
  step(): number;
  goStep(index: number): void;
}

let sentListen: { enabled: boolean; chroma: boolean } | null = null;
const follower = new NoteFollower();
const strums = new StrumGate();
let matchSignature = "";
let flashUntil = 0;
let round = 1;
let cleanCount = 0;

function element<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

/** Whether the guide is on screen: Jam open on its Scales section. */
function guideVisible(): boolean {
  const root = element<HTMLElement>("scale-guide");
  return Boolean(root && root.isConnected && !root.closest("[hidden], .tab-panel:not(.active)"));
}

/** A new progression, or its restart: counting rounds and clean chords begins again. */
export function resetRound(): void {
  round = 1;
  cleanCount = 0;
}

/** Asks the engine to listen while the guide is showing, with chroma in Chords mode. */
export function syncListening(host: ListenHost): void {
  const enabled = guideVisible();
  const chroma = enabled && host.settings().mode === "chord";

  if ((sentListen === null && !enabled) || (sentListen?.enabled === enabled && sentListen.chroma === chroma)) {
    return;
  }

  sentListen = { enabled, chroma };
  postMessage({ type: "tuner", action: "listen", enabled, chroma });
  follower.reset();
  strums.reset();
  matchSignature = "";
  renderListen(host);
}

/** Draws LISTEN's state over the current view: the notes followed, or the chord meter. */
export function renderListen(host: ListenHost): void {
  const view = host.view();
  const root = element<HTMLElement>("scale-guide");
  const neck = element<HTMLElement>("sg-neck");
  const readout = element<HTMLElement>("sg-live");
  const listening = Boolean(sentListen?.enabled && view);
  const chordMode = view?.mode === "chord";
  element<HTMLElement>("sg-match")?.toggleAttribute("hidden", !listening || !chordMode);
  const roundLabel = element<HTMLElement>("sg-round");

  if (roundLabel) {
    roundLabel.hidden = !listening || !view?.progression;
    roundLabel.textContent = `Round ${round}`;
  }

  if (!root || !neck || !readout || !view) {
    return;
  }

  const state = host.settings();
  // The guide's own Play lights the notes it sounds: hearing them too would mark each one all over the neck.
  const heard = listening && !chordMode && (state.listenEverywhere || !isScaleGuidePlaying()) ? [follower.current, ...follower.trail] : [];
  showNotes(neck, view, { labels: state.labels, leftHanded: state.leftHanded, width: neck.clientWidth }, heard, state.listenEverywhere);
  showMatch(root, view, null, state.match);

  if (!listening) {
    readout.hidden = true;
    return;
  }

  if (chordMode) {
    return;
  }

  // Nothing heard yet: no readout. Listening is simply what the guide does, so it says nothing of it.
  if (follower.current === null) {
    readout.hidden = true;
    return;
  }

  const note = noteReadout(view, follower.current);
  setReadout(readout, note.html, note.state);
}

/**
 * A `tunerUpdate` for LISTEN: the played note (Scales) or a chroma reading (Chords). True when it
 * was the guide's, so the tuner need not log it while its own dialog is closed.
 */
export function onListenReading(host: ListenHost, payload: IncomingPayload): boolean {
  if (!sentListen?.enabled) {
    return false;
  }

  if (!guideVisible()) {
    syncListening(host);
    return true;
  }

  const view = host.view();

  if (!view) {
    return true;
  }

  if (view.mode !== "chord") {
    if (follower.push(payload.detected === true ? readingMidi(payload.noteName, payload.octave) : null)) {
      renderListen(host);
    }

    return true;
  }

  const chroma = Array.isArray(payload.chroma) && payload.chroma.length === 12 ? (payload.chroma as number[]) : null;
  const level = typeof payload.levelDb === "number" ? payload.levelDb : -120;

  if (!chroma) {
    return true;
  }

  // The guide strumming a chord box or playing a progression would otherwise match its own chord and step on.
  if (isScaleGuideSounding()) {
    strums.disarm(level);
    return true;
  }

  const strictness = host.settings().match;
  const match = level > StrumGate.QUIET_DB - 2 ? matchChord(chroma, view.tones[0]?.pitchClass ?? 0, view.tones, strictness) : null;

  if (strums.push(level, Boolean(match?.ok))) {
    onCleanChord(host);
    return true;
  }

  const signature = match ? `${Math.round(match.share * 100)}|${match.heard.join()}|${match.missing.join()}|${match.ok}` : "quiet";
  const root = element<HTMLElement>("scale-guide");

  if (root && signature !== matchSignature && performance.now() > flashUntil) {
    matchSignature = signature;
    showMatch(root, view, match, strictness);
  }

  return true;
}

/** A chord rang clean: a progression moves on (a new round after its last chord); a lone chord counts. */
function onCleanChord(host: ListenHost): void {
  const readout = element<HTMLElement>("sg-live");
  const progression = host.view()?.progression;
  flashUntil = performance.now() + 700;
  matchSignature = "";

  if (progression) {
    if (progression.step === progression.chords.length - 1) {
      round += 1;
    }

    if (readout) {
      setReadout(readout, "<b>✓ Clean</b>", "clean");
    }

    host.goStep(host.step() + 1);
    return;
  }

  cleanCount += 1;

  if (readout) {
    setReadout(readout, `<b>✓ Clean ×${cleanCount}</b>`, "clean");
  }
}

/** For tests: forget everything, as on a fresh page. */
export function resetListenForTests(): void {
  sentListen = null;
  follower.reset();
  strums.reset();
  matchSignature = "";
  flashUntil = 0;
  resetRound();
}
