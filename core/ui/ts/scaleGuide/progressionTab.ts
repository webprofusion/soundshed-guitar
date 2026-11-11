/**
 * A progression's arpeggios as tab (docs/plans/scale-guide.md): the whole of it under the neck as
 * one diagram, a bar for each chord, running on to new lines at the card's width. A bar shows its
 * chord on the neck and plays when clicked, and the card's Slides switch joins each shape to the
 * next. The piece itself is the engine's; panel.ts wires the clicks.
 */

import { escapeHtml } from "../utils.js";
import { renderRunTabSvg } from "./runTab.js";
import type { ProgressionRun, ScaleGuideView } from "./types.js";

/** The width the tab is laid out for where the page has none to measure. */
export const FALLBACK_TAB_WIDTH = 900;

/** The card's contents: its heading with Slides and Play, then the tab at `width`. */
export function progressionTabMarkup(view: ScaleGuideView, whole: ProgressionRun, width: number): string {
  const playable = whole.steps.length > 0;
  const frets = !playable ? "" : whole.fretFrom === whole.fretTo ? `Fret ${whole.fretFrom}` : `Frets ${whole.fretFrom}–${whole.fretTo}`;
  const name = whole.name || "Progression";
  const flow = { bars: whole.bars, width: width > 0 ? width : FALLBACK_TAB_WIDTH, active: view.progression?.step ?? -1 };
  return (
    `<div class="sg-arp-tab-head"><span class="sg-summary-name">${escapeHtml(name)}</span><span class="sg-arp-tab-frets">${escapeHtml(frets)}</span>` +
    `<button type="button" class="sg-listen sg-tab-slides${whole.slides ? " active" : ""}" data-sg-arp-slides aria-pressed="${whole.slides}" title="Slide from each shape into the next, where they meet on one string">Slides</button>` +
    `<button type="button" class="sg-listen sg-play" data-sg-play aria-pressed="false"${playable ? "" : " disabled"} title="Hear the whole progression played through the current preset">Play</button></div>` +
    `<div class="sg-prog-tab">${renderRunTabSvg(whole, view, name, flow)}</div>`
  );
}
