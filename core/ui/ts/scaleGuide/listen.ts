/**
 * The Scale Guide's LISTEN, as logic apart from the page: which note is being played (scales) and
 * whether a strummed chord rings clean (chords). Fed by `tunerUpdate`, which the engine sends about
 * every 43 ms while the guide listens (`tuner {action: "listen"}`), with a 12-bin `chroma` (the
 * input's energy per pitch class, C first) in Chords mode.
 */

export type Strictness = "easy" | "normal" | "strict";

/** How much of the chroma the chord's tones must hold, and how strong each must be. */
export const STRICTNESS: Readonly<Record<Strictness, { share: number; tone: number }>> = {
  easy: { share: 0.6, tone: 0.03 },
  normal: { share: 0.7, tone: 0.05 },
  strict: { share: 0.8, tone: 0.07 },
};

const NOTE_PITCH_CLASSES: Readonly<Record<string, number>> = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };

/** The tuner's note ("C#/Db", octave 3) as a MIDI note, or null. */
export function readingMidi(noteName: unknown, octave: unknown): number | null {
  if (typeof noteName !== "string" || typeof octave !== "number" || !Number.isFinite(octave)) {
    return null;
  }

  const match = /^([A-G])(#?)/.exec(noteName);

  if (!match) {
    return null;
  }

  const pitchClass = (NOTE_PITCH_CLASSES[match[1]] + (match[2] ? 1 : 0)) % 12;
  return (octave + 1) * 12 + pitchClass;
}

export interface ChordTone {
  pitchClass: number;
  /** "R", "3", "♭3", "5" … */
  interval: string;
}

export interface ChordMatch {
  /** The chord's tones' share of the chroma, 0-1. */
  share: number;
  /** Per tone, in the order given: heard strongly enough. */
  heard: boolean[];
  /** The tones that must sound and do not. */
  missing: number[];
  ok: boolean;
}

/**
 * Whether `chroma` is the chord: its tones hold at least the strictness's share, and each tone (bar
 * a plain 5th, which a chord can lose) reads at least the strictness's level.
 */
export function matchChord(chroma: readonly number[], rootPitchClass: number, tones: readonly ChordTone[], strictness: Strictness): ChordMatch {
  const rule = STRICTNESS[strictness];
  const total = chroma.reduce((sum, value) => sum + value, 0);
  const at = (pitchClass: number): number => (total > 0 ? (chroma[((pitchClass % 12) + 12) % 12] ?? 0) / total : 0);
  const intervals = [...new Set(tones.map((tone) => (((tone.pitchClass - rootPitchClass) % 12) + 12) % 12))];
  const shareOf = (set: readonly number[]): number => set.reduce((sum, interval) => sum + at(rootPitchClass + interval), 0);
  const share = shareOf(intervals);
  const heard = tones.map((tone) => at(tone.pitchClass) >= rule.tone);
  const optional = (tone: ChordTone): boolean => tone.interval === "5" && intervals.length > 2;
  const missing = tones.filter((tone, index) => !heard[index] && !optional(tone)).map((tone) => tone.pitchClass);
  return { share, heard, missing, ok: total > 0 && share >= rule.share && missing.length === 0 };
}

/**
 * Turns clean readings into strums: a chord counts once it has read clean for HOLD readings in a
 * row (about 130 ms), and the next count needs a fresh strum, a jump in level after a short lockout
 * (REARM_READINGS, about 340 ms), so a repeated chord (a 12-bar's I7 I7) steps once per strum
 * rather than racing ahead.
 */
export class StrumGate {
  static readonly HOLD = 3;
  static readonly JUMP_DB = 3;
  static readonly REARM_READINGS = 8;
  static readonly QUIET_DB = -50;
  private levels: number[] = [];
  private armed = true;
  private held = 0;
  private rearmWait = 0;

  /** True when this reading completes a clean strum. */
  push(levelDb: number, clean: boolean): boolean {
    this.levels.push(levelDb);

    if (this.levels.length > 10) {
      this.levels.shift();
    }

    this.rearmWait = Math.max(0, this.rearmWait - 1);

    if (this.rearmWait === 0 && levelDb > StrumGate.QUIET_DB && levelDb - Math.min(...this.levels) >= StrumGate.JUMP_DB) {
      this.armed = true;
    }

    this.held = clean && this.armed ? this.held + 1 : 0;

    if (this.held >= StrumGate.HOLD) {
      this.held = 0;
      this.armed = false;
      // The quiet before this strum must not read as a jump while it still rings.
      this.levels = [levelDb];
      this.rearmWait = StrumGate.REARM_READINGS;
      return true;
    }

    return false;
  }

  reset(): void {
    this.levels = [];
    this.armed = true;
    this.held = 0;
    this.rearmWait = 0;
  }
}

/**
 * The note being played, steadied: a note must read the same twice before it shows, so a pick's
 * attack or an octave blip does not flicker; a few silent readings let it go; the last two notes
 * stay as a trail until a long silence clears it.
 */
export class NoteFollower {
  static readonly RELEASE_READINGS = 4;
  static readonly CLEAR_READINGS = 190;
  current: number | null = null;
  trail: number[] = [];
  private candidate: number | null = null;
  private seen = 0;
  private silent = 0;

  /** Feeds one reading (null for none); true when what should show has changed. */
  push(midi: number | null): boolean {
    if (midi === null) {
      this.silent += 1;
      this.candidate = null;

      if (this.current !== null && this.silent >= NoteFollower.RELEASE_READINGS) {
        this.trail = [this.current, ...this.trail.filter((note) => note !== this.current)].slice(0, 2);
        this.current = null;
        return true;
      }

      if (this.current === null && this.trail.length > 0 && this.silent >= NoteFollower.CLEAR_READINGS) {
        this.trail = [];
        return true;
      }

      return false;
    }

    this.silent = 0;

    if (midi === this.current) {
      return false;
    }

    this.seen = midi === this.candidate ? this.seen + 1 : 1;
    this.candidate = midi;

    if (this.seen < 2) {
      return false;
    }

    if (this.current !== null) {
      this.trail = [this.current, ...this.trail.filter((note) => note !== this.current && note !== midi)].slice(0, 2);
    } else {
      this.trail = this.trail.filter((note) => note !== midi);
    }

    this.current = midi;
    this.candidate = null;
    return true;
  }

  reset(): void {
    this.current = null;
    this.trail = [];
    this.candidate = null;
    this.seen = 0;
    this.silent = 0;
  }
}
