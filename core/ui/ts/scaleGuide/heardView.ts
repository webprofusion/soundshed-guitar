/**
 * What LISTEN hears whatever the guide shows, as the engine works it out (`heard` in each
 * `tunerUpdate`, core/src/music/ListenAnalysis.h): in Chords the chord a strum most likely is and
 * the few before it, a tool for naming a chord, and in every mode the key the recent notes or chords
 * most likely belong to. A click shows it: the chord in Chords, the key's scale in Scales.
 */

import { escapeHtml } from "../utils.js";
import type { ScaleGuideMode, ScaleGuideSettings } from "./types.js";

export interface HeardKey {
  name: string;
  tonic: number;
  minor: boolean;
  confidence: number;
  /** The simplest scale on the tonic holding every note heard, when one does. */
  scale?: string;
  scaleName?: string;
  /** A key that fits nearly as well (usually the relative major or minor). */
  alternative?: string;
}

export interface HeardChord {
  name: string;
  root: number;
  id: string;
  /** Still being heard; false once it has stopped and is only the last chord heard. */
  ringing: boolean;
}

export interface Heard {
  key: HeardKey | null;
  chord: HeardChord | null;
  recent: string[];
}

const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null;

function parseKey(value: unknown): HeardKey | null {
  if (!isRecord(value) || typeof value.name !== "string" || typeof value.tonic !== "number") {
    return null;
  }

  return {
    name: value.name,
    tonic: value.tonic,
    minor: value.minor === true,
    confidence: typeof value.confidence === "number" ? value.confidence : 0,
    scale: typeof value.scale === "string" ? value.scale : undefined,
    scaleName: typeof value.scaleName === "string" ? value.scaleName : undefined,
    alternative: typeof value.alternative === "string" ? value.alternative : undefined,
  };
}

function parseChord(value: unknown): HeardChord | null {
  if (!isRecord(value) || typeof value.name !== "string" || typeof value.root !== "number" || typeof value.id !== "string") {
    return null;
  }

  return { name: value.name, root: value.root, id: value.id, ringing: value.ringing === true };
}

/** A `tunerUpdate`'s `heard`, or null when it has none. */
export function parseHeard(value: unknown): Heard | null {
  if (!isRecord(value)) {
    return null;
  }

  const recent = Array.isArray(value.recent) ? value.recent.filter((name): name is string => typeof name === "string") : [];
  return { key: parseKey(value.key), chord: parseChord(value.chord), recent };
}

/** The change a click on what was heard makes: the chord on its own in Chords, the key's scale in Scales. */
export function heardChange(heard: Heard | null, what: string | undefined): Partial<ScaleGuideSettings> | null {
  if (what === "chord" && heard?.chord) {
    return { mode: "chord", key: heard.chord.root, chord: heard.chord.id, progression: "" };
  }

  if (what === "key" && heard?.key) {
    return { mode: "scale", key: heard.key.tonic, scale: heard.key.scale ?? (heard.key.minor ? "minor" : "major") };
  }

  return null;
}

let shown: Heard | null = null;
let signature = "";

/** What was last shown, for a click on it. */
export function shownHeard(): Heard | null {
  return shown;
}

function chordMarkup(chord: HeardChord, recent: readonly string[]): string {
  const trail = recent.map((name) => `<span class="sg-heard-before">${escapeHtml(name)}</span>`).join("");
  const state = chord.ringing ? "Hearing" : "Last heard";
  return (
    `<span class="sg-heard-chords">` +
    `<button type="button" class="sg-heard-chord${chord.ringing ? " sg-heard-ringing" : ""}" data-sg-heard="chord" title="${escapeHtml(`${state} ${chord.name}: show it on the neck`)}">${escapeHtml(chord.name)}</button>` +
    trail +
    `</span>`
  );
}

function keyMarkup(key: HeardKey, mode: ScaleGuideMode): string {
  const fits = key.scaleName ? `<span>${escapeHtml(key.scaleName)} fits</span>` : "";
  const or = key.alternative ? `<span class="sg-heard-or">or ${escapeHtml(key.alternative)}</span>` : "";
  const action = mode === "scale" ? "show its scale" : "show its scale in Scales";
  return (
    `<span class="sg-heard-key-row">` +
    `<button type="button" class="sg-heard-key" data-sg-heard="key" title="${escapeHtml(`${key.name}: ${action}`)}"><span class="sg-label">Key</span><b>${escapeHtml(key.name)}</b></button>` +
    fits +
    or +
    `</span>`
  );
}

/**
 * Draws what was heard into the summary's Heard section, hidden while nothing has been. Only a
 * change is written, since readings come every 43 ms.
 */
export function showHeard(root: HTMLElement, heard: Heard | null, mode: ScaleGuideMode): void {
  const section = root.querySelector<HTMLElement>("#sg-heard");
  const body = root.querySelector<HTMLElement>("#sg-heard-body");
  const chord = mode === "chord" ? (heard?.chord ?? null) : null;
  const key = heard?.key ?? null;
  const next = JSON.stringify({ chord, key, recent: chord ? heard?.recent : [], mode });
  shown = heard;

  if (!section || !body || next === signature) {
    return;
  }

  signature = next;
  section.hidden = !chord && !key;
  body.innerHTML = (chord ? chordMarkup(chord, heard?.recent ?? []) : "") + (key ? keyMarkup(key, mode) : "");
}

/** For tests and a fresh page: nothing shown. */
export function resetHeardForTests(): void {
  shown = null;
  signature = "";
}
