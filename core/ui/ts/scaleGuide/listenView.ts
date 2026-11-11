/**
 * What LISTEN changes on the page (panel.ts decides when): the played note and its trail on the
 * neck, the readout beside the tabs, and in Chords mode the match meter and which tones were heard.
 * Each writes only what it is given, so the caller keeps DOM writes to the readings that change
 * something.
 */

import { escapeHtml } from "../utils.js";
import type { ChordMatch, Strictness } from "./listen.js";
import { STRICTNESS } from "./listen.js";
import { noteMarkersSvg, type NeckOptions } from "./neck.js";
import type { ScaleGuideView } from "./types.js";

const SHARP_NAMES = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
const FLAT_NAMES = ["C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"];
const HIT_CLASSES = ["sg-hit", "sg-hit-1", "sg-hit-2"];

/** A played note as the view spells it: its scale or chord tone when it is one. */
export function describeNote(view: ScaleGuideView, midi: number): { name: string; interval: string; inView: boolean } {
  const pitchClass = ((midi % 12) + 12) % 12;
  const tone = view.tones.find((t) => t.pitchClass === pitchClass);
  return tone
    ? { name: tone.name, interval: tone.interval, inView: true }
    : { name: (view.flats ? FLAT_NAMES : SHARP_NAMES)[pitchClass], interval: "", inView: false };
}

/**
 * Marks the note now (`notes[0]`) and the trail behind it on the neck; null entries are skipped.
 * `everywhere` marks every place the note is; without it only the places inside the chosen
 * position are marked, unless the note is nowhere in it.
 */
export function showNotes(neck: HTMLElement, view: ScaleGuideView, options: NeckOptions, notes: readonly (number | null)[], everywhere = true): void {
  neck.querySelectorAll<SVGGElement>(".sg-dot").forEach((dot) => dot.classList.remove(...HIT_CLASSES));
  const layer = neck.querySelector<SVGGElement>(".sg-live-layer");
  const markers: string[] = [];

  notes.forEach((midi, age) => {
    if (midi === null) {
      return;
    }

    const all = neck.querySelectorAll<SVGGElement>(`.sg-dot[data-midi="${midi}"]`);
    const inPosition = everywhere ? [] : Array.from(all).filter((dot) => !dot.classList.contains("sg-dot-dim"));
    const dots = inPosition.length > 0 ? inPosition : Array.from(all);
    dots.forEach((dot) => dot.classList.add(HIT_CLASSES[age]));

    if (dots.length === 0) {
      markers.push(noteMarkersSvg(view, options, midi, `sg-miss ${HIT_CLASSES[age]}`));
    }
  });

  if (layer) {
    layer.innerHTML = markers.join("");
  }
}

export function setReadout(readout: HTMLElement, html: string, state: "idle" | "in" | "out" | "clean"): void {
  readout.hidden = false;
  readout.className = `sg-live sg-live-${state}`;
  readout.innerHTML = html;
}

export function noteReadout(view: ScaleGuideView, midi: number): { html: string; state: "in" | "out" } {
  const note = describeNote(view, midi);
  const what = view.mode;
  return note.inView
    ? { html: `<b>${escapeHtml(note.name)}</b><span>${escapeHtml(note.interval)} of the ${what}</span>`, state: "in" }
    : { html: `<b>${escapeHtml(note.name)}</b><span>outside the ${what}</span>`, state: "out" };
}

/** The match meter, the tones heard or missing, and the readout, for one chroma reading (null: quiet). */
export function showMatch(root: HTMLElement, view: ScaleGuideView, match: ChordMatch | null, strictness: Strictness): void {
  const fill = root.querySelector<HTMLElement>("#sg-meter-fill");
  const threshold = root.querySelector<HTMLElement>("#sg-meter-threshold");
  const text = root.querySelector<HTMLElement>("#sg-match-text");
  const percent = match ? Math.round(match.share * 100) : 0;

  if (fill) {
    fill.style.width = `${percent}%`;
    fill.classList.toggle("ok", Boolean(match?.ok));
  }

  if (threshold) {
    threshold.style.left = `${STRICTNESS[strictness].share * 100}%`;
  }

  if (text) {
    text.textContent = match ? `Clean ${percent}%` : "Waiting for a strum";
  }

  root.querySelectorAll<HTMLElement>("#sg-info .sg-pill").forEach((pill, index) => {
    pill.classList.toggle("heard", Boolean(match?.heard[index]));
    pill.classList.toggle("missing", Boolean(match && !match.heard[index] && match.missing.includes(view.tones[index]?.pitchClass ?? -1)));
  });

  const readout = root.querySelector<HTMLElement>("#sg-live");

  if (!readout) {
    return;
  }

  if (match && match.missing.length > 0) {
    const names = view.tones.filter((tone) => match.missing.includes(tone.pitchClass)).map((tone) => tone.name);
    setReadout(readout, `<b>${escapeHtml(view.title)}</b><span>missing ${escapeHtml(names.join(" "))}</span>`, "out");
  } else {
    setReadout(readout, `<span>Play</span><b>${escapeHtml(view.title)}</b>`, "idle");
  }
}
