/**
 * A run as tab, an arpeggio's or a scale position's: the frets in playing order, up and back, the pick strokes
 * above and each note's interval below. The highest string is on top, as tab always has it, and it
 * reads left to right for either hand. Drawn from the engine's run (types.ts) as SVG markup.
 *
 * A progression's arpeggios are one long run (progressionTab.ts). With a `flow` it is drawn as one
 * diagram that runs on to new lines at the width given: a bar line after each chord, the chord's
 * name over its first note, and the string names once at the start of each line. Each chord's part
 * of a line is a group the page can click.
 */

import { escapeHtml } from "../utils.js";
import type { ArpeggioNote, GuideRun, PickStroke, ProgressionBar, ScaleGuideView } from "./types.js";

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
/** A slide's slanted line beside its number: how far it runs and how much it rises. */
const SLIDE_LENGTH = 12;
const SLIDE_RISE = 4;
/** In a flowed tab: the room a bar line takes, the row the chord names sit in, and the gap between lines. */
const BAR_GAP = 14;
const CHORD_ROW = 20;
const LINE_GAP = 12;
/** The fewest notes a line holds, however narrow the page. */
const FEWEST_SLOTS = 6;

const STROKE_NAMES: Readonly<Record<PickStroke, string>> = {
  down: "Down-stroke",
  up: "Up-stroke",
  hammer: "Hammer-on",
  pull: "Pull-off",
  slide: "Slide",
};

/** How a progression's tab is laid out: its bars, the width to fill, and the chord showing on the neck. */
export interface TabFlow {
  bars: readonly ProgressionBar[];
  width: number;
  active: number;
}

/** One chord's stretch of one line: a bar that does not fit a line carries on as another. */
interface Segment {
  bar: ProgressionBar | null;
  line: number;
  from: number;
  to: number;
  /** The first of the bar's segments carries its name. */
  first: boolean;
  cells: Array<{ step: number; x: number }>;
}

function legatoMark(text: string, cx: number, y: number): string {
  return `<text class="sg-tab-legato" x="${cx}" y="${y + 4}" text-anchor="middle">${text}</text>`;
}

/** The stroke marks: the usual staple for a down-stroke and V for an up-stroke, h, p and sl. for the fretting hand. */
function strokeMark(stroke: PickStroke, cx: number, y: number): string {
  switch (stroke) {
    case "down":
      return `<path class="sg-tab-stroke" d="M${cx - 5} ${y + 5}V${y - 5}H${cx + 5}V${y + 5}"/>`;
    case "up":
      return `<path class="sg-tab-stroke" d="M${cx - 5} ${y - 5}L${cx} ${y + 5}L${cx + 5} ${y - 5}"/>`;
    case "hammer":
      return legatoMark("h", cx, y);
    case "slide":
      return legatoMark("sl.", cx, y);
    default:
      return legatoMark("p", cx, y);
  }
}

/** Half a slide: the line into a number (`side` -1) or out of one (1), rising when the fret does. */
function slideLine(cx: number, cy: number, side: -1 | 1, rising: boolean): string {
  const near = cx + side * (NUMBER_GAP / 2 - 1);
  const far = near + side * SLIDE_LENGTH;
  const lift = (rising ? -1 : 1) * side * SLIDE_RISE;
  return `<line class="sg-tab-slide" x1="${near}" y1="${cy - lift}" x2="${far}" y2="${cy + lift}"/>`;
}

/** Where every step goes: one segment for a plain run, or the bars flowed on to as many lines as they need. */
function layOut(stepCount: number, flow: TabFlow | undefined): Segment[] {
  if (!flow) {
    return [{ bar: null, line: 0, from: LEFT, to: LEFT + stepCount * SLOT, first: true, cells: Array.from({ length: stepCount }, (_, step) => ({ step, x: LEFT + step * SLOT + SLOT / 2 })) }];
  }

  const edge = Math.max(flow.width - RIGHT, LEFT + FEWEST_SLOTS * SLOT + BAR_GAP);
  const segments: Segment[] = [];
  let line = 0;
  let x = LEFT;

  for (const bar of flow.bars) {
    // A chord with no run keeps a short bar, so the progression still reads whole.
    const slots = Math.max(bar.count, 2);

    // Keep a bar on one line where a line can hold it.
    if (x > LEFT && x + slots * SLOT + BAR_GAP > edge && LEFT + slots * SLOT + BAR_GAP <= edge) {
      line += 1;
      x = LEFT;
    }

    let segment: Segment = { bar, line, from: x, to: x, first: true, cells: [] };

    for (let slot = 0; slot < slots; slot += 1) {
      if (x > LEFT && x + SLOT + BAR_GAP > edge) {
        segments.push(segment);
        line += 1;
        x = LEFT;
        segment = { bar, line, from: x, to: x, first: false, cells: [] };
      }

      if (slot < bar.count) {
        segment.cells.push({ step: bar.from + slot, x: x + SLOT / 2 });
      }

      x += SLOT;
      segment.to = x;
    }

    segments.push(segment);
    x += BAR_GAP;
  }

  return segments;
}

export function renderRunTabSvg(run: Pick<GuideRun, "notes" | "steps">, view: ScaleGuideView, label: string, flow?: TabFlow): string {
  const noteAt = (index: number): ArpeggioNote | undefined => run.notes[run.steps[index]?.note ?? -1];
  const segments = layOut(run.steps.length, flow);
  const lines = (segments[segments.length - 1]?.line ?? 0) + 1;
  const strings = view.strings.length;
  const head = flow ? CHORD_ROW : 0;
  const intervalRow = head + TOP + (strings - 1) * STRING_GAP + INTERVAL_GAP;
  const lineHeight = intervalRow + 6;
  const lineTop = (line: number): number => line * (lineHeight + LINE_GAP);
  const stringY = (line: number, string: number): number => lineTop(line) + head + TOP + (strings - 1 - string) * STRING_GAP;
  const lineEnd = (line: number): number => Math.max(...segments.filter((segment) => segment.line === line).map((segment) => segment.to)) + (flow ? BAR_GAP / 2 : RIGHT - 8);
  const width = Math.max(...Array.from({ length: lines }, (_, line) => lineEnd(line))) + (flow ? RIGHT : 8);
  const height = lineTop(lines - 1) + lineHeight;
  const parts: string[] = [];
  const spoken: string[] = [];

  for (let line = 0; line < lines; line += 1) {
    const cells = segments.filter((segment) => segment.line === line).flatMap((segment) => segment.cells);

    view.strings.forEach((string, index) => {
      const y = stringY(line, index);
      let from = LEFT - 8;

      for (const cell of cells) {
        if (noteAt(cell.step)?.string === index) {
          parts.push(`<line class="sg-tab-string" x1="${from}" x2="${cell.x - NUMBER_GAP / 2}" y1="${y}" y2="${y}"/>`);
          from = cell.x + NUMBER_GAP / 2;
        }
      }

      parts.push(`<line class="sg-tab-string" x1="${from}" x2="${lineEnd(line)}" y1="${y}" y2="${y}"/>`);
      parts.push(`<text class="sg-tab-name" x="${NAME_X}" y="${y + 4}" text-anchor="middle">${escapeHtml(string.name)}</text>`);
    });
  }

  segments.forEach((segment, index) => {
    const top = lineTop(segment.line);
    const bar = segment.bar;
    const inner: string[] = [];

    if (bar) {
      const where = bar.run ? `${bar.run}, ${bar.fretFrom === bar.fretTo ? `fret ${bar.fretFrom}` : `frets ${bar.fretFrom}–${bar.fretTo}`}` : "no run in this style";
      inner.push(`<title>${escapeHtml(`${bar.name}: ${where}. Click to show it on the neck and hear it.`)}</title>`);
      inner.push(`<rect class="sg-tab-bar-hit" x="${segment.from - 4}" y="${top + 1}" width="${segment.to - segment.from + 8}" height="${lineHeight - 2}" rx="6"/>`);

      if (segment.first) {
        inner.push(
          `<text class="sg-tab-chord" x="${segment.from + 4}" y="${top + 14}">${escapeHtml(bar.name)}<tspan class="sg-tab-chord-numeral" dx="6">${escapeHtml(bar.numeral)}</tspan></text>`,
        );
      }

      if (bar.count === 0) {
        inner.push(`<text class="sg-tab-interval" x="${segment.from + SLOT}" y="${top + intervalRow}" text-anchor="middle">no run</text>`);
      }
    }

    for (const cell of segment.cells) {
      const step = run.steps[cell.step];
      const note = noteAt(cell.step);

      if (!step || !note) {
        continue;
      }

      const cy = stringY(segment.line, note.string);
      const before = noteAt(cell.step - 1);
      const after = run.steps[cell.step + 1]?.stroke === "slide" ? noteAt(cell.step + 1) : undefined;
      const root = note.root ? " sg-tab-root" : "";
      inner.push(strokeMark(step.stroke, cell.x, top + head + STROKES_Y));

      if (step.stroke === "slide" && before) {
        inner.push(slideLine(cell.x, cy, -1, note.fret > before.fret));
      }

      if (after) {
        inner.push(slideLine(cell.x, cy, 1, after.fret > note.fret));
      }

      inner.push(`<text class="sg-tab-fret${root}" data-sg-tab-step="${cell.step}" x="${cell.x}" y="${cy + 4.5}" text-anchor="middle">${note.fret}</text>`);
      inner.push(`<text class="sg-tab-interval${root}" x="${cell.x}" y="${top + intervalRow}" text-anchor="middle">${escapeHtml(note.interval)}</text>`);
      spoken.push(`${note.name} fret ${note.fret} ${STROKE_NAMES[step.stroke]}`);
    }

    if (!bar) {
      parts.push(...inner);
      return;
    }

    parts.push(`<g class="sg-tab-bar${bar.chord === flow?.active ? " active" : ""}" data-sg-goto="${bar.chord}">${inner.join("")}</g>`);

    // The bar line, where the chord ends: not where it only runs on to the next line.
    if (segments[index + 1]?.bar !== bar) {
      const x = segment.to + BAR_GAP / 2;
      const last = index === segments.length - 1;
      const from = stringY(segment.line, strings - 1);
      const to = stringY(segment.line, 0);
      parts.push(`<line class="sg-tab-barline${last ? " sg-tab-barline-end" : ""}" x1="${x}" x2="${x}" y1="${from}" y2="${to}"/>`);
    }
  });

  return `<svg class="sg-tab-svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="${escapeHtml(`${label}: ${spoken.join(", ")}`)}">${parts.join("")}</svg>`;
}
