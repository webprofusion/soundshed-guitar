/**
 * The Scale Guide's choices, kept in the `scaleGuide.view` app setting so the standalone, every
 * plugin instance and Nano Q open on the same key, scale and tuning.
 */

import { getAppSetting, updateAppSetting } from "../appSettingsStore.js";
import type { AppSettingValue } from "../types.js";
import type { ArpeggioStyle, ScaleGuideLabels, ScaleGuideMode, ScaleGuideSettings } from "./types.js";

export const SCALE_GUIDE_SETTING = "scaleGuide.view";
export const MAX_CAPO = 12;

export const DEFAULT_SETTINGS: Readonly<ScaleGuideSettings> = {
  mode: "scale",
  key: 4,
  scale: "minpent",
  chord: "maj",
  progression: "",
  tuning: "std",
  labels: "name",
  position: 0,
  capo: 0,
  leftHanded: false,
  listen: false,
  match: "normal",
  arpeggioStyle: "sweep",
  arpeggioStrings: 0,
};

const LABELS: readonly ScaleGuideLabels[] = ["name", "interval", "degree", "none", "order", "finger"];
const MODES: readonly ScaleGuideMode[] = ["scale", "chord", "arpeggio"];
const ARPEGGIO_STYLES: readonly ArpeggioStyle[] = ["sweep", "two", "box"];
const MAX_STRINGS = 8;
const MATCHES: readonly ScaleGuideSettings["match"][] = ["easy", "normal", "strict"];
const ID = /^[\w-]{1,32}$/;
const PROGRESSION = /^[bu]:[\w-]{1,32}$/;

function wholeNumber(value: unknown, fallback: number, lowest: number, highest: number): number {
  return typeof value === "number" && Number.isInteger(value) ? Math.min(highest, Math.max(lowest, value)) : fallback;
}

function id(value: unknown, fallback: string): string {
  return typeof value === "string" && ID.test(value) ? value : fallback;
}

/** A stored value made safe to use: anything missing or malformed takes its default. */
export function normalizeSettings(value: unknown): ScaleGuideSettings {
  const stored = value !== null && typeof value === "object" && !Array.isArray(value) ? (value as Record<string, unknown>) : {};
  const defaults = DEFAULT_SETTINGS;
  return {
    mode: MODES.includes(stored.mode as ScaleGuideMode) ? (stored.mode as ScaleGuideMode) : defaults.mode,
    key: wholeNumber(stored.key, defaults.key, 0, 11),
    scale: id(stored.scale, defaults.scale),
    chord: id(stored.chord, defaults.chord),
    progression: typeof stored.progression === "string" && PROGRESSION.test(stored.progression) ? stored.progression : "",
    tuning: id(stored.tuning, defaults.tuning),
    labels: LABELS.includes(stored.labels as ScaleGuideLabels) ? (stored.labels as ScaleGuideLabels) : defaults.labels,
    position: wholeNumber(stored.position, defaults.position, 0, 99),
    capo: wholeNumber(stored.capo, defaults.capo, 0, MAX_CAPO),
    leftHanded: typeof stored.leftHanded === "boolean" ? stored.leftHanded : defaults.leftHanded,
    listen: typeof stored.listen === "boolean" ? stored.listen : defaults.listen,
    match: MATCHES.includes(stored.match as ScaleGuideSettings["match"]) ? (stored.match as ScaleGuideSettings["match"]) : defaults.match,
    arpeggioStyle: ARPEGGIO_STYLES.includes(stored.arpeggioStyle as ArpeggioStyle) ? (stored.arpeggioStyle as ArpeggioStyle) : defaults.arpeggioStyle,
    arpeggioStrings: wholeNumber(stored.arpeggioStrings, defaults.arpeggioStrings, 0, MAX_STRINGS),
  };
}

export function loadSettings(): ScaleGuideSettings {
  return normalizeSettings(getAppSetting(SCALE_GUIDE_SETTING));
}

export function saveSettings(settings: ScaleGuideSettings): void {
  updateAppSetting(SCALE_GUIDE_SETTING, { ...settings } as unknown as AppSettingValue);
}

/** What a request adds for a progression: its chords (or a built-in's id) and the step showing. */
export interface ProgressionRequest {
  progression?: string;
  progressionChords?: string[];
  progressionName?: string;
  step?: number;
  /** The shape last shown, so the next chord takes the shape nearest it. */
  previousFrets?: number[];
}

/** The engine request for these choices (core/src/music/ScaleGuideView.h). */
export function viewRequest(settings: ScaleGuideSettings, progression: ProgressionRequest = {}): Record<string, unknown> {
  return {
    mode: settings.mode,
    key: settings.key,
    scale: settings.scale,
    chord: settings.chord,
    tuning: settings.tuning,
    capo: settings.capo,
    position: settings.position,
    arpeggioStyle: settings.arpeggioStyle,
    arpeggioStrings: settings.arpeggioStrings,
    ...progression,
  };
}
