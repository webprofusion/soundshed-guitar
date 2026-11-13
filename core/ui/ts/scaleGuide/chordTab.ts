/**
 * The chord showing in Chords, written out as tab beside its boxes: strummed as one column, then
 * picked string by string from the lowest that sounds. The highest string is on top, as tab always
 * has it, × marks a muted string, and the root's frets are marked as the boxes mark them. Drawn
 * from the engine's shape (types.ts) as SVG markup, in the run tab's classes (runTab.ts).
 */

import { escapeHtml } from "../utils.js";
import type { ScaleGuideShape, ScaleGuideView } from "./types.js";

const SLOT = 30;
const LEFT = 30;
const RIGHT = 12;
const LABEL_Y = 12;
const TOP = 30;
const STRING_GAP = 15;
const NAME_X = 12;
const BAR_GAP = 14;
/** The gap a number leaves in its string, the way printed tab sits on its lines. */
const NUMBER_GAP = 18;

/** The chord the tab is of: the progression's current one, or the shape picked from the boxes. */
export interface CurrentChord {
  name: string;
  caption: string;
  rootPitchClass: number;
  shape: ScaleGuideShape | null;
}

export function currentChord(view: ScaleGuideView): CurrentChord | null {
  if (view.mode !== "chord") {
    return null;
  }

  const progression = view.progression;

  if (progression) {
    const chord = progression.chords[progression.step];
    return chord ? { name: chord.name, caption: chord.numeral, rootPitchClass: chord.rootPitchClass, shape: chord.shape } : null;
  }

  const shape = view.position > 0 ? (view.shapes?.[view.position - 1] ?? null) : null;
  return { name: view.title, caption: shape?.caged ? `${shape.caged} shape` : "", rootPitchClass: view.tones[0]?.pitchClass ?? -1, shape };
}

export function renderChordTabSvg(shape: ScaleGuideShape, view: ScaleGuideView, chord: Pick<CurrentChord, "name" | "rootPitchClass">): string {
  const strings = shape.frets.length;
  const picked = shape.frets.map((fret, string) => ({ fret, string })).filter((note) => note.fret >= 0);
  const strumX = LEFT + SLOT / 2;
  const pickFrom = LEFT + SLOT + BAR_GAP;
  const end = pickFrom + picked.length * SLOT;
  const width = end + RIGHT;
  const height = TOP + (strings - 1) * STRING_GAP + 10;
  const stringY = (string: number): number => TOP + (strings - 1 - string) * STRING_GAP;
  const isRoot = (string: number, fret: number): boolean => ((((view.strings[string]?.openMidi ?? 0) + fret) % 12) + 12) % 12 === chord.rootPitchClass;
  const parts: string[] = [];

  parts.push(`<text class="sg-tab-interval" x="${strumX}" y="${LABEL_Y}" text-anchor="middle">Strum</text>`);

  if (picked.length > 0) {
    parts.push(`<text class="sg-tab-interval" x="${pickFrom + 2}" y="${LABEL_Y}">Picked</text>`);
  }

  for (let string = 0; string < strings; string += 1) {
    const y = stringY(string);
    const pick = picked.findIndex((note) => note.string === string);
    // Each string is broken where a number sits on it: the strum column always, and its picked note.
    const gaps = [strumX, ...(pick >= 0 ? [pickFrom + pick * SLOT + SLOT / 2] : [])];
    let from = LEFT - 8;

    for (const x of gaps) {
      parts.push(`<line class="sg-tab-string" x1="${from}" x2="${x - NUMBER_GAP / 2}" y1="${y}" y2="${y}"/>`);
      from = x + NUMBER_GAP / 2;
    }

    parts.push(`<line class="sg-tab-string" x1="${from}" x2="${end}" y1="${y}" y2="${y}"/>`);
    parts.push(`<text class="sg-tab-name" x="${NAME_X}" y="${y + 4}" text-anchor="middle">${escapeHtml(view.strings[string]?.name ?? "")}</text>`);

    const fret = shape.frets[string] ?? -1;
    const root = fret >= 0 && isRoot(string, fret) ? " sg-tab-root" : "";
    parts.push(`<text class="sg-tab-fret${root}" x="${strumX}" y="${y + 4.5}" text-anchor="middle">${fret < 0 ? "×" : fret}</text>`);
  }

  picked.forEach((note, index) => {
    const root = isRoot(note.string, note.fret) ? " sg-tab-root" : "";
    parts.push(`<text class="sg-tab-fret${root}" x="${pickFrom + index * SLOT + SLOT / 2}" y="${stringY(note.string) + 4.5}" text-anchor="middle">${note.fret}</text>`);
  });

  const barX = LEFT + SLOT + BAR_GAP / 2;
  parts.push(`<line class="sg-tab-barline" x1="${barX}" x2="${barX}" y1="${stringY(strings - 1)}" y2="${stringY(0)}"/>`);
  parts.push(`<line class="sg-tab-barline sg-tab-barline-end" x1="${end}" x2="${end}" y1="${stringY(strings - 1)}" y2="${stringY(0)}"/>`);

  // Highest string first, as the tab reads.
  const spoken = shape.frets
    .map((fret, string) => `${view.strings[string]?.name ?? ""} ${fret < 0 ? "muted" : fret === 0 ? "open" : `fret ${fret}`}`)
    .reverse()
    .join(", ");
  return `<svg class="sg-tab-svg sg-chord-tab-svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="${escapeHtml(`${chord.name} as tab: ${spoken}`)}">${parts.join("")}</svg>`;
}

/** The card beside the boxes, shown in Chords only, with its row; a hint while the whole neck is showing. */
export function renderChordTabCard(card: HTMLElement | null, view: ScaleGuideView): void {
  if (!card) {
    return;
  }

  const chord = currentChord(view);
  const shown = chord !== null && (view.progression !== undefined || (view.shapes?.length ?? 0) > 0);
  card.toggleAttribute("hidden", !shown);
  card.parentElement?.toggleAttribute("hidden", !shown);

  if (!shown || !chord) {
    card.innerHTML = "";
    return;
  }

  const head = `<div class="sg-arp-tab-head"><span class="sg-summary-name">${escapeHtml(chord.name)}</span><span class="sg-arp-tab-frets">${escapeHtml(chord.caption)}</span></div>`;
  const body = chord.shape
    ? `<div class="sg-arp-tab-scroll">${renderChordTabSvg(chord.shape, view, chord)}</div>`
    : `<p class="sg-chord-tab-hint">${view.progression ? "No shape for this chord." : "Pick a shape to see it as tab."}</p>`;
  card.innerHTML = head + body;
}
