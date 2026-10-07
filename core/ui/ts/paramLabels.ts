/**
 * An enum parameter's label for a value.
 *
 * Labels are listed in value order from the parameter's minimum, one per step, as the engine
 * declares them (EffectParamSpec.h: "an enum's choices, in value order"). Indexing them by the
 * value itself only works for an enum that starts at 0: the Auto Arpeggiator's Steps runs 2 to 8,
 * and showed "6" at 4. The Nano UI does the same in EnumLabelIndex (uiclient/ParamFormat.h).
 */
export function enumLabel(value: number, labels: readonly string[], min = 0, step?: number): string | undefined {
  const index = Math.round((value - min) / (step !== undefined && step > 0 ? step : 1));
  return index >= 0 ? labels[index] : undefined;
}

/**
 * A parameter that moves in whole steps (a MIDI channel, semitones, a count) as a whole number
 * with its unit, where it showed "1.00" or "-12.00st"; undefined for any other parameter, or a
 * value off the whole numbers. The Nano UI does the same in FormatParamValue (uiclient/ParamFormat.cpp).
 */
export function wholeStepLabel(value: number, unit?: string, step?: number): string | undefined {
  if (step === undefined || !Number.isInteger(step) || step < 1) return undefined;
  if (unit === "enum" || unit === "toggle" || unit === "pan") return undefined;
  const rounded = Math.round(value);
  if (Math.abs(value - rounded) > 1e-6) return undefined;
  return `${rounded}${!unit || unit === "amount" ? "" : unit}`;
}
