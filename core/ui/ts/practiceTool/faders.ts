/**
 * The Practice Tool's four faders — Volume, Balance, Speed, Pitch — and the
 * debounced engine sends they share with the loop-region drag.
 *
 * Split out of practiceTool.ts. Like the rest of the Practice Tool's modules it
 * never imports that facade: the one thing a fader change has to tell it (keep
 * the playhead's dead-reckoning rate and the active loop's settings in step)
 * comes in as the `onChange` hook passed to bindPracticeToolFaders().
 */

import { setPracticeToolBalance, setPracticeToolGain, setPracticeToolPitch, setPracticeToolSpeed } from "../bridge.js";
import type { PracticeToolLoopSettings, PracticeToolState } from "../types.js";

const SPEED_PITCH_SEND_DEBOUNCE_MS = 80;

/** A debounced one-shot sender: `schedule()` coalesces rapid updates (a slider
 * being dragged, a loop handle being dragged), `flush()` cancels any pending
 * timer and sends straight away.
 *
 * Speed, pitch, and loop-region sends each flush the native render-ahead ring
 * buffer (they must, to apply the new value promptly) — sending on every
 * `input`/`mousemove` event during a drag would flush repeatedly and cause
 * audible stutter, which is exactly what un-throttled sends did before this
 * existed. The one-off end-of-gesture event (`change`, `mouseup`) calls
 * `flush()` so the final value is never left sitting in a pending timer. */
export function debouncedSender<T>(send: (value: T) => void, delayMs: number) {
  let timer: ReturnType<typeof setTimeout> | null = null;
  const cancel = () => {
    if (timer !== null) {
      clearTimeout(timer);
      timer = null;
    }
  };
  return {
    schedule(value: T): void {
      cancel();
      timer = setTimeout(() => {
        timer = null;
        send(value);
      }, delayMs);
    },
    flush(value: T): void {
      cancel();
      send(value);
    },
  };
}

const speedSender = debouncedSender(setPracticeToolSpeed, SPEED_PITCH_SEND_DEBOUNCE_MS);
const pitchSender = debouncedSender(setPracticeToolPitch, SPEED_PITCH_SEND_DEBOUNCE_MS);

// ════════════════════════════════════════════════════════════════════
// Unified faders (Volume/Balance/Speed/Pitch): one shared implementation
// instead of four near-duplicated sliders, so they all look, feel, and
// reset the same way.
//
// Every fader's default value sits at the exact visual center regardless
// of how asymmetric its real min/max range is (e.g. Volume's 0-150% with
// a 100% default, or Speed's 25%-200% with a 100% default) — the
// underlying <input type=range> always uses a normalized 0..FADER_SLIDER_STEPS
// domain split into two independently-scaled linear halves (min..default,
// default..max), converted to/from the real value on every read/write. A
// plain <input type=range> can't express that piecewise mapping itself, so
// this conversion layer is what makes "100%" (or "0 st", or center balance)
// always land in the middle of the track, and what makes double-clicking
// anywhere on the slider a well-defined "reset to default" regardless of
// the range's shape.
// ════════════════════════════════════════════════════════════════════

const FADER_SLIDER_STEPS = 1000;

export type FaderId = "volume" | "balance" | "speed" | "pitch";

type FaderSpec = {
  id: FaderId;
  min: number;
  max: number;
  default: number;
  format: (value: number) => string;
  parse: (text: string) => number | null;
  getValue: (player: PracticeToolState) => number;
  setValue: (player: PracticeToolState, value: number) => void;
  /** immediate=true on release/reset/typed-entry; false for in-progress drag
   * ticks, letting speed/pitch debounce (they flush the render-ahead ring)
   * while volume/balance (pure audio-thread mix, no flush) can ignore the
   * flag and always send right away. */
  send: (value: number, immediate: boolean) => void;
};

/** Told after every fader write, user-made or programmatic. */
export type FaderChangeHook = (id: FaderId, value: number, immediate: boolean) => void;

function faderValueToSliderPos(spec: FaderSpec, value: number): number {
  const half = FADER_SLIDER_STEPS / 2;
  if (value <= spec.default) {
    if (spec.default === spec.min) {
      return half;
    }
    const t = (value - spec.min) / (spec.default - spec.min);
    return Math.round(t * half);
  }
  if (spec.max === spec.default) {
    return half;
  }
  const t = (value - spec.default) / (spec.max - spec.default);
  return Math.round(half + t * half);
}

function faderSliderPosToValue(spec: FaderSpec, pos: number): number {
  const half = FADER_SLIDER_STEPS / 2;
  if (pos <= half) {
    return spec.min + (pos / half) * (spec.default - spec.min);
  }
  return spec.default + ((pos - half) / half) * (spec.max - spec.default);
}

/** Extracts a leading signed number from free-typed text (tolerating a
 * trailing unit like "%" or "st"); returns null if nothing parseable. */
function parseLeadingNumber(text: string): number | null {
  const match = text.trim().match(/^[+-]?\d*\.?\d+/);
  if (!match) {
    return null;
  }
  const n = parseFloat(match[0]);
  return isFinite(n) ? n : null;
}

function parsePercentText(text: string): number | null {
  const n = parseLeadingNumber(text);
  return n === null ? null : n / 100;
}

function formatPitchText(value: number): string {
  return `${value > 0 ? "+" : ""}${value.toFixed(1)} st`;
}

function formatBalanceText(value: number): string {
  const pct = Math.round(value * 100);
  if (pct === 0) {
    return "C";
  }
  return pct < 0 ? `L${Math.abs(pct)}` : `R${pct}`;
}

function parseBalanceText(text: string): number | null {
  const trimmed = text.trim();
  if (/^c(enter)?$/i.test(trimmed)) {
    return 0;
  }
  const sided = /^([lr])\s*(\d+(?:\.\d+)?)$/i.exec(trimmed);
  if (sided) {
    const magnitude = parseFloat(sided[2]) / 100;
    return sided[1].toLowerCase() === "l" ? -magnitude : magnitude;
  }
  const n = parseLeadingNumber(trimmed);
  if (n === null) {
    return null;
  }
  // Accept both "35"/"-35" (percent-style) and "0.35"/"-0.35" (raw fraction).
  return Math.abs(n) > 1 ? n / 100 : n;
}

const FADER_SPECS: Record<FaderId, FaderSpec> = {
  volume: {
    id: "volume",
    min: 0,
    max: 1.5,
    default: 1,
    format: (v) => `${Math.round(v * 100)}%`,
    parse: parsePercentText,
    getValue: (p) => p.gain,
    setValue: (p, v) => { p.gain = v; },
    send: (v) => setPracticeToolGain(v),
  },
  balance: {
    id: "balance",
    min: -1,
    max: 1,
    default: 0,
    format: formatBalanceText,
    parse: parseBalanceText,
    getValue: (p) => p.balance,
    setValue: (p, v) => { p.balance = v; },
    send: (v) => setPracticeToolBalance(v),
  },
  speed: {
    id: "speed",
    min: 0.25,
    max: 2,
    default: 1,
    format: (v) => `${Math.round(v * 100)}%`,
    parse: parsePercentText,
    getValue: (p) => p.speed,
    setValue: (p, v) => { p.speed = v; },
    send: (v, immediate) => (immediate ? speedSender.flush(v) : speedSender.schedule(v)),
  },
  pitch: {
    id: "pitch",
    min: -12,
    max: 12,
    default: 0,
    format: formatPitchText,
    parse: parseLeadingNumber,
    getValue: (p) => p.pitchSemitones,
    setValue: (p, v) => { p.pitchSemitones = v; },
    send: (v, immediate) => (immediate ? pitchSender.flush(v) : pitchSender.schedule(v)),
  },
};

function writeFader(player: PracticeToolState, spec: FaderSpec, value: number, immediate: boolean, onChange: FaderChangeHook): void {
  const clamped = Math.max(spec.min, Math.min(spec.max, value));
  spec.setValue(player, clamped);
  onChange(spec.id, clamped, immediate);
  spec.send(clamped, immediate);
}

/** The four fader values as a loop records them. */
export function captureFaderSettings(player: PracticeToolState): PracticeToolLoopSettings {
  return { gain: player.gain, balance: player.balance, speed: player.speed, pitchSemitones: player.pitchSemitones };
}

/**
 * Sets all four faders at once — a recalled project, a loop's own settings, or
 * (with no `settings`) every fader's default when a new file resets the session.
 * Each value is clamped and sent on to the engine exactly as moving that
 * control by hand would.
 */
export function applyFaderSettings(player: PracticeToolState, settings: PracticeToolLoopSettings | null, onChange: FaderChangeHook): void {
  const values: Record<FaderId, number> = settings
    ? { volume: settings.gain, balance: settings.balance, speed: settings.speed, pitch: settings.pitchSemitones }
    : { volume: FADER_SPECS.volume.default, balance: FADER_SPECS.balance.default, speed: FADER_SPECS.speed.default, pitch: FADER_SPECS.pitch.default };
  Object.values(FADER_SPECS).forEach((spec) => writeFader(player, spec, values[spec.id], true, onChange));
}

export function renderPracticeToolFaders(player: PracticeToolState): void {
  Object.values(FADER_SPECS).forEach((spec) => {
    const slider = document.getElementById(`practice-tool-${spec.id}`) as HTMLInputElement | null;
    const valueInput = document.getElementById(`practice-tool-${spec.id}-value`) as HTMLInputElement | null;
    const value = spec.getValue(player);
    if (slider && document.activeElement !== slider) {
      slider.value = String(faderValueToSliderPos(spec, value));
    }
    if (valueInput && document.activeElement !== valueInput) {
      valueInput.value = spec.format(value);
    }
  });
}

/** `rerender` redraws the transport row; `onChange` is told about every write. */
export function bindPracticeToolFaders(getPlayer: () => PracticeToolState, onChange: FaderChangeHook, rerender: () => void): void {
  Object.values(FADER_SPECS).forEach((spec) => bindFader(spec, getPlayer, onChange, rerender));
}

function bindFader(spec: FaderSpec, getPlayer: () => PracticeToolState, onChange: FaderChangeHook, rerender: () => void): void {
  const slider = document.getElementById(`practice-tool-${spec.id}`) as HTMLInputElement | null;
  const valueInput = document.getElementById(`practice-tool-${spec.id}-value`) as HTMLInputElement | null;

  const applyValue = (value: number, immediate: boolean) => {
    writeFader(getPlayer(), spec, value, immediate, onChange);
    rerender();
  };

  if (slider && slider.dataset.bound !== "true") {
    slider.dataset.bound = "true";
    slider.addEventListener("input", () => {
      const pos = parseFloat(slider.value);
      if (isFinite(pos)) {
        applyValue(faderSliderPosToValue(spec, pos), false);
      }
    });
    // Fires once on release (mouseup/keyup) — always commit the final
    // value immediately even if speed/pitch were mid-debounce.
    slider.addEventListener("change", () => {
      const pos = parseFloat(slider.value);
      if (isFinite(pos)) {
        applyValue(faderSliderPosToValue(spec, pos), true);
      }
    });
    slider.addEventListener("dblclick", () => {
      applyValue(spec.default, true);
      // The two clicks that make up a dblclick each jump the native thumb
      // to the click position first (and focus the slider) before this
      // handler runs — renderPracticeToolFaders() then skips redrawing it because it
      // deliberately never overwrites the focused element mid-drag. Force
      // the visual thumb back to center explicitly so it doesn't end up
      // stuck at the click position while the value/text already reset.
      slider.value = String(faderValueToSliderPos(spec, spec.default));
    });
  }

  if (valueInput && valueInput.dataset.bound !== "true") {
    valueInput.dataset.bound = "true";
    const commit = () => {
      const parsed = spec.parse(valueInput.value);
      if (parsed === null) {
        rerender(); // invalid text — revert to the last real value
        return;
      }
      applyValue(parsed, true);
    };
    valueInput.addEventListener("focus", () => valueInput.select());
    valueInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        valueInput.blur();
      } else if (event.key === "Escape") {
        rerender();
        valueInput.blur();
      }
    });
    valueInput.addEventListener("focusout", commit);
  }
}
