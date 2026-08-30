import { describe, expect, it } from "vitest";
import { clampRatioRange, otherHandle, type RangeHandle } from "../ts/waveform/range.js";

const MIN = 0.004; // ~0.25s over a 60s track, the Practice Tool's real floor

describe("clampRatioRange", () => {
  it("passes an already-valid range through untouched", () => {
    const r = clampRatioRange(0.3, 0.5, MIN);
    expect(r).toEqual({ startRatio: 0.3, endRatio: 0.5, swapped: false });
  });

  it("clamps both bounds into 0..1", () => {
    const r = clampRatioRange(-0.4, 1.8, MIN);
    expect(r.startRatio).toBe(0);
    expect(r.endRatio).toBe(1);
  });

  it("reorders inverted bounds and reports the swap", () => {
    const r = clampRatioRange(0.6, 0.2, MIN);
    expect(r.startRatio).toBeCloseTo(0.2);
    expect(r.endRatio).toBeCloseTo(0.6);
    expect(r.swapped).toBe(true);
  });

  it("widens a too-short range forward rather than reporting a swap", () => {
    const r = clampRatioRange(0.5, 0.5005, MIN);
    expect(r.startRatio).toBeCloseTo(0.5);
    expect(r.endRatio).toBeCloseTo(0.5 + MIN);
    expect(r.swapped).toBe(false);
  });

  it("widens backwards from the end when there is no room on the right", () => {
    const r = clampRatioRange(1, 1, MIN);
    expect(r.startRatio).toBeCloseTo(1 - MIN);
    expect(r.endRatio).toBe(1);
  });
});

describe("minimum span pushes the held bound, not the anchor", () => {
  // Squeezing below the floor has to move something. Moving the bound the user
  // is not holding reads as the whole selection sliding away from them — and it
  // silently drags the anchor along across a crossing, which is how a drag past
  // the far handle ended up 0.2s adrift in the live app.
  it("stops the start handle rather than shoving the end away", () => {
    const r = clampRatioRange(0.598, 0.6, MIN, "start");
    expect(r.endRatio).toBeCloseTo(0.6); // untouched
    expect(r.startRatio).toBeCloseTo(0.6 - MIN);
  });

  it("stops the end handle rather than shoving the start away", () => {
    const r = clampRatioRange(0.3, 0.302, MIN, "end");
    expect(r.startRatio).toBeCloseTo(0.3); // untouched
    expect(r.endRatio).toBeCloseTo(0.3 + MIN);
  });

  it("keeps the anchor put when a crossing lands inside the floor", () => {
    // Dragging END left, one step past START at 0.3.
    const r = clampRatioRange(0.3, 0.299, MIN, "end");
    expect(r.swapped).toBe(true);
    expect(r.endRatio).toBeCloseTo(0.3); // the anchor survived
    expect(r.startRatio).toBeCloseTo(0.3 - MIN);
  });

  it("still falls back to pushing the end when no handle is named", () => {
    const r = clampRatioRange(0.3, 0.302, MIN);
    expect(r.startRatio).toBeCloseTo(0.3);
    expect(r.endRatio).toBeCloseTo(0.3 + MIN);
  });
});

describe("handle crossing", () => {
  // Reproduces the gesture that was broken in both editors: grab the END
  // handle and drag it left, past the stationary START handle. The region must
  // stay anchored on START and grow leftwards; the caller must retarget to the
  // handle it is now actually holding.
  //
  // Before the fix the Practice Tool re-crossed on every event, so the region
  // never anchored — it shrank to one pointer step and slid along. The Riff
  // editor never swapped at all, so the trailing marker was dragged onto the
  // leading one and the user's END position was destroyed.
  function dragEndPast(anchor: number, path: readonly number[], minSpan: number) {
    let start = anchor;
    let end = 0.5;
    let held: RangeHandle = "end";
    for (const pointer of path) {
      const next = held === "start"
        ? clampRatioRange(pointer, end, minSpan)
        : clampRatioRange(start, pointer, minSpan);
      start = next.startRatio;
      end = next.endRatio;
      if (next.swapped) {
        held = otherHandle(held);
      }
    }
    return { start, end, held };
  }

  it("anchors on the stationary handle and grows away from it", () => {
    const { start, end, held } = dragEndPast(0.3, [0.28, 0.26, 0.2, 0.1], MIN);
    expect(start).toBeCloseTo(0.1);
    expect(end).toBeCloseTo(0.3); // the anchor survived the crossing
    expect(held).toBe("start"); // retargeted to the bound under the pointer
  });

  it("keeps the region growing monotonically once crossed", () => {
    const spans = [0.28, 0.26, 0.24, 0.2, 0.15, 0.1].map((_, i, all) => {
      const { start, end } = dragEndPast(0.3, all.slice(0, i + 1), MIN);
      return end - start;
    });
    for (let i = 1; i < spans.length; i++) {
      expect(spans[i]).toBeGreaterThan(spans[i - 1]);
    }
  });

  it("crosses cleanly with the Riff editor's much smaller minimum span", () => {
    const { start, end, held } = dragEndPast(0.3, [0.28, 0.1], 0.001);
    expect(start).toBeCloseTo(0.1);
    expect(end).toBeCloseTo(0.3);
    expect(held).toBe("start");
  });

  it("survives being dragged back across a second time", () => {
    const { start, end, held } = dragEndPast(0.3, [0.2, 0.1, 0.4, 0.6], MIN);
    expect(start).toBeCloseTo(0.3);
    expect(end).toBeCloseTo(0.6);
    expect(held).toBe("end");
  });
});

describe("otherHandle", () => {
  it("is its own inverse", () => {
    expect(otherHandle("start")).toBe("end");
    expect(otherHandle(otherHandle("start"))).toBe("start");
  });
});
