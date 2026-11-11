/**
 * The Scale Guide in Jam → Scales (docs/plans/scale-guide.md): the controls, and the neck and chord
 * boxes the engine's view model is drawn into. Every change is saved to the `scaleGuide.view` app
 * setting and sent as a `scaleGuide` request; the answer (`scaleGuideView`) carries the request's
 * id, and only the latest is drawn, so a fast run of clicks never paints an older answer last.
 */

import { postMessage } from "../bridge.js";
import type { IncomingPayload } from "../messages/types.js";
import { escapeHtml } from "../utils.js";
import { effectiveLabels, isArpeggioStyle, renderRunCard, renderArpeggioControls, watchRunCard } from "./arpeggioView.js";
import { renderChordBoxSvg } from "./chordBox.js";
import { onListenReading, renderListen, resetListenForTests, resetRound, syncListening, type ListenHost } from "./listenController.js";
import {
  markSummaryPlayable,
  onScaleGuidePlaybackEnded,
  bindPlayVolume,
  followProgressionWith,
  playScaleGuideBar,
  playScaleGuideChord,
  playScaleGuideNote,
  resetPlaybackForTests,
  showPlayVolume,
  showScaleGuidePlaying,
  stopScaleGuidePlayback,
  toggleScaleGuidePlay,
} from "./playback.js";
import { neckDrawingWidth, renderNeckSvg } from "./neck.js";
import { renderPositions } from "./positionsView.js";
import { MAX_PROGRESSION_CHORDS, loadProgressions, newProgressionId, romanToken, saveProgressions, type UserProgression } from "./progressions.js";
import { NEW_PROGRESSION, degreeOptions, editorChordsMarkup, numeralTokens, progressionOptions, qualityOptions, stepsMarkup, stripMarkup } from "./progressionView.js";
import { bindSetupMenu, instrumentLabel, showSetupChoices } from "./setupMenu.js";
import { loadSettings, MAX_CAPO, saveSettings, viewRequest, type ProgressionRequest } from "./state.js";
import type { ScaleGuideCatalog, ScaleGuideLabels, ScaleGuideMode, ScaleGuideSettings, ScaleGuideView } from "./types.js";

const FALLBACK_KEYS = ["C", "C♯/D♭", "D", "D♯/E♭", "E", "F", "F♯/G♭", "G", "G♯/A♭", "A", "A♯/B♭", "B"];
const INSTRUMENT_GROUPS: ReadonlyArray<[string, string]> = [
  ["guitar", "Guitar"],
  ["bass", "Bass"],
  ["ukulele", "Ukulele"],
];
/** What a diatonic chord's quality is called in the chord list. */
const DIATONIC_CHORDS: Readonly<Record<string, string>> = { maj: "maj", m: "m", dim: "dim", aug: "aug" };

let settings: ScaleGuideSettings | null = null;
let catalog: ScaleGuideCatalog | null = null;
let view: ScaleGuideView | null = null;
let latestRequestId = 0;
let catalogRequested = false;
let pickersBuilt = false;
let bound = false;
/** The width the neck was last drawn at, so a resize that changes nothing costs nothing. */
let drawnNeckWidth = 0;
/** The progression step showing; the engine wraps it and says where it landed. */
let step = 0;
/** In Arpeggios: the progression chord whose run the player chose; the others follow from it. */
let anchorStep = 0;
/** The shape to lead from, sent once with the next request after a step. */
let pendingPrevious: number[] | undefined;
/** A progression being written or changed: `id` null for a new one (or a built-in's copy). */
let draft: { id: string | null; name: string; chords: string[] } | null = null;
let userProgressions: UserProgression[] | null = null;
const listenHost: ListenHost = {
  settings: () => current(),
  view: () => view,
  step: () => step,
  goStep: (index) => goStep(index),
};

/** A `tunerUpdate`: true when it was for the guide's LISTEN (listenController.ts). */
export function onScaleGuideReading(payload: IncomingPayload): boolean {
  return onListenReading(listenHost, payload);
}

function element<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function current(): ScaleGuideSettings {
  settings ??= loadSettings();
  return settings;
}

function mine(): UserProgression[] {
  userProgressions ??= loadProgressions();
  return userProgressions;
}

/** What the request adds in Chords and Arpeggios: the draft being edited, or the chosen progression. */
function progressionRequest(): ProgressionRequest {
  const state = current();
  const previousFrets = pendingPrevious;
  pendingPrevious = undefined;

  if (state.mode === "scale") {
    return {};
  }

  const where = state.mode === "arpeggio" ? { step, anchorStep } : { step, previousFrets };

  if (draft) {
    return draft.chords.length ? { ...where, progressionChords: [...draft.chords], progressionName: draft.name, step: Math.min(step, draft.chords.length - 1) } : {};
  }

  if (state.progression.startsWith("b:")) {
    return { ...where, progression: state.progression.slice(2) };
  }

  const own = state.progression.startsWith("u:") ? mine().find((p) => p.id === state.progression.slice(2)) : undefined;
  return own ? { ...where, progressionChords: [...own.chords], progressionName: own.name } : {};
}

/** Steps a progression; the next chord takes the shape nearest the one showing, or `frets` when given. */
function goStep(index: number, frets?: number[]): void {
  const progression = view?.progression;

  if (!view || !progression || progression.chords.length === 0) {
    return;
  }

  const count = progression.chords.length;
  const showing = view.position > 0 ? view.shapes?.[view.position - 1] : undefined;
  pendingPrevious = frets ?? showing?.frets;
  step = ((index % count) + count) % count;
  requestView();
}

/** Draws the guide into its section; called whenever Jam → Scales is shown. */
export function renderScaleGuidePanel(): void {
  const root = element<HTMLElement>("scale-guide");

  if (!root) {
    return;
  }

  if (!bound) {
    bindEvents(root);
    watchNeckWidth();
    watchRunCard(() => view);
    bound = true;
  }

  if (!catalog && !catalogRequested) {
    catalogRequested = true;
    postMessage({ type: "scaleGuide", action: "catalog" });
  }

  renderControls();
  requestView();
  syncListening(listenHost);
}

function requestView(): void {
  latestRequestId += 1;
  postMessage({ type: "scaleGuide", action: "view", requestId: latestRequestId, request: viewRequest(current(), progressionRequest()) });
}

/** Applies a change: saved, drawn and asked for again. A new scale, chord, tuning or mode starts on the whole neck; arpeggios on their first run, since a run is what that tab shows. */
function change(patch: Partial<ScaleGuideSettings>): void {
  const before = current();
  const next = { ...before, ...patch };
  const reshaped = ["mode", "scale", "chord", "progression", "tuning", "arpeggioStyle", "arpeggioStrings"].some(
    (key) => key in patch && patch[key as keyof ScaleGuideSettings] !== before[key as keyof ScaleGuideSettings],
  );

  if (reshaped && !("position" in patch)) {
    next.position = next.mode === "arpeggio" ? 1 : 0;
  }

  if (next.mode !== before.mode || next.progression !== before.progression) {
    step = 0;
    anchorStep = 0;
    resetRound();
    draft = next.mode === before.mode ? draft : null;
  }

  settings = next;
  saveSettings(next);
  renderControls();

  // Show the chosen position at once; the engine's answer then dims the rest of the neck.
  if ("position" in patch) {
    document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-position]").forEach((button) => {
      const chosen = Number(button.dataset.sgPosition) === next.position;
      button.classList.toggle("active", chosen);
      button.setAttribute("aria-pressed", String(chosen));
    });
  }

  if ("listen" in patch || "mode" in patch) {
    syncListening(listenHost);
  }

  // Labels, handedness and LISTEN's settings only change how the same model is drawn.
  const redrawOnly = Object.keys(patch).every((key) => ["labels", "leftHanded", "listen", "match", "listenEverywhere", "playVolume"].includes(key));

  if (redrawOnly && view) {
    renderView();
    return;
  }

  // What is sounding is no longer what is showing.
  stopScaleGuidePlayback();
  requestView();
}

export function onScaleGuideCatalog(payload: IncomingPayload): void {
  if (!Array.isArray(payload.scales) || !Array.isArray(payload.tunings)) {
    return;
  }

  catalog = payload as unknown as ScaleGuideCatalog;
  pickersBuilt = false;
  renderControls();
}

export function onScaleGuideView(payload: IncomingPayload): void {
  if (payload.requestId !== latestRequestId || typeof payload.view !== "object" || payload.view === null) {
    return;
  }

  view = payload.view as ScaleGuideView;
  step = view.progression?.step ?? step;

  // The engine falls back to the whole neck for a position the new scale or tuning lacks. In a
  // progression's arpeggios the choice is the anchor chord's run, not the one showing.
  const chosen = view.progression?.anchorPosition ?? view.position;
  anchorStep = view.progression?.anchorStep ?? 0;

  if (settings && settings.position !== chosen) {
    settings = { ...settings, position: chosen };
  }

  renderView();
}

function togglePlay(action: "play" | "playScale"): void {
  toggleScaleGuidePlay(action, viewRequest(current(), progressionRequest()));
}

function playShape(frets: number[] | undefined): void {
  playScaleGuideChord(viewRequest(current(), progressionRequest()), frets);
}

function bindEvents(root: HTMLElement): void {
  root.addEventListener("click", (event) => {
    const dot = (event.target as Element | null)?.closest(".sg-dot");

    if (dot) {
      playScaleGuideNote(Number(dot.getAttribute("data-midi")));
      return;
    }

    const target = (event.target as HTMLElement | null)?.closest<HTMLElement>(
      "[data-sg-mode],[data-sg-labels],[data-sg-position],[data-sg-chord-root],[data-sg-goto],[data-sg-step],[data-sg-remove],[data-sg-match],[data-sg-arp-style],[data-sg-arp-strings],[data-sg-arp-slides],[data-sg-play],[data-sg-play-scale],#sg-listen",
    );

    if (!target || !root.contains(target)) {
      return;
    }

    const data = target.dataset;

    if (data.sgPlay !== undefined) {
      togglePlay("play");
      return;
    }

    if (data.sgPlayScale !== undefined) {
      togglePlay("playScale");
      return;
    }

    if (target.id === "sg-listen") {
      change({ listen: !current().listen });
    } else if (data.sgMatch) {
      change({ match: data.sgMatch as ScaleGuideSettings["match"] });
    } else if (isArpeggioStyle(data.sgArpStyle)) {
      change({ arpeggioStyle: data.sgArpStyle });
    } else if (data.sgArpStrings !== undefined) {
      change({ arpeggioStrings: Number(data.sgArpStrings) });
    } else if (data.sgArpSlides !== undefined) {
      change({ arpeggioSlides: !current().arpeggioSlides });
    } else if (data.sgMode) {
      change({ mode: data.sgMode as ScaleGuideMode });
    } else if (data.sgLabels) {
      change({ labels: data.sgLabels as ScaleGuideLabels });
    } else if (data.sgPosition !== undefined) {
      const shape = target.classList.contains("sg-box") ? view?.shapes?.[Number(data.sgPosition) - 1] : undefined;
      anchorStep = step;
      change({ position: Number(data.sgPosition) });
      playShape(shape?.frets);
    } else if (data.sgChordRoot !== undefined) {
      // A chord of the scale: open it in Chords mode.
      change({ mode: "chord", key: Number(data.sgChordRoot), chord: DIATONIC_CHORDS[data.sgChordQuality ?? ""] ?? "maj", progression: "" });
    } else if (data.sgGoto !== undefined) {
      const index = Number(data.sgGoto);
      const frets = target.classList.contains("sg-strip-box") ? (view?.progression?.chords[index]?.shape?.frets ?? undefined) : undefined;
      goStep(index, frets);
      playShape(frets);

      if (target.classList.contains("sg-tab-bar")) {
        playScaleGuideBar(viewRequest(current(), progressionRequest()), index);
      }
    } else if (data.sgStep) {
      if (data.sgStep === "first") {
        resetRound();
      }

      goStep(data.sgStep === "first" ? 0 : step + (data.sgStep === "next" ? 1 : -1));
    } else if (data.sgRemove !== undefined && draft) {
      draft.chords.splice(Number(data.sgRemove), 1);
      step = Math.min(step, Math.max(0, draft.chords.length - 1));
      renderControls();
      requestView();
    }
  });

  // The scale's summary plays like a button from the keyboard too.
  element<HTMLElement>("sg-summary-main")?.addEventListener("keydown", (event) => {
    if ((event.key === "Enter" || event.key === " ") && (event.currentTarget as HTMLElement).dataset.sgPlayScale !== undefined) {
      event.preventDefault();
      togglePlay("playScale");
    }
  });

  // The arrow keys step a progression, unless a field has them.
  document.addEventListener("keydown", (event) => {
    const target = event.target as HTMLElement | null;

    if ((event.key !== "ArrowLeft" && event.key !== "ArrowRight") || !view?.progression || draft) {
      return;
    }

    if (!root.isConnected || root.closest("[hidden], .tab-panel:not(.active)") || (target && /^(INPUT|SELECT|TEXTAREA)$/.test(target.tagName))) {
      return;
    }

    event.preventDefault();
    goStep(step + (event.key === "ArrowRight" ? 1 : -1));
  });

  bindEditor();
  bindSetupMenu(root);
  followProgressionWith((chord) => chord !== step && goStep(chord));
  bindPlayVolume((playVolume) => change({ playVolume }));
  element<HTMLInputElement>("sg-listen-everywhere")?.addEventListener("change", (event) => change({ listenEverywhere: (event.target as HTMLInputElement).checked }));

  element<HTMLSelectElement>("sg-key")?.addEventListener("change", (event) => change({ key: Number((event.target as HTMLSelectElement).value) }));
  element<HTMLSelectElement>("sg-scale")?.addEventListener("change", (event) => change({ scale: (event.target as HTMLSelectElement).value }));
  element<HTMLSelectElement>("sg-chord")?.addEventListener("change", (event) => change({ chord: (event.target as HTMLSelectElement).value }));
  element<HTMLSelectElement>("sg-tuning")?.addEventListener("change", (event) => change({ tuning: (event.target as HTMLSelectElement).value }));
  element<HTMLSelectElement>("sg-capo")?.addEventListener("change", (event) => change({ capo: Number((event.target as HTMLSelectElement).value) }));
  element<HTMLInputElement>("sg-left-handed")?.addEventListener("change", (event) => change({ leftHanded: (event.target as HTMLInputElement).checked }));
}

function bindEditor(): void {
  element<HTMLSelectElement>("sg-progression")?.addEventListener("change", (event) => {
    const value = (event.target as HTMLSelectElement).value;

    if (value === NEW_PROGRESSION) {
      draft = { id: null, name: "", chords: [] };
      step = 0;
      renderControls();
      requestView();
      return;
    }

    draft = null;
    change({ progression: value });
  });

  element<HTMLButtonElement>("sg-progression-edit")?.addEventListener("click", () => {
    const chosen = current().progression;
    const builtIn = chosen.startsWith("b:") ? catalog?.progressions.find((p) => p.id === chosen.slice(2)) : undefined;
    const own = chosen.startsWith("u:") ? mine().find((p) => p.id === chosen.slice(2)) : undefined;

    if (builtIn) {
      // A built-in is never changed: editing it makes the player a copy.
      draft = { id: null, name: `Copy of ${builtIn.name}`, chords: numeralTokens(builtIn.numerals) };
    } else if (own) {
      draft = { id: own.id, name: own.name, chords: [...own.chords] };
    } else {
      return;
    }

    step = 0;
    renderControls();
    requestView();
  });

  element<HTMLInputElement>("sg-editor-name")?.addEventListener("input", (event) => {
    if (draft) {
      draft.name = (event.target as HTMLInputElement).value;
    }
  });

  element<HTMLButtonElement>("sg-editor-add")?.addEventListener("click", () => {
    const degree = Number(element<HTMLSelectElement>("sg-editor-degree")?.value ?? 0);
    const quality = element<HTMLSelectElement>("sg-editor-quality")?.value ?? "maj";

    if (!draft || draft.chords.length >= MAX_PROGRESSION_CHORDS) {
      return;
    }

    draft.chords.push(romanToken(degree, quality));
    step = draft.chords.length - 1;
    renderControls();
    requestView();
  });

  element<HTMLButtonElement>("sg-editor-save")?.addEventListener("click", () => {
    if (!draft || draft.chords.length === 0) {
      return;
    }

    const list = [...mine()];
    const id = draft.id ?? newProgressionId(list);
    const saved: UserProgression = { id, name: draft.name.trim() || "My progression", chords: [...draft.chords] };
    const index = list.findIndex((p) => p.id === id);

    if (index >= 0) {
      list[index] = saved;
    } else {
      list.push(saved);
    }

    userProgressions = list;
    saveProgressions(list);
    draft = null;
    change({ progression: `u:${id}` });
  });

  element<HTMLButtonElement>("sg-editor-cancel")?.addEventListener("click", () => {
    draft = null;
    step = 0;
    renderControls();
    requestView();
  });

  element<HTMLButtonElement>("sg-editor-delete")?.addEventListener("click", () => {
    if (!draft?.id) {
      return;
    }

    const id = draft.id;
    userProgressions = mine().filter((p) => p.id !== id);
    saveProgressions(userProgressions);
    draft = null;
    change({ progression: "" });
  });
}

function renderEditor(state: ScaleGuideSettings): void {
  const editor = element<HTMLElement>("sg-editor");
  editor?.toggleAttribute("hidden", !draft || state.mode === "scale");

  if (!draft) {
    return;
  }

  const name = element<HTMLInputElement>("sg-editor-name");

  if (name && document.activeElement !== name) {
    name.value = draft.name;
  }

  const chords = element<HTMLElement>("sg-editor-chords");

  if (chords) {
    chords.innerHTML = editorChordsMarkup(draft.chords, view);
  }

  const degree = element<HTMLSelectElement>("sg-editor-degree");

  if (degree) {
    const chosen = degree.value;
    degree.innerHTML = degreeOptions(catalog?.keys.length === 12 ? catalog.keys : FALLBACK_KEYS, state.key);
    degree.value = chosen || "0";
  }

  const quality = element<HTMLSelectElement>("sg-editor-quality");

  if (quality && (quality.options.length === 0 || quality.options.length !== (catalog?.chords.length ?? 0))) {
    quality.innerHTML = qualityOptions(catalog);
  }

  element<HTMLButtonElement>("sg-editor-delete")?.toggleAttribute("hidden", draft.id === null);
  element<HTMLButtonElement>("sg-editor-save")?.toggleAttribute("disabled", draft.chords.length === 0);
  element<HTMLButtonElement>("sg-editor-add")?.toggleAttribute("disabled", draft.chords.length >= MAX_PROGRESSION_CHORDS);
}

function buildPickers(): void {
  const keySelect = element<HTMLSelectElement>("sg-key");

  if (keySelect && (keySelect.options.length === 0 || (catalog && !pickersBuilt))) {
    const names = catalog?.keys.length === 12 ? catalog.keys : FALLBACK_KEYS;
    keySelect.innerHTML = names.map((name, pc) => `<option value="${pc}">${escapeHtml(name)}</option>`).join("");
  }

  const scaleSelect = element<HTMLSelectElement>("sg-scale");
  const chordSelect = element<HTMLSelectElement>("sg-chord");
  const tuningSelect = element<HTMLSelectElement>("sg-tuning");
  const capoSelect = element<HTMLSelectElement>("sg-capo");

  if (capoSelect && capoSelect.options.length === 0) {
    const capos = Array.from({ length: MAX_CAPO + 1 }, (_, fret) => `<option value="${fret}">${fret === 0 ? "None" : fret}</option>`);
    capoSelect.innerHTML = capos.join("");
  }

  if (!catalog || pickersBuilt || !scaleSelect || !chordSelect || !tuningSelect) {
    return;
  }

  const groups = [...new Set(catalog.scales.map((scale) => scale.group))];
  scaleSelect.innerHTML = groups
    .map((group) => {
      const options = catalog!.scales
        .filter((scale) => scale.group === group)
        .map((scale) => `<option value="${escapeHtml(scale.id)}" title="${escapeHtml(scale.formula)}">${escapeHtml(scale.name)}</option>`);
      return `<optgroup label="${escapeHtml(group)}">${options.join("")}</optgroup>`;
    })
    .join("");
  chordSelect.innerHTML = catalog.chords.map((chord) => `<option value="${escapeHtml(chord.id)}">${escapeHtml(chord.name)}</option>`).join("");
  tuningSelect.innerHTML = INSTRUMENT_GROUPS.map(([instrument, label]) => {
    const options = catalog!.tunings
      .filter((tuning) => tuning.instrument === instrument)
      .map((tuning) => `<option value="${escapeHtml(tuning.id)}">${escapeHtml(tuning.name)}</option>`);
    return options.length ? `<optgroup label="${escapeHtml(label)}">${options.join("")}</optgroup>` : "";
  }).join("");
  pickersBuilt = true;
}

function renderControls(): void {
  const state = current();
  const chordMode = state.mode === "chord";
  const scaleMode = state.mode === "scale";
  const labels = effectiveLabels(state);
  buildPickers();

  document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-mode]").forEach((button) => {
    button.classList.toggle("active", button.dataset.sgMode === state.mode);
    button.setAttribute("aria-pressed", String(button.dataset.sgMode === state.mode));
  });
  document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-labels]").forEach((button) => {
    button.classList.toggle("active", button.dataset.sgLabels === labels);
    button.setAttribute("aria-pressed", String(button.dataset.sgLabels === labels));
  });
  document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-match]").forEach((button) => {
    button.classList.toggle("active", button.dataset.sgMatch === state.match);
    button.setAttribute("aria-pressed", String(button.dataset.sgMatch === state.match));
  });

  const listen = element<HTMLElement>("sg-listen");

  if (listen) {
    listen.classList.toggle("active", state.listen);
    listen.setAttribute("aria-pressed", String(state.listen));
  }

  const keyLabel = element<HTMLElement>("sg-key-label");

  // Chords and Arpeggios take one chord on a root, or a progression in a key.
  const singleChord = !scaleMode && !state.progression && !draft;

  if (keyLabel) {
    keyLabel.textContent = singleChord ? "Root" : "Key";
  }

  element<HTMLElement>("sg-scale-field")?.toggleAttribute("hidden", !scaleMode);
  element<HTMLElement>("sg-progression-field")?.toggleAttribute("hidden", scaleMode);
  element<HTMLElement>("sg-chord-field")?.toggleAttribute("hidden", !singleChord);
  element<HTMLElement>("sg-progression-edit")?.toggleAttribute("hidden", scaleMode || !state.progression || draft !== null);

  const progressionSelect = element<HTMLSelectElement>("sg-progression");

  if (progressionSelect) {
    progressionSelect.innerHTML = progressionOptions(catalog, mine());
    const wanted = draft ? NEW_PROGRESSION : state.progression;
    progressionSelect.value = Array.from(progressionSelect.options).some((option) => option.value === wanted) ? wanted : "";
  }

  renderEditor(state);

  const root = element<HTMLElement>("scale-guide");

  if (root) {
    renderArpeggioControls(root, state, view);
  }

  const setValue = (id: string, value: string): void => {
    const select = element<HTMLSelectElement>(id);

    if (select && Array.from(select.options).some((option) => option.value === value)) {
      select.value = value;
    }
  };

  setValue("sg-key", String(state.key));
  setValue("sg-scale", state.scale);
  setValue("sg-chord", state.chord);
  setValue("sg-tuning", state.tuning);
  setValue("sg-capo", String(state.capo));

  showSetupChoices(state);
  showPlayVolume(state.playVolume);

  const positionLabel = element<HTMLElement>("sg-position-label");

  if (positionLabel) {
    positionLabel.textContent = chordMode ? "Shape" : "Position";
  }
}

/** The middle of the summary: the scale's chords, or the chord's formula and shape. */
function renderInfo(model: ScaleGuideView): string {
  if (model.mode !== "scale" && model.chord) {
    const pills = model.tones.map(
      (tone) => `<span class="sg-pill${tone.root ? " sg-pill-root" : ""}"><b>${escapeHtml(tone.interval)}</b>${escapeHtml(tone.name)}</span>`,
    );
    return pills.join("");
  }

  return (model.scale?.diatonic ?? [])
    .map(
      (chord, index) =>
        `<button type="button" class="sg-pill${index === 0 ? " sg-pill-root" : ""}" data-sg-chord-root="${chord.rootPitchClass}" data-sg-chord-quality="${escapeHtml(chord.quality)}" title="${escapeHtml(chord.numeral)}: open in Chords">${escapeHtml(chord.name)}</button>`,
    )
    .join("");
}

function setText(id: string, text: string): void {
  const target = element<HTMLElement>(id);

  if (target) {
    target.textContent = text;
  }
}

function renderSummary(model: ScaleGuideView): void {
  const chordMode = model.mode === "chord";
  const arpeggioMode = model.mode === "arpeggio";
  const root = model.tones[0]?.name ?? "";
  const names = model.tones.map((tone) => tone.name);
  const shape = chordMode && model.position > 0 ? model.shapes?.[model.position - 1] : undefined;

  setText("sg-root-badge", root);
  setText("sg-summary-label", arpeggioMode ? "Arpeggio" : chordMode ? "Chord" : "Scale");
  setText("sg-summary-name", chordMode || arpeggioMode ? `${model.chord?.name ?? ""} · ${model.chord?.quality ?? ""}` : (model.scale?.name ?? ""));
  // A scale reads up to its octave; a chord is just its tones.
  setText("sg-tones", (chordMode || arpeggioMode ? names : [...names, root]).join("  "));
  setText("sg-info-label", chordMode || arpeggioMode ? "Formula" : model.scale?.diatonic.length ? "Chords" : "Steps");
  setText(
    "sg-reference",
    arpeggioMode
      ? (model.arpeggio?.advice ?? "")
      : chordMode
        ? model.progression
          ? `Next: ${model.progression.next}. ◀ ▶ or the arrow keys step through; each chord takes the shape nearest the last.`
          : shape
            ? `${shape.caged ? `The ${shape.caged} shape. ` : ""}Numbers are fingers; × is a string you don't play.`
            : "Pick a shape to see one way to play it; All shows every chord tone."
        : `Steps ${model.scale?.steps ?? ""}. Pick a position for one box; All shows the whole neck. Tap a chord to open it.`,
  );

  markSummaryPlayable(model.mode === "scale" && model.tones.length > 0);

  const info = element<HTMLElement>("sg-info");

  if (info) {
    info.innerHTML = model.mode === "scale" && !model.scale?.diatonic.length ? `<span class="sg-steps">${escapeHtml(model.scale?.steps ?? "")}</span>` : renderInfo(model);
  }
}

function renderNeck(model: ScaleGuideView, state: ScaleGuideSettings): void {
  const neck = element<HTMLElement>("sg-neck");

  if (!neck) {
    return;
  }

  const options = { labels: effectiveLabels(state), leftHanded: state.leftHanded, width: neck.clientWidth };
  const noRuns = model.mode === "arpeggio" && model.arpeggio?.runs.length === 0 ? model.arpeggio.empty : "";
  drawnNeckWidth = neckDrawingWidth(options.width);
  neck.innerHTML =
    noRuns || (model.mode === "chord" && (model.shapes?.length ?? 0) === 0)
      ? `${renderNeckSvg(model, options)}<p class="sg-note">${escapeHtml(noRuns || "No shape fits this tuning, so the neck marks all the chord's notes.")}</p>`
      : renderNeckSvg(model, options);
}

/** Redraws the neck when its space changes: a window resize, a zoom, or the panel coming into view. */
function watchNeckWidth(): void {
  const neck = element<HTMLElement>("sg-neck");

  if (!neck || typeof ResizeObserver === "undefined") {
    return;
  }

  new ResizeObserver(() => {
    // A hidden panel measures nothing: keep the drawing for when it shows again.
    if (view && neck.clientWidth > 0 && neckDrawingWidth(neck.clientWidth) !== drawnNeckWidth) {
      renderNeck(view, current());
      renderListen(listenHost);
    }
  }).observe(neck);
}

function renderView(): void {
  if (!view) {
    return;
  }

  const state = current();
  const model = view;
  const play = element<HTMLButtonElement>("sg-play");

  if (play) {
    const nothingChosen = model.position === 0 && !model.progressionRun?.steps.length;
    play.disabled = nothingChosen;
    play.title = nothingChosen ? "Pick a position to hear it" : "Hear it played through the current preset";
  }

  setText("sg-title", instrumentLabel(model, state));

  const positions = element<HTMLElement>("sg-positions");

  if (positions) {
    positions.innerHTML = renderPositions(model);
  }

  renderNeck(model, state);

  renderRunCard(element<HTMLElement>("sg-arp-tab"), model);

  const root = element<HTMLElement>("scale-guide");

  if (root) {
    renderArpeggioControls(root, state, model);
  }

  // The steps row is the progression's in Chords and Arpeggios; the strip of chord boxes is Chords' alone.
  element<HTMLElement>("sg-steps-row")?.toggleAttribute("hidden", !model.progression);
  const steps = element<HTMLElement>("sg-steps");

  if (steps) {
    steps.innerHTML = model.progression ? stepsMarkup(model) : "";
  }

  const progression = model.mode === "chord" ? model.progression : undefined;

  const boxes = element<HTMLElement>("sg-boxes");

  if (boxes && progression) {
    boxes.toggleAttribute("hidden", false);
    boxes.classList.add("sg-strip");
    boxes.innerHTML = stripMarkup(model, state.leftHanded);
  } else if (boxes) {
    const shapes = model.mode === "chord" ? (model.shapes ?? []) : [];
    boxes.classList.remove("sg-strip");
    boxes.toggleAttribute("hidden", shapes.length === 0);
    boxes.innerHTML = shapes
      .map(
        (shape, index) =>
          `<button type="button" class="sg-box${model.position === index + 1 ? " active" : ""}" data-sg-position="${index + 1}" aria-pressed="${model.position === index + 1}" title="Show this shape and hear it strummed">${renderChordBoxSvg(shape, model, { leftHanded: state.leftHanded })}</button>`,
      )
      .join("");
  }

  renderSummary(model);

  if (draft) {
    renderEditor(state);
  }

  renderListen(listenHost);
  showScaleGuidePlaying();
}

/** For tests: forget everything, as on a fresh page. */
export function resetScaleGuideForTests(): void {
  settings = null;
  catalog = null;
  view = null;
  latestRequestId = 0;
  catalogRequested = false;
  pickersBuilt = false;
  bound = false;
  drawnNeckWidth = 0;
  step = 0;
  anchorStep = 0;
  pendingPrevious = undefined;
  draft = null;
  userProgressions = null;
  resetListenForTests();
  resetPlaybackForTests();
  onScaleGuidePlaybackEnded();
}
