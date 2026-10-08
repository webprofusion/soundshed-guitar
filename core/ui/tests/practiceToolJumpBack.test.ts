import { beforeEach, describe, expect, it } from "vitest";
import {
  clearJumpBackPoint,
  getJumpBackMarkerSec,
  getJumpBackTargetSec,
  setJumpBackPoint,
} from "../ts/practiceTool/jumpBack.js";

const loop = { startSec: 30, endSec: 38 };

beforeEach(() => {
  clearJumpBackPoint();
});

describe("Jump Back target", () => {
  it("goes to the loop's start when nothing was clicked", () => {
    expect(getJumpBackTargetSec(loop)).toBe(30);
    expect(getJumpBackMarkerSec(loop)).toBeNull();
  });

  it("goes to the top of the track with no loop and no click", () => {
    expect(getJumpBackTargetSec(null)).toBe(0);
    expect(getJumpBackMarkerSec(null)).toBeNull();
  });

  it("goes to a point clicked inside the loop, and marks it", () => {
    setJumpBackPoint(34.5);
    expect(getJumpBackTargetSec(loop)).toBe(34.5);
    expect(getJumpBackMarkerSec(loop)).toBe(34.5);
  });

  it("goes to the clicked point with no loop active", () => {
    setJumpBackPoint(72);
    expect(getJumpBackTargetSec(null)).toBe(72);
    expect(getJumpBackMarkerSec(null)).toBe(72);
  });

  it("falls back to the loop's start once the loop no longer contains the point", () => {
    setJumpBackPoint(34.5);
    expect(getJumpBackTargetSec({ startSec: 35, endSec: 40 })).toBe(35);
    expect(getJumpBackTargetSec({ startSec: 30, endSec: 34 })).toBe(30);
    expect(getJumpBackMarkerSec({ startSec: 35, endSec: 40 })).toBeNull();
  });

  it("does not mark a click that landed on the loop's start", () => {
    setJumpBackPoint(30);
    expect(getJumpBackTargetSec(loop)).toBe(30);
    expect(getJumpBackMarkerSec(loop)).toBeNull();
  });

  it("forgets the point when cleared", () => {
    setJumpBackPoint(34.5);
    clearJumpBackPoint();
    expect(getJumpBackTargetSec(loop)).toBe(30);
  });
});
