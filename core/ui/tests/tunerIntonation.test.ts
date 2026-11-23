import { describe, expect, it } from "vitest";

import {
  IntonationTest,
  NoteSettler,
  fretReading,
  intonationAdvice,
  intonationQuality,
  kAttackReadings,
  kSettleReadings,
  nearestNote,
  stringAdvice,
  type PitchReading,
} from "../ts/tunerIntonation.js";

const kLowE = 82.4069;

function cents(hz: number, c: number): number {
  return hz * Math.pow(2, c / 1200);
}

/** A plucked note as the tuner reads it: an attack that starts sharp, then a steady sustain. */
function pluck(hz: number, sustain = 12, levelDb = -20): PitchReading[] {
  const out: PitchReading[] = [];
  for (let i = 0; i < kAttackReadings; i++) {
    out.push({ detected: true, frequency: cents(hz, 30 - i * 7), levelDb });
  }
  for (let i = 0; i < sustain; i++) {
    // ±1 cent of scatter, as a held note reads.
    out.push({ detected: true, frequency: cents(hz, i % 2 ? 1 : -1), levelDb: levelDb - i * 0.3 });
  }
  return out;
}

const silence: PitchReading = { detected: false };

function play(test: IntonationTest, readings: PitchReading[]) {
  let view = test.view();
  for (const r of readings) view = test.push(r);
  return view;
}

describe("fretReading", () => {
  it("names the fret from the pitch relative to the open string", () => {
    const r = fretReading(kLowE, kLowE * 2);
    expect(r?.fret).toBe(12);
    expect(r?.cents).toBeCloseTo(0, 6);
    expect(fretReading(kLowE, kLowE * Math.pow(2, 5 / 12))?.fret).toBe(5);
  });

  it("reads how far the fret is out, sharp positive", () => {
    const r = fretReading(kLowE, cents(kLowE * 2, 7));
    expect(r?.fret).toBe(12);
    expect(r?.cents).toBeCloseTo(7, 6);
    expect(r?.targetFrequency).toBeCloseTo(kLowE * 2, 6);
    expect(fretReading(kLowE, cents(kLowE * 2, -12))?.cents).toBeCloseTo(-12, 6);
  });

  it("does not care how well the open string was tuned", () => {
    const open = cents(kLowE, 18);
    expect(fretReading(open, cents(open * 2, 3))?.cents).toBeCloseTo(3, 6);
  });

  it("rejects notes below the open string or past the top fret", () => {
    expect(fretReading(kLowE, kLowE * 0.9)).toBeNull();
    expect(fretReading(kLowE, kLowE * Math.pow(2, 25 / 12))).toBeNull();
    expect(fretReading(kLowE, kLowE * 4)?.fret).toBe(24);
  });
});

describe("intonationQuality and advice", () => {
  it("grades by cents either way", () => {
    expect(intonationQuality(1.5)).toBe("excellent");
    expect(intonationQuality(-4)).toBe("good");
    expect(intonationQuality(8)).toBe("fair");
    expect(intonationQuality(-15)).toBe("poor");
  });

  it("says which way to move the saddle at the 12th fret", () => {
    expect(intonationAdvice(12, 8)).toMatch(/further from the fretboard/);
    expect(intonationAdvice(12, -8)).toMatch(/closer to the fretboard/);
    expect(intonationAdvice(12, 2)).toBe("");
  });

  it("blames the nut for sharp frets near it, not the saddle", () => {
    expect(intonationAdvice(1, 9)).toMatch(/nut slot/);
    expect(intonationAdvice(1, 9)).not.toMatch(/saddle/);
  });

  it("diagnoses a string from its frets together", () => {
    // Saddle fine, first frets sharp: the nut.
    const nut = stringAdvice([
      { fret: 1, cents: 9, quality: "fair" },
      { fret: 12, cents: 1, quality: "excellent" },
    ]);
    expect(nut.join(" ")).toMatch(/Saddle is right/);
    expect(nut.join(" ")).toMatch(/nut slot/);

    // Everything sharp by the same amount: the saddle, and not the nut.
    const saddle = stringAdvice([
      { fret: 2, cents: 8, quality: "fair" },
      { fret: 12, cents: 9, quality: "fair" },
    ]);
    expect(saddle.join(" ")).toMatch(/further from the fretboard/);
    expect(saddle.join(" ")).not.toMatch(/nut slot/);

    // The fret nearest the 12th decides the saddle.
    const nearest = stringAdvice([
      { fret: 7, cents: -1, quality: "excellent" },
      { fret: 12, cents: -9, quality: "fair" },
      { fret: 19, cents: 0, quality: "excellent" },
    ]);
    expect(nearest).toEqual([expect.stringMatching(/closer to the fretboard/)]);
  });
});

describe("nearestNote", () => {
  it("names the low E", () => {
    expect(nearestNote(kLowE).name).toBe("E2");
    expect(nearestNote(440).name).toBe("A4");
    expect(nearestNote(cents(440, 20)).cents).toBeCloseTo(20, 6);
  });
});

describe("NoteSettler", () => {
  it("skips the attack and measures the sustain", () => {
    const settler = new NoteSettler();
    const readings = pluck(kLowE);
    const results = readings.map((r) => settler.push(r));
    // Nothing until the attack is past and the sustain has had time to settle.
    const first = results.findIndex((r) => r !== null);
    expect(first).toBe(kAttackReadings + kSettleReadings - 1);
    const last = results[results.length - 1];
    expect(last && 1200 * Math.log2(last.frequency / kLowE)).toBeCloseTo(0, 1);
  });

  it("starts over on a new pluck of the same note", () => {
    const settler = new NoteSettler();
    for (const r of pluck(kLowE)) settler.push(r);
    // Same pitch, 10 dB louder: a new attack, so nothing until it settles again.
    expect(settler.push({ detected: true, frequency: cents(kLowE, 25), levelDb: -10 })).toBeNull();
  });

  it("does not settle on a wobbling pitch", () => {
    const settler = new NoteSettler();
    let any = false;
    for (let i = 0; i < 30; i++) {
      any ||= settler.push({ detected: true, frequency: cents(kLowE, Math.sin(i) * 25), levelDb: -20 }) !== null;
    }
    expect(any).toBe(false);
  });
});

describe("IntonationTest", () => {
  it("sets the open string from the last steady note, after it has died away", () => {
    const test = new IntonationTest();
    expect(test.setOpenString()).toBe(false);
    play(test, [...pluck(cents(kLowE, 4)), silence, silence]);
    expect(test.view().candidate?.name).toBe("E2");
    expect(test.setOpenString()).toBe(true);
    const view = test.view();
    expect(view.phase).toBe("fret");
    expect(view.string?.name).toBe("E2");
  });

  it("measures frets against the open string and records them per string", () => {
    const test = new IntonationTest();
    const open = cents(kLowE, 4);
    play(test, [...pluck(open), silence]);
    test.setOpenString();

    let view = play(test, [...pluck(cents(open * 2, 6)), silence]);
    expect(view.measurement?.fret).toBe(12);
    expect(view.measurement?.cents).toBeCloseTo(6, 0);
    expect(view.measurement?.quality).toBe("fair");

    view = play(test, [...pluck(open * Math.pow(2, 5 / 12)), silence]);
    expect(view.strings[0].frets.map((f) => f.fret)).toEqual([5, 12]);

    // Same fret again replaces the old reading.
    view = play(test, [...pluck(open * 2), silence]);
    expect(view.strings[0].frets.find((f) => f.fret === 12)?.quality).toBe("excellent");
  });

  it("flags a note that is not on the string being checked", () => {
    const test = new IntonationTest();
    play(test, [...pluck(110), silence]);
    test.setOpenString();
    const view = play(test, pluck(kLowE));
    expect(view.offString).toBe(true);
    expect(view.measurement).toBeNull();
  });

  it("keeps strings apart, lowest first, and setting one again starts its frets over", () => {
    const test = new IntonationTest();
    play(test, [...pluck(110), silence]);
    test.setOpenString();
    play(test, [...pluck(220), silence]);

    test.newString();
    expect(test.view().phase).toBe("open");
    play(test, [...pluck(kLowE), silence]);
    test.setOpenString();
    play(test, [...pluck(kLowE * 2), silence]);
    expect(test.view().strings.map((s) => s.name)).toEqual(["E2", "A2"]);

    test.newString();
    play(test, [...pluck(cents(110, -3)), silence]);
    test.setOpenString();
    const view = test.view();
    expect(view.strings.map((s) => s.name)).toEqual(["E2", "A2"]);
    expect(view.strings[1].frets).toEqual([]);
    expect(view.strings[0].frets).toHaveLength(1);

    test.clear();
    expect(test.view().strings).toEqual([]);
    expect(test.view().phase).toBe("open");
  });
});
