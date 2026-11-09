/**
 * The Scale Guide's neck: every scale or chord tone on a horizontal fretboard, drawn as SVG
 * markup from the engine's view model (types.ts). The lowest string is at the bottom, as in tab;
 * a left-handed neck runs from right to left. Colours come from CSS (scale-guide.css), so the
 * themes apply.
 *
 * One drawing unit is one CSS pixel once there is room for it, so the dots and labels stay the size
 * of the UI around them (and follow its zoom) and only the frets spread out to fill the width. The
 * drawing only scales down, in a space narrower than NARROWEST_DRAWING.
 */

import { escapeHtml } from "../utils.js";
import type { ScaleGuideDot, ScaleGuideLabels, ScaleGuideView } from "./types.js";

export interface NeckOptions {
  labels: ScaleGuideLabels;
  leftHanded: boolean;
  /** The space to fill, in CSS pixels. Left out, the narrowest drawing. */
  width?: number;
}

const NARROWEST_DRAWING = 1000;
const NAME_X = 14;
const OPEN_X = 46;
const NUT_X = 66;
const END_MARGIN = 8;
const TOP = 26;
const STRING_GAP = 34;
const DOT_RADIUS = 13;
const INLAYS = [3, 5, 7, 9, 15, 17, 19, 21];
const DOUBLE_INLAYS = [12, 24];
const NUMBERED = [3, 5, 7, 9, 12, 15, 17, 19, 21, 24];

/** How wide to draw the neck in this much space. */
export function neckDrawingWidth(available = 0): number {
  return Math.max(NARROWEST_DRAWING, Math.round(available));
}

/** Half real fret spacing, half even, so the top frets still hold a label. */
export function fretX(fret: number, frets: number, width = NARROWEST_DRAWING): number {
  if (fret <= 0) {
    return NUT_X;
  }
  const real = (1 - Math.pow(2, -fret / 12)) / (1 - Math.pow(2, -frets / 12));
  return NUT_X + (width - END_MARGIN - NUT_X) * (0.55 * real + 0.45 * (fret / frets));
}

/** Where a note on `fret` sits: mid-way between its fret wires, or left of the nut when open. */
export function noteX(fret: number, frets: number, width = NARROWEST_DRAWING): number {
  return fret <= 0 ? OPEN_X : (fretX(fret - 1, frets, width) + fretX(fret, frets, width)) / 2;
}

export function dotLabel(dot: ScaleGuideDot, labels: ScaleGuideLabels): string {
  switch (labels) {
    case "interval":
      return dot.interval;
    case "degree":
      return String(dot.degree);
    case "none":
      return "";
    case "order":
      return dot.order === undefined ? "" : String(dot.order);
    case "finger":
      return dot.finger === undefined ? "" : String(dot.finger);
    default:
      return dot.name;
  }
}

/** One decimal place: enough for a 1000-unit drawing, and a number, so markup stays plain. */
function fmt(value: number): number {
  return Math.round(value * 10) / 10;
}

export function renderNeckSvg(view: ScaleGuideView, options: NeckOptions): string {
  const strings = view.strings.length;
  const frets = view.frets;
  const width = neckDrawingWidth(options.width);
  const endX = width - END_MARGIN;
  const height = TOP + (strings - 1) * STRING_GAP + 54;
  const x = (value: number): number => (options.leftHanded ? width - value : value);
  const y = (string: number): number => TOP + (strings - 1 - string) * STRING_GAP;
  const middle = TOP + ((strings - 1) * STRING_GAP) / 2;
  const top = TOP - 18;
  const bottom = y(0) + 18;
  const parts: string[] = [];

  const boardLeft = Math.min(x(NUT_X), x(endX));
  parts.push(
    '<defs><linearGradient id="sg-wood" x1="0" y1="0" x2="0" y2="1"><stop offset="0" class="sg-wood-top"/><stop offset="0.5" class="sg-wood-mid"/><stop offset="1" class="sg-wood-bottom"/></linearGradient></defs>',
  );
  parts.push(`<rect class="sg-board" x="${fmt(boardLeft)}" y="${top}" width="${fmt(endX - NUT_X)}" height="${bottom - top}" rx="2" fill="url(#sg-wood)"/>`);

  for (const fret of INLAYS.filter((f) => f <= frets)) {
    parts.push(`<circle class="sg-inlay" cx="${fmt(x(noteX(fret, frets, width)))}" cy="${middle}" r="6"/>`);
  }

  for (const fret of DOUBLE_INLAYS.filter((f) => f <= frets)) {
    const cx = fmt(x(noteX(fret, frets, width)));
    const offset = ((strings - 1) * STRING_GAP) / 4;
    parts.push(`<circle class="sg-inlay" cx="${cx}" cy="${middle - offset}" r="6"/>`);
    parts.push(`<circle class="sg-inlay" cx="${cx}" cy="${middle + offset}" r="6"/>`);
  }

  for (let fret = 1; fret <= frets; fret += 1) {
    parts.push(`<rect class="sg-fret" x="${fmt(x(fretX(fret, frets, width)) - 1.25)}" y="${top}" width="2.5" height="${bottom - top}"/>`);
  }

  parts.push(`<rect class="sg-nut" x="${fmt(x(NUT_X) - 5)}" y="${top - 1}" width="10" height="${bottom - top + 2}" rx="2"/>`);

  if (view.capo > 0) {
    const cx = x(noteX(view.capo, frets, width));
    parts.push(`<rect class="sg-capo" x="${fmt(cx - 7)}" y="${top - 4}" width="14" height="${bottom - top + 8}" rx="5"/>`);
  }

  for (let s = 0; s < strings; s += 1) {
    // Thicker towards the low strings; the wound ones read lighter.
    const weight = 0.9 + 1.9 * (1 - s / Math.max(1, strings - 1));
    const wound = strings > 5 ? s < strings - 3 : s < strings - 2;
    const from = fmt(x(OPEN_X - 14));
    const to = fmt(x(endX));
    parts.push(`<line class="sg-string${wound ? " sg-string-wound" : ""}" x1="${from}" x2="${to}" y1="${y(s)}" y2="${y(s)}" stroke-width="${weight.toFixed(2)}"/>`);
    const nameX = fmt(x(NAME_X));
    parts.push(`<text class="sg-string-name" x="${nameX}" y="${y(s) + 5}" text-anchor="middle">${escapeHtml(view.strings[s]?.name ?? "")}</text>`);
    // The open string's place, so an empty one still reads as a string you could play.
    parts.push(`<circle class="sg-open-slot" cx="${fmt(x(OPEN_X))}" cy="${y(s)}" r="9"/>`);
  }

  for (const fret of NUMBERED.filter((f) => f <= frets)) {
    parts.push(`<text class="sg-fret-number" x="${fmt(x(noteX(fret, frets, width)))}" y="${bottom + 28}" text-anchor="middle">${fret}</text>`);
  }

  const shape = view.mode === "chord" && view.position > 0 ? view.shapes?.[view.position - 1] : undefined;
  const run = view.mode === "arpeggio" && view.position > 0 ? view.arpeggio?.runs[view.position - 1] : undefined;
  // Order and fingers number one run: with none chosen the neck has only its notes to name.
  const labels = !run && (options.labels === "order" || options.labels === "finger") ? "name" : options.labels;

  if (run) {
    // The way the run goes, under its dots.
    const points = run.notes.map((note) => `${fmt(x(noteX(note.fret, frets, width)))},${y(note.string)}`);
    parts.push(`<polyline class="sg-run-path" points="${escapeHtml(points.join(" "))}"/>`);
  }

  for (const dot of view.dots) {
    const muted = shape !== undefined && shape.frets[dot.string] < 0;

    if (muted && dot.fret <= view.capo) {
      continue;
    }

    const cx = fmt(x(noteX(dot.fret, frets, width)));
    const label = dotLabel(dot, labels);
    const classes = ["sg-dot", dot.root ? "sg-dot-root" : "", dot.inPosition ? "" : "sg-dot-dim"].filter(Boolean).join(" ");
    const size = label.length > 2 ? 10 : 12.5;
    parts.push(
      `<g class="${escapeHtml(classes)}" data-midi="${(view.strings[dot.string]?.openMidi ?? 0) + dot.fret}" data-string="${dot.string}" data-fret="${dot.fret}"><circle cx="${cx}" cy="${y(dot.string)}" r="${DOT_RADIUS}"/>` +
        (label ? `<text x="${cx}" y="${y(dot.string) + size * 0.36}" text-anchor="middle" font-size="${size}">${escapeHtml(label)}</text>` : "") +
        "</g>",
    );
  }

  if (shape) {
    shape.frets.forEach((fret, s) => {
      if (fret >= 0) {
        return;
      }
      const cx = x(OPEN_X);
      const cy = y(s);
      parts.push(`<path class="sg-mute" d="M${fmt(cx - 6)} ${cy - 6}l12 12m0 -12l-12 12"/>`);
    });
  }

  // LISTEN draws here: rings for a played note the scale or chord does not have (noteMarkersSvg).
  parts.push('<g class="sg-live-layer"></g>');

  return `<svg class="sg-neck-svg" viewBox="0 0 ${width} ${height}" role="img" aria-label="${escapeHtml(view.title)} on the neck">${parts.join("")}</svg>`;
}

/**
 * Rings where `midi` sounds on this neck, for LISTEN to show a note that has no dot (one outside
 * the scale or chord). `className` sets how it is drawn: the note now, or one fading behind it.
 */
export function noteMarkersSvg(view: ScaleGuideView, options: NeckOptions, midi: number, className: string): string {
  const strings = view.strings.length;
  const width = neckDrawingWidth(options.width);
  const x = (value: number): number => (options.leftHanded ? width - value : value);
  const y = (string: number): number => TOP + (strings - 1 - string) * STRING_GAP;
  const parts: string[] = [];

  view.strings.forEach((string, index) => {
    const fret = midi - string.openMidi;

    if (fret < view.capo || fret > view.frets) {
      return;
    }

    parts.push(`<circle class="${escapeHtml(className)}" cx="${fmt(x(noteX(fret, view.frets, width)))}" cy="${y(index)}" r="${DOT_RADIUS + 3}"/>`);
  });

  return parts.join("");
}
