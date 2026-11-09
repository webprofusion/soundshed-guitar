/**
 * A run as tab, an arpeggio's or a scale position's: the frets in playing order, up and back, the pick strokes
 * above and each note's interval below. The highest string is on top, as tab always has it, and it
 * reads left to right for either hand. Drawn from the engine's run (types.ts) as SVG markup.
 */

import { escapeHtml } from "../utils.js";
import type { GuideRun, PickStroke, ScaleGuideView } from "./types.js";

const SLOT = 30;
const LEFT = 30;
const RIGHT = 12;
const STROKES_Y = 12;
const TOP = 34;
const STRING_GAP = 15;
const INTERVAL_GAP = 22;
const NAME_X = 12;
/** The gap a number leaves in its string, the way printed tab sits on its lines. */
const NUMBER_GAP = 18;

const STROKE_NAMES: Readonly<Record<PickStroke, string>> = {
  down: "Down-stroke",
  up: "Up-stroke",
  hammer: "Hammer-on",
  pull: "Pull-off",
};

/** The stroke marks: the usual staple for a down-stroke and V for an up-stroke, h and p for legato. */
function strokeMark(stroke: PickStroke, cx: number): string {
  switch (stroke) {
    case "down":
      return `<path class="sg-tab-stroke" d="M${cx - 5} ${STROKES_Y + 5}V${STROKES_Y - 5}H${cx + 5}V${STROKES_Y + 5}"/>`;
    case "up":
      return `<path class="sg-tab-stroke" d="M${cx - 5} ${STROKES_Y - 5}L${cx} ${STROKES_Y + 5}L${cx + 5} ${STROKES_Y - 5}"/>`;
    case "hammer":
      return `<text class="sg-tab-legato" x="${cx}" y="${STROKES_Y + 4}" text-anchor="middle">h</text>`;
    default:
      return `<text class="sg-tab-legato" x="${cx}" y="${STROKES_Y + 4}" text-anchor="middle">p</text>`;
  }
}

export function renderRunTabSvg(run: GuideRun, view: ScaleGuideView): string {
  const strings = view.strings.length;
  const width = LEFT + run.steps.length * SLOT + RIGHT;
  const stringY = (string: number): number => TOP + (strings - 1 - string) * STRING_GAP;
  const intervalY = stringY(0) + INTERVAL_GAP;
  const height = intervalY + 6;
  const parts: string[] = [];

  const slotX = (index: number): number => LEFT + index * SLOT + SLOT / 2;

  view.strings.forEach((string, index) => {
    const y = stringY(index);
    let from = LEFT - 8;

    run.steps.forEach((step, slot) => {
      if (run.notes[step.note]?.string === index) {
        parts.push(`<line class="sg-tab-string" x1="${from}" x2="${slotX(slot) - NUMBER_GAP / 2}" y1="${y}" y2="${y}"/>`);
        from = slotX(slot) + NUMBER_GAP / 2;
      }
    });

    parts.push(`<line class="sg-tab-string" x1="${from}" x2="${width - RIGHT + 4}" y1="${y}" y2="${y}"/>`);
    parts.push(`<text class="sg-tab-name" x="${NAME_X}" y="${y + 4}" text-anchor="middle">${escapeHtml(string.name)}</text>`);
  });

  const spoken: string[] = [];

  run.steps.forEach((step, index) => {
    const note = run.notes[step.note];

    if (!note) {
      return;
    }

    const cx = slotX(index);
    const cy = stringY(note.string);
    parts.push(strokeMark(step.stroke, cx));
    parts.push(`<text class="sg-tab-fret${note.root ? " sg-tab-root" : ""}" x="${cx}" y="${cy + 4.5}" text-anchor="middle">${note.fret}</text>`);
    parts.push(`<text class="sg-tab-interval${note.root ? " sg-tab-root" : ""}" x="${cx}" y="${intervalY}" text-anchor="middle">${escapeHtml(note.interval)}</text>`);
    spoken.push(`${note.name} fret ${note.fret} ${STROKE_NAMES[step.stroke]}`);
  });

  return `<svg class="sg-tab-svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="${escapeHtml(`${view.title}, ${run.name}: ${spoken.join(", ")}`)}">${parts.join("")}</svg>`;
}
