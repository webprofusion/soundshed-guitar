/**
 * The Scale Guide's Position row (panel.ts wires it up): All for the whole neck, then one segment
 * for each scale position, chord shape or arpeggio run the engine found.
 */

import { escapeHtml } from "../utils.js";
import type { ScaleGuideView } from "./types.js";

export function renderPositions(model: ScaleGuideView): string {
  const chordMode = model.mode === "chord";
  const runs = model.arpeggio?.runs ?? [];
  const count = chordMode ? (model.shapes?.length ?? 0) : model.mode === "arpeggio" ? runs.length : (model.positions?.length ?? 0);
  const segment = (position: number, label: string, title: string, hint = ""): string =>
    `<button type="button" class="sg-segment${model.position === position ? " active" : ""}" data-sg-position="${position}" aria-pressed="${model.position === position}" title="${escapeHtml(title)}">${escapeHtml(label)}${hint ? `<small>${escapeHtml(hint)}</small>` : ""}</button>`;
  const segments = [segment(0, "All", chordMode || model.mode === "arpeggio" ? "Every chord tone on the neck" : "The whole neck")];

  for (let position = 1; position <= count; position += 1) {
    const run = runs[position - 1];

    if (run) {
      segments.push(segment(position, String(position), `${run.name}, from fret ${run.fretFrom}`, run.start));
      continue;
    }

    const caged = chordMode ? (model.shapes?.[position - 1]?.caged ?? "") : "";
    segments.push(segment(position, String(position), caged ? `${caged} shape` : `${chordMode ? "Shape" : "Position"} ${position}`, caged));
  }

  return segments.join("");
}
