import { describe, expect, it } from "vitest";
import { FOOTER_PRIORITY, itemsThatFit } from "../ts/footerFit.js";

describe("itemsThatFit", () => {
  it("keeps every item when the room covers them all", () => {
    expect(itemsThatFit([44, 44, 88], 176)).toBe(3);
  });

  it("drops from the lowest priority up, never skipping one that does not fit", () => {
    // Room for the first two; the third would overflow, so it and all after it go.
    expect(itemsThatFit([44, 44, 88, 10], 100)).toBe(2);
  });

  it("lets a hidden item (no width) ride along with its neighbours", () => {
    expect(itemsThatFit([44, 0, 44], 88)).toBe(3);
  });

  it("keeps nothing when there is no room", () => {
    expect(itemsThatFit([44], 0)).toBe(0);
  });
});

describe("FOOTER_PRIORITY", () => {
  it("puts Record and Loop before the history, and the tempo and play last", () => {
    const order = FOOTER_PRIORITY.map((selectors) => selectors[0]);
    expect(order.indexOf("#footer-riff-record-btn")).toBeLessThan(order.indexOf("#footer-chain-undo"));
    expect(order.indexOf("#footer-looper-btn")).toBeLessThan(order.indexOf("#footer-chain-undo"));
    expect(order.slice(-2)).toEqual(["#footer-bpm-btn", "#footer-metronome-toggle"]);
  });
});
