/**
 * The chord wheel: what LISTEN hears in Chords mode, drawn round the twelve notes with the chord's
 * root at the top. Each note's spoke grows with its share of what is heard, the chord's own notes
 * labelled and lit, the rest grey. The centre fills as the chord's notes take over the sound, and
 * the dashed ring is what a strum must fill to land: it tightens as chords land and eases when they
 * stop (ChordEar, listen.ts). Nano Q draws the same wheel (ChordWheelView, ScaleGuideDraw.h).
 */

import { escapeHtml } from "../utils.js";
import type { ChordMatch, EarRule } from "./listen.js";
import type { ScaleGuideLabels, ScaleGuideView } from "./types.js";

/** Where the spokes start, how far they reach, and where the labels sit. */
const SPOKE_FROM = 19;
const SPOKE_REACH = 17;
const LABEL_AT = 44;
/** The centre's largest radius; it starts filling at FILL_FLOOR of the sound. */
const FILL_RADIUS = 15;
const FILL_FLOOR = 0.3;
/** A note's share that draws its spoke at full length: a triad's notes hold about a third each. */
const FULL_SPOKE = 0.3;

/** The centre's radius for a share of the sound: what the fill and the target ring both use. */
export function wheelRadius(share: number): number {
  return FILL_RADIUS * Math.min(1, Math.max(0, (share - FILL_FLOOR) / (1 - FILL_FLOOR)));
}

/** A spoke's outer end for a note's share of the sound. */
export function spokeEnd(share: number): number {
  return SPOKE_FROM + SPOKE_REACH * Math.min(1, Math.max(0, share / FULL_SPOKE));
}

const round = (value: number): number => Math.round(value * 10) / 10;

/** The wheel for the view's chord, at rest: spokes as dots, the centre empty. Notes are named, or with the neck on any other labels, given as intervals. */
export function chordWheelMarkup(view: ScaleGuideView, labels: ScaleGuideLabels, rule: EarRule): string {
  const root = view.tones[0]?.pitchClass ?? 0;
  const parts: string[] = [`<circle class="sg-wheel-rim" r="${SPOKE_FROM + SPOKE_REACH + 2}" />`];

  for (let step = 0; step < 12; step += 1) {
    const pitchClass = (root + step) % 12;
    const tone = view.tones.find((t) => t.pitchClass === pitchClass);
    const kind = tone ? (step === 0 ? "sg-wheel-tone sg-wheel-root" : "sg-wheel-tone") : "sg-wheel-stray";
    parts.push(`<line class="sg-wheel-spoke ${kind}" data-pc="${pitchClass}" x1="0" y1="${-SPOKE_FROM}" x2="0" y2="${-SPOKE_FROM}" transform="rotate(${step * 30})" />`);

    if (tone) {
      const angle = (step * Math.PI) / 6;
      const x = round(Math.sin(angle) * LABEL_AT);
      const y = round(-Math.cos(angle) * LABEL_AT);
      const text = labels === "name" ? tone.name : tone.interval;
      parts.push(`<text class="sg-wheel-label${step === 0 ? " sg-wheel-root" : ""}" data-pc="${pitchClass}" x="${x}" y="${y}">${escapeHtml(text)}</text>`);
    }
  }

  parts.push(`<circle class="sg-wheel-fill" r="0" />`, `<circle class="sg-wheel-target" r="${round(wheelRadius(rule.share))}" />`);
  return parts.join("");
}

/** Moves the wheel to one reading (null: quiet, the spokes back to dots). */
export function updateChordWheel(svg: SVGSVGElement, match: ChordMatch | null, rule: EarRule): void {
  svg.querySelectorAll<SVGLineElement>(".sg-wheel-spoke").forEach((spoke) => {
    const pitchClass = Number(spoke.dataset.pc);
    spoke.setAttribute("y2", String(round(-spokeEnd(match?.spread[pitchClass] ?? 0))));
    spoke.classList.toggle("missing", Boolean(match?.missing.includes(pitchClass)));
  });
  svg.querySelectorAll<SVGTextElement>(".sg-wheel-label").forEach((label) => {
    label.classList.toggle("missing", Boolean(match?.missing.includes(Number(label.dataset.pc))));
  });
  svg.querySelector(".sg-wheel-fill")?.setAttribute("r", String(round(match ? wheelRadius(match.share) : 0)));
  svg.querySelector(".sg-wheel-target")?.setAttribute("r", String(round(wheelRadius(rule.share))));
  svg.classList.toggle("ok", Boolean(match?.ok));
}
