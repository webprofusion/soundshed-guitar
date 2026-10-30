/**
 * How a parameter's value is shown under its control: the same text in the params panel, a
 * custom layout and the layout designer's preview.
 */

import { enumLabel, wholeStepLabel } from "./paramLabels.js";
import { formatTaperedValue, parseParamTaper, type ParamTaper } from "./paramTaper.js";

/**
 * Format a parameter value for display.
 * Returns just the number for generic "amount" units to keep labels compact.
 * Exported so the signal-path renderer and designer preview share identical formatting.
 * A log-taper value, which can run from 1.25 to 18,000, is shown to about three figures, and
 * one that moves in whole steps as a whole number with its unit (wholeStepLabel).
 */
export function formatParamValue(
  value: number,
  unit?: string,
  labels?: string[],
  taper?: ParamTaper,
  min = 0,
  step?: number,
): string {
  if (unit === "toggle") {
    return value >= 0.5 ? "On" : "Off";
  }
  if (unit === "enum" && Array.isArray(labels)) {
    return enumLabel(value, labels, min, step) ?? `${Math.round(value)}`;
  }
  // Whole steps first, so a stepped count on a log taper (the Dispersal's Stages) reads "64".
  const whole = wholeStepLabel(value, unit, step);
  if (whole !== undefined) {
    return whole;
  }
  if (taper === "log") {
    return formatTaperedValue(value, unit);
  }
  if (unit === "dB" || unit === "ms" || unit === "Hz") {
    return `${value.toFixed(1)}${unit}`;
  }
  return value.toFixed(2);
}

/**
 * How a bound knob shows its value: read from the knob's data attributes, as the params panel
 * binds it, so the layout designer draws the same text as the panel. It differs from
 * formatParamValue, which the markup carries until the knob is bound: two decimals with any unit,
 * pan as C/L/R, and a blend knob to one decimal.
 */
export function knobValueFormatter(knob: HTMLElement): (value: number) => string {
  const min = parseFloat(knob.dataset.min || "0");
  const unit = knob.dataset.unit || "amount";
  const step = knob.dataset.step ? parseFloat(knob.dataset.step) : undefined;
  const labels = (knob.dataset.labels || "").split("|").filter(Boolean);
  const isEnum = unit === "enum" && labels.length > 0;
  const isBlendParam = knob.dataset.blendParam === "true";
  const taper = parseParamTaper(knob.dataset.taper);
  return (rawValue) => {
    if (isEnum) {
      return enumLabel(rawValue, labels, min, step) ?? `${Math.round(rawValue)}`;
    }
    if (isBlendParam) {
      return rawValue.toFixed(1);
    }
    if (unit === "pan") {
      if (Math.abs(rawValue) < 0.01) return "C";
      return rawValue < 0
        ? `L${Math.abs(rawValue * 100).toFixed(0)}`
        : `R${(rawValue * 100).toFixed(0)}`;
    }
    const whole = wholeStepLabel(rawValue, unit, step);
    if (whole !== undefined) {
      return whole;
    }
    if (taper === "log") {
      return formatTaperedValue(rawValue, unit);
    }
    return `${rawValue.toFixed(2)}${unit === "amount" ? "" : unit}`;
  };
}
