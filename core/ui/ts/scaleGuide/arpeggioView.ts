/**
 * The Scale Guide's Arpeggios tab (docs/plans/scale-guide.md): its style and string choices, and the
 * card that writes a chosen arpeggio run or scale position out as tab. The runs themselves are the
 * engine's; panel.ts asks for them and calls these to show them.
 */

import { escapeHtml } from "../utils.js";
import { renderRunTabSvg } from "./runTab.js";
import type { ArpeggioStyle, ScaleGuideLabels, ScaleGuideSettings, ScaleGuideView } from "./types.js";

/** The arpeggio-only labels: other views show notes in their place. */
export function effectiveLabels(settings: ScaleGuideSettings): ScaleGuideLabels {
  const runOnly = settings.labels === "order" || settings.labels === "finger";
  return runOnly && settings.mode !== "arpeggio" ? "name" : settings.labels;
}

function pressed(button: HTMLElement, on: boolean): void {
  button.classList.toggle("active", on);
  button.setAttribute("aria-pressed", String(on));
}

/** The style and strings row, shown on the Arpeggios tab only. A box has no string choice. */
export function renderArpeggioControls(root: HTMLElement, settings: ScaleGuideSettings, view: ScaleGuideView | null): void {
  const showing = settings.mode === "arpeggio";
  root.querySelector<HTMLElement>("#sg-arp-row")?.toggleAttribute("hidden", !showing);
  root.querySelectorAll<HTMLElement>("[data-sg-labels-arp]").forEach((button) => button.toggleAttribute("hidden", !showing));

  if (!showing) {
    return;
  }

  root.querySelectorAll<HTMLElement>("[data-sg-arp-style]").forEach((button) => pressed(button, button.dataset.sgArpStyle === settings.arpeggioStyle));
  root.querySelector<HTMLElement>("#sg-arp-strings-field")?.toggleAttribute("hidden", settings.arpeggioStyle === "box");
  const strings = root.querySelector<HTMLElement>("#sg-arp-strings");

  if (strings && view?.arpeggio) {
    const { stringChoices, strings: chosen } = view.arpeggio;
    strings.innerHTML = stringChoices
      .map((count) => `<button type="button" class="sg-segment${count === chosen ? " active" : ""}" data-sg-arp-strings="${count}" aria-pressed="${count === chosen}">${count}</button>`)
      .join("");
  }
}

/** The run written out as tab, or nothing while the whole neck is showing. */
export function renderRunCard(card: HTMLElement, view: ScaleGuideView): void {
  const run = view.run;
  card.toggleAttribute("hidden", !run);

  if (!run) {
    card.innerHTML = "";
    return;
  }

  const frets = run.fretFrom === run.fretTo ? `Fret ${run.fretFrom}` : `Frets ${run.fretFrom}–${run.fretTo}`;
  card.innerHTML =
    `<div class="sg-arp-tab-head"><span class="sg-summary-name">${escapeHtml(run.name)}</span><span class="sg-arp-tab-frets">${escapeHtml(frets)}</span>` +
    `<button type="button" class="sg-listen sg-play sg-tab-play" data-sg-play aria-pressed="false" title="Hear this run played through the current preset">Play</button></div>` +
    `<div class="sg-arp-tab-scroll">${renderRunTabSvg(run, view)}</div>`;
}

export function isArpeggioStyle(value: string | undefined): value is ArpeggioStyle {
  return value === "sweep" || value === "two" || value === "box";
}
