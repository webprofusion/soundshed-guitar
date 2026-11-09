/**
 * The Scale Guide's progression markup (panel.ts wires it up): the Play picker's options, the steps
 * row, the chord-box strip and the editor's pills. Pure functions of the engine's view model, the
 * catalogue and the player's own progressions.
 */

import { escapeHtml } from "../utils.js";
import { renderChordBoxSvg } from "./chordBox.js";
import { DEGREE_NUMERALS, type UserProgression } from "./progressions.js";
import type { ScaleGuideCatalog, ScaleGuideView } from "./types.js";

export const NEW_PROGRESSION = "+new";

/** "bVII" as a chart writes it: "♭VII". */
export function displayNumeral(token: string): string {
  return token.replace(/^b/, "♭").replace(/^#/, "♯");
}

/** A built-in progression's numerals as tokens a player's copy can keep. */
export function numeralTokens(numerals: readonly string[]): string[] {
  return numerals.map((numeral) => numeral.replace(/^♭/, "b").replace(/^♯/, "#"));
}

export function progressionOptions(catalog: ScaleGuideCatalog | null, mine: readonly UserProgression[]): string {
  const builtIn = (catalog?.progressions ?? []).map(
    (p) => `<option value="b:${escapeHtml(p.id)}">${escapeHtml(p.name)} · ${escapeHtml(p.numerals.join(" "))}</option>`,
  );
  const own = mine.map((p) => `<option value="u:${escapeHtml(p.id)}">${escapeHtml(p.name)}</option>`);
  return [
    `<option value="">One chord</option>`,
    builtIn.length ? `<optgroup label="Progressions">${builtIn.join("")}</optgroup>` : "",
    own.length ? `<optgroup label="Saved progressions">${own.join("")}</optgroup>` : "",
    `<option value="${NEW_PROGRESSION}">+ Write a progression…</option>`,
  ].join("");
}

/** The editor's degree picker: each numeral with the note it lands on in this key. */
export function degreeOptions(keys: readonly string[], key: number): string {
  return DEGREE_NUMERALS.map((numeral, offset) => {
    const note = keys[(key + offset) % 12] ?? "";
    return `<option value="${offset}">${escapeHtml(displayNumeral(numeral))} · ${escapeHtml(note)}</option>`;
  }).join("");
}

export function qualityOptions(catalog: ScaleGuideCatalog | null): string {
  return (catalog?.chords ?? []).map((chord) => `<option value="${escapeHtml(chord.id)}">${escapeHtml(chord.name)}</option>`).join("");
}

export function stepsMarkup(view: ScaleGuideView): string {
  const progression = view.progression;

  if (!progression) {
    return "";
  }

  return progression.chords
    .map((chord, index) => {
      const state = index === progression.step ? " active" : index < progression.step ? " done" : "";
      return `<button type="button" class="sg-step${state}" data-sg-goto="${index}" aria-pressed="${index === progression.step}"><b>${escapeHtml(chord.name)}</b><small>${escapeHtml(chord.numeral)}</small></button>`;
    })
    .join("");
}

/** One chord box per step, each the shape stepping to it would show, the current one marked. */
export function stripMarkup(view: ScaleGuideView, leftHanded: boolean): string {
  const progression = view.progression;

  if (!progression) {
    return "";
  }

  return progression.chords
    .map((chord, index) => {
      const active = index === progression.step;
      const box = chord.shape
        ? renderChordBoxSvg(chord.shape, view, { leftHanded, chordName: chord.name, rootPitchClass: chord.rootPitchClass, caption: chord.numeral })
        : `<span class="sg-box-empty">No shape</span>`;
      return `<button type="button" class="sg-box sg-strip-box${active ? " active" : ""}" data-sg-goto="${index}" aria-pressed="${active}"><span class="sg-strip-name">${escapeHtml(chord.name)}</span>${box}</button>`;
    })
    .join("");
}

/** The chords being edited, named as the engine spelled them once it has answered. */
export function editorChordsMarkup(tokens: readonly string[], view: ScaleGuideView | null): string {
  if (tokens.length === 0) {
    return `<span class="sg-note">Add chords below.</span>`;
  }

  return tokens
    .map((token, index) => {
      const name = view?.progression?.chords[index]?.name ?? displayNumeral(token);
      return `<span class="sg-pill sg-editor-pill"><b>${escapeHtml(displayNumeral(token))}</b>${escapeHtml(name)}<button type="button" class="sg-pill-remove" data-sg-remove="${index}" aria-label="Remove ${escapeHtml(name)}">✕</button></span>`;
    })
    .join("");
}
