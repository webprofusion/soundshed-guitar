/**
 * The player's own chord progressions for the Scale Guide, kept in the `scaleGuide.progressions`
 * app setting as roman numerals ("I", "bVII7", "iiø7"), which is what the engine parses
 * (music/MusicTheory.h ParseRoman). A key-free progression plays in whatever key is chosen.
 */

import { getAppSetting, updateAppSetting } from "../appSettingsStore.js";
import type { AppSettingValue } from "../types.js";

export const PROGRESSIONS_SETTING = "scaleGuide.progressions";
export const MAX_PROGRESSION_CHORDS = 16;
export const MAX_PROGRESSION_NAME = 40;

export interface UserProgression {
  id: string;
  name: string;
  /** Roman numerals, one per chord. */
  chords: string[];
}

/** The twelve degrees a chord can sit on, by semitones above the key. */
export const DEGREE_NUMERALS = ["I", "bII", "II", "bIII", "III", "IV", "bV", "V", "bVI", "VI", "bVII", "VII"] as const;

/** How each chord quality is written after its numeral, and whether the numeral is lower case. */
const QUALITY_SUFFIXES: Readonly<Record<string, readonly [suffix: string, minor: boolean]>> = {
  maj: ["", false],
  m: ["", true],
  "5": ["5", false],
  sus2: ["sus2", false],
  sus4: ["sus4", false],
  dim: ["°", true],
  aug: ["+", false],
  "6": ["6", false],
  m6: ["6", true],
  "7": ["7", false],
  maj7: ["maj7", false],
  m7: ["7", true],
  mMaj7: ["maj7", true],
  m7b5: ["ø7", true],
  dim7: ["°7", true],
  aug7: ["+7", false],
  "7sus4": ["7sus4", false],
  add9: ["add9", false],
  "9": ["9", false],
  "7#9": ["7#9", false],
  maj9: ["maj9", false],
  m9: ["9", true],
};

const TOKEN = /^[b#]?(?:VII|VI|V|IV|III|II|I|vii|vi|v|iv|iii|ii|i)[\w°ø+#]{0,6}$/;

/** The numeral for a chord `offset` semitones above the key with quality `quality`. */
export function romanToken(offset: number, quality: string): string {
  const degree = DEGREE_NUMERALS[((offset % 12) + 12) % 12];
  const [suffix, minor] = QUALITY_SUFFIXES[quality] ?? QUALITY_SUFFIXES.maj;
  const flat = degree.startsWith("b") ? "b" : "";
  const numeral = degree.slice(flat.length);
  return flat + (minor ? numeral.toLowerCase() : numeral) + suffix;
}

export function isRomanToken(value: unknown): value is string {
  return typeof value === "string" && TOKEN.test(value);
}

export function normalizeProgressions(value: unknown): UserProgression[] {
  if (!Array.isArray(value)) {
    return [];
  }

  const seen = new Set<string>();
  const progressions: UserProgression[] = [];

  for (const entry of value) {
    if (entry === null || typeof entry !== "object") {
      continue;
    }

    const record = entry as Record<string, unknown>;
    const id = typeof record.id === "string" && /^[\w-]{1,32}$/.test(record.id) ? record.id : "";
    const chords = Array.isArray(record.chords) ? record.chords.filter(isRomanToken).slice(0, MAX_PROGRESSION_CHORDS) : [];

    if (!id || seen.has(id) || chords.length === 0) {
      continue;
    }

    seen.add(id);
    const name = typeof record.name === "string" ? record.name.trim().slice(0, MAX_PROGRESSION_NAME) : "";
    progressions.push({ id, name: name || "My progression", chords });
  }

  return progressions;
}

export function loadProgressions(): UserProgression[] {
  return normalizeProgressions(getAppSetting(PROGRESSIONS_SETTING));
}

export function saveProgressions(progressions: UserProgression[]): void {
  updateAppSetting(PROGRESSIONS_SETTING, progressions.map((p) => ({ ...p, chords: [...p.chords] })) as unknown as AppSettingValue);
}

/** An id no saved progression has. */
export function newProgressionId(existing: readonly UserProgression[]): string {
  let n = existing.length + 1;

  while (existing.some((p) => p.id === `p${n}`)) {
    n += 1;
  }

  return `p${n}`;
}
