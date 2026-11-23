/**
 * The Scale Guide's LISTEN, wired to the panel through `ListenHost` (panel.ts): asks the engine to
 * listen whenever the guide is on screen (`tuner {action: "listen"}`; there is no switch), follows the
 * played note in Scales and Arpeggios mode, and in Chords mode turns chroma readings into clean strums that step a
 * progression on. listen.ts holds the logic, listenView.ts the page writes. What the engine says it
 * heard whatever the guide shows (the chord, the key) is heardView.ts's.
 */

import { postMessage } from "../bridge.js";
import type { IncomingPayload } from "../messages/types.js";
import { wheelRadius } from "./chordWheel.js";
import { heardChange, parseHeard, resetHeardForTests, showHeard, shownHeard } from "./heardView.js";
import { ChordEar, matchChord, NoteFollower, readingMidi, StrumGate } from "./listen.js";
import { noteReadout, setReadout, showMatch, showNotes } from "./listenView.js";
import { isScaleGuidePlaying, isScaleGuideSounding } from "./playback.js";
import type { ScaleGuideSettings, ScaleGuideView } from "./types.js";

export interface ListenHost {
  settings(): ScaleGuideSettings;
  view(): ScaleGuideView | null;
  step(): number;
  goStep(index: number): void;
  /** Applies a change to the guide's settings, as its own controls do. */
  change(patch: Partial<ScaleGuideSettings>): void;
}

let sentListen: { enabled: boolean; chroma: boolean } | null = null;
const follower = new NoteFollower();
const strums = new StrumGate();
const ear = new ChordEar();
let matchSignature = "";
let flashUntil = 0;
let round = 1;
let cleanCount = 0;
/** The guide roots whose Heard section answers clicks (a test's page is a new root). */
const heardBound = new WeakSet<HTMLElement>();

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

  // Each visit to the guide starts the ear at its middle.
  if (!enabled) {
    ear.reset();
  }

  matchSignature = "";
  const root = element<HTMLElement>("scale-guide");

  if (root) {
    showHeard(root, null, host.settings().mode);
  }

  renderListen(host);
}

/** A click on what was heard shows it in the guide. */
function bindHeard(host: ListenHost, root: HTMLElement): void {
  if (heardBound.has(root)) {
    return;
  }

  heardBound.add(root);
  root.addEventListener("click", (event) => {
    const what = (event.target as HTMLElement | null)?.closest<HTMLElement>("[data-sg-heard]")?.dataset.sgHeard;
    const patch = heardChange(shownHeard(), what);

    if (patch) {
      host.change(patch);
    }
  });
}

/** Draws LISTEN's state over the current view: the notes followed, or the chord meter. */
export function renderListen(host: ListenHost): void {
  const view = host.view();
  const root = element<HTMLElement>("scale-guide");
  const neck = element<HTMLElement>("sg-neck");
  const readout = element<HTMLElement>("sg-live");
  const listening = Boolean(sentListen?.enabled && view);
  const chordMode = view?.mode === "chord";
  element<HTMLElement>("sg-ear")?.toggleAttribute("hidden", !listening || !chordMode);
  const roundLabel = element<HTMLElement>("sg-round");

  if (roundLabel) {
    roundLabel.hidden = !listening || !view?.progression;
    roundLabel.textContent = `Round ${round}`;
  }

  if (!root || !neck || !readout || !view) {
    return;
  }

  bindHeard(host, root);
  showHeard(root, listening ? shownHeard() : null, view.mode);
  const state = host.settings();
  // The guide's own Play lights the notes it sounds: hearing them too would mark each one all over the neck.
  const heard = listening && !chordMode && (state.listenEverywhere || !isScaleGuidePlaying()) ? [follower.current, ...follower.trail] : [];
  showNotes(neck, view, { labels: state.labels, leftHanded: state.leftHanded, width: neck.clientWidth }, heard, state.listenEverywhere);
  showMatch(root, view, null, ear.rule, state.labels);

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

  const root = element<HTMLElement>("scale-guide");

  if (root) {
    showHeard(root, parseHeard(payload.heard), view.mode);
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

  const loud = level > StrumGate.QUIET_DB - 2;
  const match = loud ? matchChord(chroma, view.tones[0]?.pitchClass ?? 0, view.tones, ear.rule) : null;

  if (strums.push(level, Boolean(match?.ok))) {
    ear.landed();
    onCleanChord(host);
    return true;
  }

  const eased = ear.push(loud, Boolean(match?.ok));
  const signature = match ? `${match.spread.map((share) => Math.round(share * 40)).join()}|${match.missing.join()}|${match.ok}|${ear.tightness}` : "quiet";

  if (root && (eased || signature !== matchSignature) && performance.now() > flashUntil) {
    matchSignature = signature;
    showMatch(root, view, match, ear.rule, host.settings().labels);
  }

  return true;
}

/** A chord rang clean: a progression moves on (a new round after its last chord); a lone chord counts. */
function onCleanChord(host: ListenHost): void {
  const readout = element<HTMLElement>("sg-live");
  const progression = host.view()?.progression;
  flashUntil = performance.now() + 700;
  matchSignature = "";
  landedPulse();

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

/** The wheel's ring pulses as a chord lands, and shows the tighter target it now asks for. */
function landedPulse(): void {
  const wheel = document.getElementById("sg-wheel");

  if (!wheel) {
    return;
  }

  wheel.classList.remove("landed");
  // A layout read restarts the pulse when chords land close together.
  wheel.getBoundingClientRect();
  wheel.classList.add("landed");
  wheel.querySelector(".sg-wheel-target")?.setAttribute("r", String(Math.round(wheelRadius(ear.rule.share) * 10) / 10));
}

/** For tests: how tight the ear is now. */
export function earTightnessForTests(): number {
  return ear.tightness;
}

/** For tests: forget everything, as on a fresh page. */
export function resetListenForTests(): void {
  sentListen = null;
  follower.reset();
  strums.reset();
  ear.reset();
  matchSignature = "";
  flashUntil = 0;
  resetRound();
  resetHeardForTests();
}
