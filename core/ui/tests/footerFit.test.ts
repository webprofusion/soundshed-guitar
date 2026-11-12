import { describe, expect, it } from "vitest";
import { FOOTER_DEMO_ON_BAR, FOOTER_PRIORITY, FOOTER_SHEET_ONLY, itemsThatFit } from "../ts/footerFit.js";

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
  it("keeps the full bar's order: Record and Loop, then the tempo, the metronome and its play", () => {
    const order = FOOTER_PRIORITY.map((selectors) => selectors[0]);
    expect(order).toEqual([
      "#footer-riff-record-btn",
      "#footer-looper-btn",
      "#footer-bpm-btn",
      "#footer-metronome-btn",
      "#footer-metronome-toggle",
    ]);
  });

  it("never fits the chain's history or A/B onto the compact bar", () => {
    const fitted = FOOTER_PRIORITY.flat();
    for (const selector of FOOTER_SHEET_ONLY.flat()) {
      expect(fitted).not.toContain(selector);
    }
    expect(FOOTER_SHEET_ONLY.flat()).toEqual(["#footer-chain-undo", "#footer-chain-redo", ".footer-ab-switch", "#footer-chain-copy"]);
  });

  it("puts the demo clip's chooser before its play button", () => {
    expect(FOOTER_DEMO_ON_BAR).toEqual(["#footer-demo-audio-select", "#footer-play-demo-audio"]);
  });
});
