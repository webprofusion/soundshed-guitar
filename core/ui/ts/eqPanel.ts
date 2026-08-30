/**
 * EqPanel — the reusable four-band parametric EQ control surface.
 *
 * One EQ UI, however many EQs the app grows. It renders the band knobs, owns
 * the draggable curve, and wires the enable toggle; everything specific to a
 * *particular* EQ — where its parameter values live and how a change reaches
 * the engine — arrives as an `EqPanelBinding`. The Global EQ binds to the
 * post-chain `global_eq` graph node; the Practice Tool binds to its own
 * backing-track EQ state. Neither owns any of the code below.
 *
 * The band topology, ranges and defaults all come from `eqCurve.ts`
 * (`EQ_BAND_KEYS` / `EQ_BAND_RANGES` / `EQ_FREQ_DEFAULTS`), which already
 * mirrors `ParametricEQEffect`, so a band added there appears in every panel
 * with no further work.
 *
 * Markup: give it a host element and it fills in `.eq-bands` with one
 * `.eq-band` per band, matching the structure the EQ modal used to carry by
 * hand — the existing `.eq-band` / `.eq-knob-*` styles apply unchanged.
 */

import {
  EQ_BAND_KEYS,
  EQ_BAND_LABELS,
  EQ_BAND_RANGES,
  EQ_FREQ_DEFAULTS,
  EqCurveInteraction,
  buildEqBandConfigsFromParams,
  eqBandChangeToParams,
} from "./eqCurve.js";
import { GenericKnob } from "./knob.js";
import { appendLog } from "./logging.js";

/**
 * Everything an EQ panel needs to know about the specific EQ it is editing.
 * Implementations own storage and transport; the panel owns the UI.
 */
export interface EqPanelBinding {
  /** Short name for log lines, e.g. "eq" or "practice tool EQ". */
  label: string;
  /** Current values, keyed by ParametricEQEffect parameter names. Missing keys
   * fall back to the band defaults. */
  readParams(): Record<string, number | undefined>;
  /**
   * Applies changed parameters — locally and to the engine. `commit` is false
   * for in-progress drag ticks, letting a binding coalesce its sends, and true
   * at the end of a gesture or for a typed/reset value.
   */
  writeParams(changed: Record<string, number>, commit: boolean): void;
  readEnabled(): boolean;
  writeEnabled(enabled: boolean): void;
}

export interface EqPanelOptions {
  /** Host for the generated band rows. */
  bandsHost: HTMLElement | null;
  /** Curve canvas. Without one the panel is knobs only, which is still valid. */
  canvas: HTMLCanvasElement | null;
  /** Enable checkbox. Optional — an always-on EQ simply omits it. */
  toggle?: HTMLInputElement | null;
  /** Button that returns every band to its default. Optional. */
  resetButton?: HTMLElement | null;
  /**
   * Prefix for the generated knob ids, so two panels on the same page never
   * collide (the Global EQ and the Practice Tool EQ can both be open).
   */
  idPrefix: string;
  /**
   * Called after the panel changes anything, so a host can refresh whatever
   * *it* shows about the EQ — a mirrored toggle elsewhere in the app, a
   * "this EQ is doing something" badge, a section's enabled class.
   */
  onChanged?: () => void;
}

/** dB range every band shares — ParametricEQEffect clamps to this, so showing
 * more would just display values the engine discards. */
const BAND_GAIN_MIN = -12;
const BAND_GAIN_MAX = 12;

type BandKnobs = { gain: GenericKnob; freq: GenericKnob; q: GenericKnob | null };

function formatGain(value: number): string {
  return `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`;
}

function formatFreq(value: number): string {
  return value >= 1000 ? `${(value / 1000).toFixed(2)}k Hz` : `${Math.round(value)} Hz`;
}

function formatQ(value: number): string {
  return value.toFixed(2);
}

export class EqPanel {
  private readonly options: EqPanelOptions;
  private readonly binding: EqPanelBinding;
  private bandKnobs: BandKnobs[] = [];
  private curve: EqCurveInteraction | null = null;
  // Guards the feedback loop: writing a knob's value fires its own change
  // handler, which would write straight back to the binding mid-render.
  private syncing = false;

  constructor(options: EqPanelOptions, binding: EqPanelBinding) {
    this.options = options;
    this.binding = binding;
    this.buildBandControls();
    this.bindToggle();
    this.bindResetButton();
    this.render();
  }

  /** Redraws knobs, curve and toggle from the binding. Safe to call as often
   * as the host likes — it is the single "the EQ changed underneath me" entry
   * point, used after a preset load as much as after a local edit. */
  render(): void {
    const params = this.binding.readParams();

    this.syncing = true;
    try {
      EQ_BAND_KEYS.forEach((keys, index) => {
        const knobs = this.bandKnobs[index];
        if (!knobs) {
          return;
        }
        knobs.gain.setValue(this.valueFor(params, keys.gain, 0));
        knobs.freq.setValue(this.valueFor(params, keys.freq, EQ_FREQ_DEFAULTS[index]));
        if (knobs.q && keys.q) {
          knobs.q.setValue(this.valueFor(params, keys.q, EQ_BAND_RANGES[index].qDefault));
        }
      });
    } finally {
      this.syncing = false;
    }

    this.syncEnabledState();
    this.renderCurve(params);
  }

  destroy(): void {
    this.curve?.destroy();
    this.curve = null;
  }

  /** Returns every band to its default and commits that in one go. */
  resetToDefaults(): void {
    const changed: Record<string, number> = {};
    EQ_BAND_KEYS.forEach((keys, index) => {
      changed[keys.gain] = 0;
      changed[keys.freq] = EQ_FREQ_DEFAULTS[index];
      if (keys.q) {
        changed[keys.q] = EQ_BAND_RANGES[index].qDefault;
      }
    });
    this.binding.writeParams(changed, true);
    // The curve keeps its own copies of the bands, so a wholesale reset has to
    // rebuild it rather than nudging the values it already holds.
    this.curve?.destroy();
    this.curve = null;
    this.render();
    this.options.onChanged?.();
    appendLog(`${this.binding.label} reset to flat`);
  }

  /** Pushes the enabled flag onto the toggle and onto the bands host, which
   * dims while the EQ is off. Owned here rather than by a host's own "enabled"
   * class, so every panel gets the affordance without arranging for it — and
   * called from the toggle handler too, not just render(), so flipping it
   * updates the dimming immediately. */
  private syncEnabledState(): void {
    const enabled = this.binding.readEnabled();
    if (this.options.toggle) {
      this.options.toggle.checked = enabled;
    }
    this.options.bandsHost?.classList.toggle("is-eq-enabled", enabled);
  }

  private valueFor(params: Record<string, number | undefined>, key: string, fallback: number): number {
    const value = params[key];
    return typeof value === "number" && Number.isFinite(value) ? value : fallback;
  }

  /**
   * Emits the band rows and constructs a knob per control. This replaces what
   * used to be ~120 lines of hand-written markup plus twelve near-identical
   * `new GenericKnob({...})` blocks per EQ.
   */
  private buildBandControls(): void {
    const host = this.options.bandsHost;
    if (!host) {
      return;
    }

    const knobHtml = (id: string, label: string, value: number) => `
      <div class="eq-knob-control">
        <span class="knob-label">${label}</span>
        <div class="knob" data-param="${id}" data-value="${value}">
          <div class="knob-indicator"></div>
        </div>
        <span class="knob-value" data-knob-value="${id}"></span>
      </div>`;

    const params = this.binding.readParams();
    host.classList.add("eq-bands");
    host.innerHTML = EQ_BAND_KEYS.map((keys, index) => {
      const prefix = `${this.options.idPrefix}_${index}`;
      return `
        <div class="eq-band">
          <span class="eq-band-title">${EQ_BAND_LABELS[index].toUpperCase()}</span>
          <div class="eq-knob-group">
            ${knobHtml(`${prefix}_gain`, "GAIN", this.valueFor(params, keys.gain, 0))}
            ${knobHtml(`${prefix}_freq`, "FREQ", this.valueFor(params, keys.freq, EQ_FREQ_DEFAULTS[index]))}
            ${keys.q ? knobHtml(`${prefix}_q`, "Q", this.valueFor(params, keys.q, EQ_BAND_RANGES[index].qDefault)) : ""}
          </div>
        </div>`;
    }).join("");

    this.bandKnobs = EQ_BAND_KEYS.map((keys, index) => {
      const range = EQ_BAND_RANGES[index];
      const prefix = `${this.options.idPrefix}_${index}`;
      const make = (
        suffix: string, min: number, max: number, def: number,
        format: (value: number) => string, sensitivity: number, paramKey: string
      ): GenericKnob | null => {
        const id = `${prefix}_${suffix}`;
        const element = host.querySelector<HTMLElement>(`.knob[data-param="${id}"]`);
        if (!element) {
          return null;
        }
        return new GenericKnob({
          knobElement: element,
          paramId: id,
          minValue: min,
          maxValue: max,
          defaultValue: def,
          displayFormat: format,
          valueDisplay: host.querySelector<HTMLElement>(`[data-knob-value="${id}"]`),
          sensitivity,
          // The panel routes every change through the binding itself; the knob
          // must never take its own shortcut to the engine.
          sendParameter: false,
          onValueChange: (value) => this.onKnobChanged(paramKey, value, false),
          onValueCommit: (value) => this.onKnobChanged(paramKey, value, true),
        });
      };

      // Sensitivities scale with each control's range so every knob feels the
      // same under the hand regardless of what it spans.
      const gain = make("gain", BAND_GAIN_MIN, BAND_GAIN_MAX, 0, formatGain, 0.1, keys.gain);
      const freq = make("freq", range.freqMin, range.freqMax, EQ_FREQ_DEFAULTS[index], formatFreq,
                        Math.max(1, (range.freqMax - range.freqMin) / 240), keys.freq);
      const q = keys.q
        ? make("q", range.qMin, range.qMax, range.qDefault, formatQ, 0.05, keys.q)
        : null;
      return gain && freq ? { gain, freq, q } : null;
    }).filter((entry): entry is BandKnobs => entry !== null);
  }

  private onKnobChanged(paramKey: string, value: number, commit: boolean): void {
    if (this.syncing) {
      return;
    }
    this.binding.writeParams({ [paramKey]: value }, commit);
    this.renderCurve(this.binding.readParams());
    if (commit) {
      appendLog(`${this.binding.label} ${paramKey} → ${value.toFixed(2)}`);
    }
    this.options.onChanged?.();
  }

  private renderCurve(params: Record<string, number | undefined>): void {
    const canvas = this.options.canvas;
    if (!canvas) {
      return;
    }
    const configs = buildEqBandConfigsFromParams(params);
    if (this.curve) {
      this.curve.updateBands(configs);
      return;
    }
    // Built lazily: inside a modal the canvas has no size until it is shown,
    // and a curve constructed against a zero-sized canvas draws nothing.
    if (canvas.clientWidth <= 0 || canvas.clientHeight <= 0) {
      return;
    }
    this.curve = new EqCurveInteraction(
      canvas,
      configs,
      (bandIndex, freq, gainDb, q) => this.onCurveBandChanged(bandIndex, freq, gainDb, q, false),
      (bandIndex, freq, gainDb, q) => this.onCurveBandChanged(bandIndex, freq, gainDb, q, true)
    );
  }

  private onCurveBandChanged(bandIndex: number, freq: number, gainDb: number, q: number, commit: boolean): void {
    const changed = eqBandChangeToParams(bandIndex, freq, gainDb, q);
    this.binding.writeParams(changed, commit);

    // Keep the knobs showing what the curve is doing. Guarded, or each
    // setValue would call straight back into onKnobChanged.
    const knobs = this.bandKnobs[bandIndex];
    if (knobs) {
      this.syncing = true;
      try {
        knobs.gain.setValue(gainDb);
        knobs.freq.setValue(freq);
        knobs.q?.setValue(q);
      } finally {
        this.syncing = false;
      }
    }

    if (commit) {
      appendLog(`${this.binding.label} ${EQ_BAND_LABELS[bandIndex]}: ${Math.round(freq)}Hz ${gainDb >= 0 ? "+" : ""}${gainDb.toFixed(1)}dB Q:${q.toFixed(1)}`);
    }
    this.options.onChanged?.();
  }

  private bindToggle(): void {
    const toggle = this.options.toggle;
    if (!toggle || toggle.dataset.eqPanelBound === "true") {
      return;
    }
    toggle.dataset.eqPanelBound = "true";
    toggle.addEventListener("change", () => {
      this.binding.writeEnabled(toggle.checked);
      this.syncEnabledState();
      appendLog(`${this.binding.label} → ${toggle.checked ? "on" : "off"}`);
      this.options.onChanged?.();
    });
  }

  private bindResetButton(): void {
    const button = this.options.resetButton;
    if (!button || button.dataset.eqPanelBound === "true") {
      return;
    }
    button.dataset.eqPanelBound = "true";
    button.addEventListener("click", () => this.resetToDefaults());
  }
}
