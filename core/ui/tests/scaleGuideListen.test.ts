import { describe, expect, it } from "vitest";
import { matchChord, NoteFollower, readingMidi, StrumGate } from "../ts/scaleGuide/listen";

const C_MAJOR = [
  { pitchClass: 0, interval: "R" },
  { pitchClass: 4, interval: "3" },
  { pitchClass: 7, interval: "5" },
];

/** A chroma vector with `weights` on the given pitch classes and a little elsewhere. */
function chroma(weights: Record<number, number>, noise = 0.005): number[] {
  return Array.from({ length: 12 }, (_, pc) => weights[pc] ?? noise);
}

describe("scale guide listen", () => {
  it("reads the tuner's note as a MIDI note", () => {
    expect(readingMidi("A", 4)).toBe(69);
    expect(readingMidi("C#/Db", 3)).toBe(49);
    expect(readingMidi("E", 2)).toBe(40);
    expect(readingMidi("", 3)).toBeNull();
    expect(readingMidi("A", undefined)).toBeNull();
  });

  it("matches a chord whose tones hold the chroma and are all heard", () => {
    const clean = matchChord(chroma({ 0: 0.4, 4: 0.3, 7: 0.25 }), 0, C_MAJOR, "normal");
    expect(clean.ok).toBe(true);
    expect(clean.heard).toEqual([true, true, true]);
    expect(clean.share).toBeGreaterThan(0.9);
  });

  it("names the missing tone, and lets a plain fifth go", () => {
    const noThird = matchChord(chroma({ 0: 0.6, 7: 0.35 }), 0, C_MAJOR, "normal");
    expect(noThird.ok).toBe(false);
    expect(noThird.missing).toEqual([4]);

    const noFifth = matchChord(chroma({ 0: 0.55, 4: 0.4 }), 0, C_MAJOR, "normal");
    expect(noFifth.missing).toEqual([]);
    expect(noFifth.ok).toBe(true);
  });

  it("refuses a chord another one on the same root fits better", () => {
    // C minor's E-flat played for C major: the chord's share is low and C minor fits better.
    const minor = matchChord(chroma({ 0: 0.4, 3: 0.3, 7: 0.25, 4: 0.02 }), 0, C_MAJOR, "easy");
    expect(minor.ok).toBe(false);
  });

  it("asks more of a chord the stricter it is", () => {
    const loose = chroma({ 0: 0.3, 4: 0.18, 7: 0.15, 2: 0.12, 9: 0.12 });
    expect(matchChord(loose, 0, C_MAJOR, "easy").ok).toBe(true);
    expect(matchChord(loose, 0, C_MAJOR, "strict").ok).toBe(false);
  });

  it("counts a strum once it holds clean, and the next only after a fresh strum", () => {
    const gate = new StrumGate();
    const hits: boolean[] = [];

    for (let i = 0; i < 12; i += 1) {
      hits.push(gate.push(i === 0 ? -20 : -22 - i * 0.2, true));
    }

    expect(hits.filter(Boolean)).toHaveLength(1);
    expect(hits.indexOf(true)).toBe(StrumGate.HOLD - 1);

    // Quiet, then a new strum: counted again.
    gate.push(-70, false);
    gate.push(-70, false);
    const again = [-20, -21, -21, -21, -21].map((level) => gate.push(level, true));
    expect(again.filter(Boolean)).toHaveLength(1);
  });

  it("does not count a strum again while it rings over the quiet before it", () => {
    const gate = new StrumGate();
    gate.push(-70, false);
    gate.push(-70, false);
    const ringing = Array.from({ length: 20 }, (_, i) => gate.push(-20 - i * 0.3, true));
    expect(ringing.filter(Boolean)).toHaveLength(1);
  });

  it("follows a note once it reads twice, then lets it go into a trail", () => {
    const follower = new NoteFollower();
    expect(follower.push(57)).toBe(false);
    expect(follower.push(57)).toBe(true);
    expect(follower.current).toBe(57);
    expect(follower.push(69)).toBe(false); // one blip is not a note
    expect(follower.push(60)).toBe(false);
    expect(follower.push(60)).toBe(true);
    expect(follower.current).toBe(60);
    expect(follower.trail).toEqual([57]);

    for (let i = 0; i < NoteFollower.RELEASE_READINGS - 1; i += 1) {
      expect(follower.push(null)).toBe(false);
    }

    expect(follower.push(null)).toBe(true);
    expect(follower.current).toBeNull();
    expect(follower.trail).toEqual([60, 57]);
  });
});
