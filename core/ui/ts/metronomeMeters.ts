/**
 * Meter catalogue and accent-pattern helpers for the metronome.
 *
 * These mirror `core/src/controller/internal/MetronomeSupport.h` exactly: the
 * engine normalises whatever it is sent, so a UI that derives a different
 * default pattern would show one thing and play another. Any change here needs
 * the same change there (and both are covered by tests).
 *
 * A pattern is one character per beat — H accent, M medium, L normal, S silent —
 * and a grouping is "2+2+3", the beat clusters an odd meter falls into.
 */

export type BeatLevel = "accent" | "medium" | "normal" | "off";

export interface TimeSignatureOption {
  /** Stable id, also what the picker keys on: "7/8:2+2+3". */
  id: string;
  label: string;
  num: number;
  den: number;
  /** Empty when the meter divides evenly. */
  grouping: string;
  /** Which column of the picker this belongs in. */
  family: "simple" | "compound";
}

export interface SubdivisionOption {
  id: string;
  ticksPerBeat: number;
}

const LEVEL_CHARS: Record<BeatLevel, string> = {
  accent: "H",
  medium: "M",
  normal: "L",
  off: "S",
};

/** Clicking a beat walks these in order, loudest first. */
const LEVEL_CYCLE: BeatLevel[] = ["accent", "medium", "normal", "off"];

export const MAX_BEATS_PER_BAR = 16;

function meterId(num: number, den: number, grouping: string): string {
  return grouping ? `${num}/${den}:${grouping}` : `${num}/${den}`;
}

function simpleMeter(num: number, den: number): TimeSignatureOption {
  return { id: meterId(num, den, ""), label: `${num}/${den}`, num, den, grouping: "", family: "simple" };
}

function compoundMeter(num: number, den: number, grouping = ""): TimeSignatureOption {
  return {
    id: meterId(num, den, grouping),
    label: grouping ? `${num}/${den} (${grouping})` : `${num}/${den}`,
    num,
    den,
    grouping,
    family: "compound",
  };
}

/** Everything the meter picker offers, in the order it shows them. */
export const TIME_SIGNATURES: TimeSignatureOption[] = [
  simpleMeter(2, 4),
  simpleMeter(3, 4),
  simpleMeter(4, 4),
  simpleMeter(5, 4),
  simpleMeter(6, 4),
  simpleMeter(7, 4),
  simpleMeter(8, 4),
  simpleMeter(9, 4),
  simpleMeter(10, 4),
  simpleMeter(11, 4),
  simpleMeter(12, 4),
  simpleMeter(13, 4),
  compoundMeter(3, 8),
  compoundMeter(6, 8),
  compoundMeter(9, 8),
  compoundMeter(12, 8),
  compoundMeter(5, 8, "3+2"),
  compoundMeter(5, 8, "2+3"),
  compoundMeter(7, 8, "3+2+2"),
  compoundMeter(7, 8, "2+3+2"),
  compoundMeter(7, 8, "2+2+3"),
];

/** Used until the engine sends its own list. */
export const DEFAULT_SUBDIVISIONS: SubdivisionOption[] = [
  { id: "1/4", ticksPerBeat: 1 },
  { id: "1/8", ticksPerBeat: 2 },
  { id: "1/8T", ticksPerBeat: 3 },
  { id: "1/16", ticksPerBeat: 4 },
  { id: "1/16T", ticksPerBeat: 6 },
  { id: "1/32", ticksPerBeat: 8 },
];

export function clampBeatsPerBar(beats: number): number {
  if (!Number.isFinite(beats) || beats < 1) return 1;
  return Math.min(MAX_BEATS_PER_BAR, Math.floor(beats));
}

/**
 * Splits "2+2+3" into beat counts, or returns an empty array when the text is
 * malformed or does not add up to the bar — the engine drops an unusable
 * grouping rather than half-applying it, and so must the UI.
 */
export function parseGrouping(grouping: string, beatsPerBar: number): number[] {
  if (!grouping) return [];
  const parts = grouping.split(/[+,\s-]+/).filter((part) => part.length > 0);
  if (!parts.length) return [];

  const groups: number[] = [];
  for (const part of parts) {
    if (!/^\d+$/.test(part)) return [];
    const value = Number(part);
    if (value <= 0) return [];
    groups.push(value);
  }

  const total = groups.reduce((sum, value) => sum + value, 0);
  return total === clampBeatsPerBar(beatsPerBar) ? groups : [];
}

/** Compound meters fall into threes; everything else stays flat. */
export function impliedGrouping(beatsPerBar: number, den: number): number[] {
  const beats = clampBeatsPerBar(beatsPerBar);
  if (den === 8 && beats > 3 && beats % 3 === 0) {
    return new Array<number>(beats / 3).fill(3);
  }
  return [];
}

/** Beat 1 accents, later group heads take a medium accent, the rest are normal. */
export function defaultBeatPattern(num: number, den: number, grouping: string): string {
  const beats = clampBeatsPerBar(num);
  const groups = parseGrouping(grouping, beats).length ? parseGrouping(grouping, beats) : impliedGrouping(beats, den);

  const pattern = new Array<string>(beats).fill("L");
  pattern[0] = "H";

  let cursor = 0;
  for (let i = 0; i + 1 < groups.length; i += 1) {
    cursor += groups[i];
    if (cursor > 0 && cursor < beats) pattern[cursor] = "M";
  }

  return pattern.join("");
}

export function beatLevelFromChar(raw: string): BeatLevel {
  switch (raw.toUpperCase()) {
    case "H":
      return "accent";
    case "M":
      return "medium";
    case "S":
    case "-":
    case ".":
      return "off";
    default:
      return "normal";
  }
}

export function charFromBeatLevel(level: BeatLevel): string {
  return LEVEL_CHARS[level] ?? "L";
}

/** Forces a pattern to one character per beat, repeating or cutting as needed. */
export function normaliseBeatPattern(raw: string, num: number, den: number, grouping: string): string {
  const beats = clampBeatsPerBar(num);
  const filtered = (raw ?? "")
    .toUpperCase()
    .split("")
    .filter((ch) => "HLMS-.".includes(ch))
    .map((ch) => charFromBeatLevel(beatLevelFromChar(ch)));

  if (!filtered.length) return defaultBeatPattern(beats, den, grouping);

  const pattern: string[] = [];
  for (let i = 0; i < beats; i += 1) pattern.push(filtered[i % filtered.length]);
  return pattern.join("");
}

export function beatLevels(pattern: string, num: number, den: number, grouping: string): BeatLevel[] {
  return normaliseBeatPattern(pattern, num, den, grouping)
    .split("")
    .map((ch) => beatLevelFromChar(ch));
}

export function patternFromLevels(levels: BeatLevel[]): string {
  return levels.map((level) => charFromBeatLevel(level)).join("");
}

/** The next level a click on a beat should select. */
export function nextBeatLevel(level: BeatLevel): BeatLevel {
  const index = LEVEL_CYCLE.indexOf(level);
  return LEVEL_CYCLE[(index + 1) % LEVEL_CYCLE.length];
}

/**
 * Beat indexes that start a group, used to draw the gaps between clusters.
 * Beat 0 is always a group head and is not included.
 */
export function groupHeads(num: number, den: number, grouping: string): Set<number> {
  const beats = clampBeatsPerBar(num);
  const parsed = parseGrouping(grouping, beats);
  const groups = parsed.length ? parsed : impliedGrouping(beats, den);
  const heads = new Set<number>();

  let cursor = 0;
  for (let i = 0; i + 1 < groups.length; i += 1) {
    cursor += groups[i];
    if (cursor > 0 && cursor < beats) heads.add(cursor);
  }

  return heads;
}

/** The catalogue entry matching a meter, or undefined for a custom one. */
export function findTimeSignature(num: number, den: number, grouping: string): TimeSignatureOption | undefined {
  return TIME_SIGNATURES.find((option) => option.num === num && option.den === den && option.grouping === grouping);
}

/** Label for a meter the catalogue does not list. */
export function describeTimeSignature(num: number, den: number, grouping: string): string {
  const known = findTimeSignature(num, den, grouping);
  if (known) return known.label;
  return grouping ? `${num}/${den} (${grouping})` : `${num}/${den}`;
}
