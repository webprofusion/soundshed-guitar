/**
 * The tuner's intonation check.
 *
 * The player tunes an open string and sets it as the reference. Every note they then play on that
 * string is read against it: its distance from the open string, in semitones, names the fret
 * (the nearest whole semitone) and what is left over is how far that fret is out. Only the
 * relative pitch matters, so the reference pitch (A = 440) and how well the open string itself
 * was tuned make no difference to the reading.
 *
 * A plucked note starts sharp and settles, so a fret is measured over the sustain only
 * (NoteSettler): the first few readings of each pluck are skipped and the rest must agree before
 * the reading counts.
 *
 * Mirrored in C++ by core/src/uiclient/Instruments.h (IntonationTest) for Nano Q; keep the two
 * in step.
 */

/** The highest fret read; a note further above the open string is taken as another string. */
export const kIntonationMaxFret = 24;

/** Readings skipped at the start of each pluck (a reading is ~43 ms): the attack reads sharp. */
export const kAttackReadings = 4;
/** Readings of sustain, after the attack, before a measurement counts (~260 ms). */
export const kSettleReadings = 6;
/** The most recent readings of the sustain a measurement is taken over (~690 ms). */
export const kSettleWindow = 16;
/** A reading this many cents or more from the window's median is not steady. */
export const kSettleSpreadCents = 4;
/** At least this share of the window must be steady for the measurement to count. */
export const kSettleSteadyShare = 0.75;
/** A rise in level this large between readings is a new pluck of the same note. */
export const kNewPluckDb = 6;
/** Consecutive readings further apart than this many semitones are different notes. */
const kNewNoteSemitones = 0.5;

export type IntonationQuality = "excellent" | "good" | "fair" | "poor";

/** How good a fret's intonation is, from how many cents out it is either way. */
export function intonationQuality(cents: number): IntonationQuality {
  const off = Math.abs(cents);
  if (off <= 2) return "excellent";
  if (off <= 5) return "good";
  if (off <= 10) return "fair";
  return "poor";
}

export const kIntonationQualityLabels: Record<IntonationQuality, string> = {
  excellent: "Excellent",
  good: "Good",
  fair: "Fair",
  poor: "Poor",
};

export interface FretReading {
  fret: number;
  /** How far the note is from where that fret should be, sharp positive. */
  cents: number;
  /** Where the fret should be: the open string's pitch raised by `fret` semitones. */
  targetFrequency: number;
}

/**
 * The fret a note is nearest on a string tuned to `openFrequency`, and how far out it is. Null for
 * a note below the open string or above the top fret (another string, most likely).
 */
export function fretReading(openFrequency: number, frequency: number): FretReading | null {
  if (!(openFrequency > 0) || !(frequency > 0)) return null;
  const semitones = 12 * Math.log2(frequency / openFrequency);
  const fret = Math.round(semitones);
  if (fret < 0 || fret > kIntonationMaxFret) return null;
  const cents = (semitones - fret) * 100;
  return { fret, cents: cents === 0 ? 0 : cents, targetFrequency: openFrequency * Math.pow(2, fret / 12) };
}

const kSharpNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

/** The nearest note to a frequency against the reference pitch, as "E2", and how far from it. */
export function nearestNote(frequency: number, referenceFrequency = 440): { name: string; cents: number } {
  if (!(frequency > 0) || !(referenceFrequency > 0)) return { name: "-", cents: 0 };
  const midiFloat = 69 + 12 * Math.log2(frequency / referenceFrequency);
  const midi = Math.round(midiFloat);
  const octave = Math.floor(midi / 12) - 1;
  return { name: `${kSharpNames[((midi % 12) + 12) % 12]}${octave}`, cents: (midiFloat - midi) * 100 };
}

/** Frets up to this one are near the nut: their pitch is set more by the nut than the saddle. */
export const kNutFrets = 3;
/** From this fret up the saddle's position dominates; the 12th is the usual check. */
export const kSaddleFrets = 7;

const kSaddleSharp = "Move this string's saddle further from the fretboard (lengthens the string).";
const kSaddleFlat = "Move this string's saddle closer to the fretboard (shortens the string).";
const kNutHigh =
  "The nut slot may be cut too high: fretting near the nut stretches the string sharp. Fret lightly to rule out finger pressure.";

/** What to do about one fret that is out, in a sentence; empty when it is fine. */
export function intonationAdvice(fret: number, cents: number): string {
  const quality = intonationQuality(cents);
  if (quality === "excellent" || quality === "good") return "";
  const sharp = cents > 0;
  if (fret === 0) return "The open string has drifted: retune it and set it again.";
  if (fret <= kNutFrets) {
    return sharp ? kNutHigh : "Flat this close to the nut: check the open string is still in tune.";
  }
  if (fret < kSaddleFrets) {
    return sharp
      ? "Sharp: fret lightly, then check the 12th fret to see whether the saddle needs moving."
      : "Flat: check the 12th fret to see whether the saddle needs moving.";
  }
  return sharp ? kSaddleSharp : kSaddleFlat;
}

/**
 * What a string's frets say together, a sentence each: the saddle, from its frets at the 7th and
 * up (the one nearest the 12th); the nut, from frets near it that are sharp by more than the
 * frets higher up are, since a high nut slot sharpens the low frets and leaves the 12th alone.
 */
export function stringAdvice(frets: readonly IntonationFret[]): string[] {
  const out: string[] = [];
  const high = frets.filter((f) => f.fret >= kSaddleFrets);
  const saddle = high.length
    ? high.reduce((best, f) => (Math.abs(f.fret - 12) < Math.abs(best.fret - 12) ? f : best))
    : null;
  if (saddle && intonationQuality(saddle.cents) !== "excellent" && intonationQuality(saddle.cents) !== "good") {
    out.push(saddle.cents > 0 ? kSaddleSharp : kSaddleFlat);
  } else if (saddle) {
    out.push(`Saddle is right: fret ${saddle.fret} is within ${Math.ceil(Math.abs(saddle.cents))}¢.`);
  }

  const low = frets.filter((f) => f.fret >= 1 && f.fret <= kNutFrets);
  if (low.length) {
    const lowCents = low.reduce((sum, f) => sum + f.cents, 0) / low.length;
    const highCents = saddle ? saddle.cents : 0;
    if (lowCents > 5 && lowCents - highCents > 4) out.push(kNutHigh);
  }
  return out;
}

export interface PitchReading {
  detected: boolean;
  frequency?: number;
  levelDb?: number;
}

/** A held note's pitch measured over its sustain. */
export interface SteadyNote {
  frequency: number;
  /** How many readings of the sustain it is taken over. */
  readings: number;
}

function median(values: readonly number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

/**
 * Turns the tuner's readings into steady measurements of each held note. A note runs from the
 * first reading that finds it until the signal goes, the pitch jumps to another note, or the
 * level jumps (a new pluck); its attack is skipped and its sustain measured.
 */
export class NoteSettler {
  private seen = 0;
  private lastSemitones: number | null = null;
  private lastLevelDb: number | null = null;
  private readonly window: number[] = [];

  reset(): void {
    this.seen = 0;
    this.lastSemitones = null;
    this.lastLevelDb = null;
    this.window.length = 0;
  }

  /** One reading in; the note's measurement so far, once it is steady. */
  push(reading: PitchReading): SteadyNote | null {
    const frequency = reading.frequency ?? 0;
    if (!reading.detected || !(frequency > 0)) {
      this.reset();
      return null;
    }

    const semitones = 12 * Math.log2(frequency / 440);
    const level = reading.levelDb;
    const jumped = this.lastSemitones !== null && Math.abs(semitones - this.lastSemitones) > kNewNoteSemitones;
    const plucked =
      level !== undefined && this.lastLevelDb !== null && level - this.lastLevelDb >= kNewPluckDb;
    if (jumped || plucked) {
      this.reset();
    }
    this.lastSemitones = semitones;
    this.lastLevelDb = level ?? null;

    this.seen += 1;
    if (this.seen <= kAttackReadings) return null;

    this.window.push(semitones);
    if (this.window.length > kSettleWindow) this.window.shift();
    if (this.window.length < kSettleReadings) return null;

    const centre = median(this.window);
    const steady = this.window.filter((s) => Math.abs(s - centre) * 100 < kSettleSpreadCents);
    if (steady.length < Math.ceil(this.window.length * kSettleSteadyShare)) return null;

    const mean = steady.reduce((sum, s) => sum + s, 0) / steady.length;
    return { frequency: 440 * Math.pow(2, mean / 12), readings: steady.length };
  }
}

export interface IntonationFret {
  fret: number;
  cents: number;
  quality: IntonationQuality;
}

/** One string checked: its open pitch and the frets measured on it, lowest fret first. */
export interface IntonationString {
  /** The open string's nearest note, as "E2"; a string is known by it. */
  name: string;
  openFrequency: number;
  frets: IntonationFret[];
}

export interface IntonationMeasurement extends IntonationFret {
  frequency: number;
  targetFrequency: number;
}

export interface IntonationView {
  /** "open" until an open string is set, then "fret". */
  phase: "open" | "fret";
  /** The string being checked, once its open pitch is set. */
  string: IntonationString | null;
  /** The last steady note heard, which "Set open string" would take. */
  candidate: { name: string; frequency: number; cents: number } | null;
  /** The current note's steady measurement against the open string, while it lasts. */
  measurement: IntonationMeasurement | null;
  /** A steady note that is no fret of this string: below it or above the top fret. */
  offString: boolean;
  /** Every string checked, lowest first. */
  strings: IntonationString[];
}

/**
 * The intonation check's state: the open string set, what has been measured on each string, and
 * the latest note heard.
 */
export class IntonationTest {
  private readonly settler = new NoteSettler();
  private readonly checked: IntonationString[] = [];
  private current: IntonationString | null = null;
  private lastSteady: SteadyNote | null = null;
  private measurement: IntonationMeasurement | null = null;
  private offString = false;
  private referenceFrequency = 440;

  setReferenceFrequency(hz: number): void {
    if (hz > 0) this.referenceFrequency = hz;
  }

  /** One tuner reading in. */
  push(reading: PitchReading): IntonationView {
    const steady = this.settler.push(reading);
    if (!steady) {
      // Between notes the last measurement stays up, so it can be read after the note dies.
      if (reading.detected) this.offString = false;
      return this.view();
    }

    this.lastSteady = steady;
    this.measurement = null;
    this.offString = false;
    if (this.current) {
      const fret = fretReading(this.current.openFrequency, steady.frequency);
      if (!fret) {
        this.offString = true;
      } else {
        this.measurement = {
          fret: fret.fret,
          cents: fret.cents,
          quality: intonationQuality(fret.cents),
          frequency: steady.frequency,
          targetFrequency: fret.targetFrequency,
        };
        if (fret.fret > 0) this.record(this.current, this.measurement);
      }
    }
    return this.view();
  }

  /** Takes the last steady note as the open string. False when no note has been heard yet. */
  setOpenString(): boolean {
    if (!this.lastSteady) return false;
    const frequency = this.lastSteady.frequency;
    const name = nearestNote(frequency, this.referenceFrequency).name;
    // Setting a string again (retuned, or a saddle moved) starts its frets over: the old ones
    // were measured against the old pitch.
    const existing = this.checked.findIndex((s) => s.name === name);
    const string: IntonationString = { name, openFrequency: frequency, frets: [] };
    if (existing >= 0) this.checked.splice(existing, 1, string);
    else this.checked.push(string);
    this.checked.sort((a, b) => a.openFrequency - b.openFrequency);
    this.current = string;
    this.measurement = null;
    this.offString = false;
    return true;
  }

  /** Back to setting an open string, keeping what has been measured. */
  newString(): void {
    this.current = null;
    this.measurement = null;
    this.offString = false;
  }

  /** Forgets every string measured. */
  clear(): void {
    this.checked.length = 0;
    this.current = null;
    this.measurement = null;
    this.offString = false;
    this.lastSteady = null;
    this.settler.reset();
  }

  /** A new run of readings (the tuner reopened): the note in progress is forgotten. */
  resetReadings(): void {
    this.settler.reset();
    this.measurement = null;
    this.offString = false;
  }

  view(): IntonationView {
    const candidate = this.lastSteady
      ? {
          ...nearestNote(this.lastSteady.frequency, this.referenceFrequency),
          frequency: this.lastSteady.frequency,
        }
      : null;
    return {
      phase: this.current ? "fret" : "open",
      string: this.current,
      candidate,
      measurement: this.measurement,
      offString: this.offString,
      strings: this.checked,
    };
  }

  private record(string: IntonationString, measurement: IntonationMeasurement): void {
    const entry: IntonationFret = { fret: measurement.fret, cents: measurement.cents, quality: measurement.quality };
    const index = string.frets.findIndex((f) => f.fret === measurement.fret);
    if (index >= 0) string.frets[index] = entry;
    else {
      string.frets.push(entry);
      string.frets.sort((a, b) => a.fret - b.fret);
    }
  }
}
