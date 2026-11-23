/**
 * The tuner dialog's intonation panel (tuner-modal.html, .tuner-intonation): the steps, the
 * measurement of the note being played and the frets checked so far on each string. The maths
 * is in tunerIntonation.ts; tuner.ts owns the dialog and hands every reading on.
 */

import {
  IntonationTest,
  fretReading,
  intonationAdvice,
  kIntonationMaxFret,
  kIntonationQualityLabels,
  stringAdvice,
  type IntonationView,
  type PitchReading,
} from "./tunerIntonation.js";

const test = new IntonationTest();
let enabled = false;

let toggleBtn: HTMLElement | null = null;
let panel: HTMLElement | null = null;
let hint: HTMLElement | null = null;
let setOpenBtn: HTMLButtonElement | null = null;
let newStringBtn: HTMLButtonElement | null = null;
let result: HTMLElement | null = null;
let resultFret: HTMLElement | null = null;
let resultCents: HTMLElement | null = null;
let resultQuality: HTMLElement | null = null;
let advice: HTMLElement | null = null;
let strings: HTMLElement | null = null;
let clearBtn: HTMLButtonElement | null = null;

/** Called whenever the panel's mode changes, so the dialog can redraw its main readout. */
let onModeChange: () => void = () => {};

function formatCents(cents: number): string {
  const rounded = Math.round(cents * 10) / 10;
  return `${rounded > 0 ? "+" : rounded < 0 ? "−" : "±"}${Math.abs(rounded).toFixed(1)}¢`;
}

export function initializeTunerIntonation(redraw: () => void): void {
  onModeChange = redraw;
  toggleBtn = document.getElementById("tuner-intonation-btn");
  panel = document.getElementById("tuner-intonation-panel");
  hint = document.getElementById("tuner-intonation-hint");
  setOpenBtn = document.getElementById("tuner-intonation-set-open") as HTMLButtonElement | null;
  newStringBtn = document.getElementById("tuner-intonation-new-string") as HTMLButtonElement | null;
  result = document.getElementById("tuner-intonation-result");
  resultFret = document.getElementById("tuner-intonation-result-fret");
  resultCents = document.getElementById("tuner-intonation-result-cents");
  resultQuality = document.getElementById("tuner-intonation-result-quality");
  advice = document.getElementById("tuner-intonation-advice");
  strings = document.getElementById("tuner-intonation-strings");
  clearBtn = document.getElementById("tuner-intonation-clear") as HTMLButtonElement | null;

  toggleBtn?.addEventListener("click", () => setIntonationEnabled(!enabled));
  setOpenBtn?.addEventListener("click", () => {
    if (test.setOpenString()) refresh();
  });
  newStringBtn?.addEventListener("click", () => {
    test.newString();
    refresh();
  });
  clearBtn?.addEventListener("click", () => {
    test.clear();
    refresh();
  });

  syncEnabled();
}

/** The panel and the dialog's readout, after the string being checked changes. */
function refresh(): void {
  render(test.view());
  onModeChange();
}

export function isIntonationEnabled(): boolean {
  return enabled;
}

export function setIntonationEnabled(on: boolean): void {
  enabled = on;
  syncEnabled();
  onModeChange();
}

function syncEnabled(): void {
  toggleBtn?.classList.toggle("active", enabled);
  toggleBtn?.setAttribute("aria-pressed", String(enabled));
  if (panel) panel.hidden = !enabled;
  if (enabled) render(test.view());
}

export function setIntonationReference(hz: number): void {
  test.setReferenceFrequency(hz);
}

/** The tuner opened or closed: a note in progress is not carried across. */
export function resetIntonationReadings(): void {
  test.resetReadings();
  if (enabled) render(test.view());
}

/** Every tuner reading, raw (before the dialog's own smoothing). */
export function pushIntonationReading(reading: PitchReading): void {
  const view = test.push(reading);
  if (enabled) render(view);
}

/**
 * What the dialog's big readout shows while a string is set: the fret nearest the note and how
 * far out it is, its neighbours either side. Null to show the note as usual.
 */
export function intonationReadout(
  frequency: number,
): { fret: string; left: string; right: string; cents: number } | null {
  if (!enabled) return null;
  const string = test.view().string;
  if (!string) return null;
  const reading = fretReading(string.openFrequency, frequency);
  if (!reading) return null;
  const fret = reading.fret;
  return {
    fret: String(fret),
    left: fret > 0 ? String(fret - 1) : "",
    right: fret < kIntonationMaxFret ? String(fret + 1) : "",
    cents: reading.cents,
  };
}

function render(view: IntonationView): void {
  if (!panel) return;
  panel.dataset.phase = view.phase;

  if (hint) hint.textContent = hintText(view);
  if (setOpenBtn) {
    setOpenBtn.hidden = view.phase !== "open";
    setOpenBtn.disabled = !view.candidate;
    setOpenBtn.textContent = view.candidate ? `Set ${view.candidate.name} as open string` : "Set open string";
  }
  if (newStringBtn) newStringBtn.hidden = view.phase !== "fret";

  renderMeasurement(view);
  renderStrings(view);
}

function hintText(view: IntonationView): string {
  if (view.phase === "open") {
    if (!view.candidate) {
      return "Tune an open string, pick it gently and let it ring, then set it as the reference.";
    }
    return `Heard ${view.candidate.name} (${formatCents(view.candidate.cents)}). Set it as the open string, or retune and play it again.`;
  }
  const name = view.string?.name ?? "";
  if (view.offString) {
    return `That note isn't on the ${name} string (below it, or past fret ${kIntonationMaxFret}). Play this string, or start a new one.`;
  }
  return `Play the ${name} string at any fret, picking gently and letting it ring; the 12th is the usual check. The fret is found from the pitch.`;
}

function renderMeasurement(view: IntonationView): void {
  if (!result || !resultFret || !resultCents || !resultQuality || !advice) return;
  const m = view.phase === "fret" ? view.measurement : null;
  result.hidden = view.phase !== "fret";
  if (!m) {
    result.dataset.quality = "";
    resultFret.textContent = "Fret –";
    resultCents.textContent = "";
    resultQuality.textContent = "Waiting for a steady note";
    advice.textContent = "";
    return;
  }
  result.dataset.quality = m.quality;
  resultFret.textContent = m.fret === 0 ? "Open" : `Fret ${m.fret}`;
  resultCents.textContent = formatCents(m.cents);
  resultQuality.textContent = kIntonationQualityLabels[m.quality];
  advice.textContent = intonationAdvice(m.fret, m.cents);
}

function renderStrings(view: IntonationView): void {
  if (!strings) return;
  const rows: HTMLElement[] = [];
  for (const string of view.strings) {
    const row = document.createElement("div");
    row.className = "tuner-intonation-string";
    row.classList.toggle("current", string === view.string);
    const label = document.createElement("span");
    label.className = "tuner-intonation-string-name";
    label.textContent = string.name;
    row.appendChild(label);
    const frets = document.createElement("span");
    frets.className = "tuner-intonation-string-frets";
    if (!string.frets.length) {
      const empty = document.createElement("span");
      empty.className = "tuner-intonation-string-empty";
      empty.textContent = "No frets checked yet";
      frets.appendChild(empty);
    }
    for (const fret of string.frets) {
      const chip = document.createElement("span");
      chip.className = "tuner-intonation-chip";
      chip.dataset.quality = fret.quality;
      chip.title = `Fret ${fret.fret}: ${formatCents(fret.cents)}, ${kIntonationQualityLabels[fret.quality]}`;
      const n = document.createElement("b");
      n.textContent = String(fret.fret);
      chip.appendChild(n);
      chip.appendChild(document.createTextNode(` ${formatCents(fret.cents)}`));
      frets.appendChild(chip);
    }
    row.appendChild(frets);
    const notes = stringAdvice(string.frets);
    if (notes.length) {
      const diagnosis = document.createElement("span");
      diagnosis.className = "tuner-intonation-string-advice";
      diagnosis.textContent = notes.join(" ");
      row.appendChild(diagnosis);
    }
    rows.push(row);
  }
  strings.replaceChildren(...rows);
  strings.hidden = rows.length === 0;
  if (clearBtn) clearBtn.hidden = rows.length === 0;
}
