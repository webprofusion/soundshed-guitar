import { afterEach, describe, expect, it, vi } from "vitest";

// Persisting a preference goes through the bridge; these tests are about the
// layout decision, not about a message to an engine that is not there.
vi.mock("../ts/bridge.js", () => ({ setAppSetting: vi.fn() }));

function setViewport(width: number, height: number): void {
  Object.defineProperty(window, "innerWidth", { configurable: true, value: width });
  Object.defineProperty(window, "innerHeight", { configurable: true, value: height });
}

// The module keeps its resolved density in module state, so each test gets a
// fresh copy rather than inheriting the last test's viewport.
async function loadCompactMode() {
  vi.resetModules();
  return import("../ts/compactMode.js");
}

function captureNextDensityChange(): unknown[] {
  const seen: unknown[] = [];
  window.addEventListener("densityChanged", (event) => seen.push((event as CustomEvent).detail), { once: true });
  return seen;
}

describe("compactLayoutForViewport", () => {
  it("puts a landscape viewport on the rail", async () => {
    const { compactLayoutForViewport } = await loadCompactMode();
    expect(compactLayoutForViewport(640, 400)).toBe("rail");
    expect(compactLayoutForViewport(844, 390)).toBe("rail");
  });

  it("keeps the stacked bars for a portrait or square viewport", async () => {
    const { compactLayoutForViewport } = await loadCompactMode();
    expect(compactLayoutForViewport(390, 844)).toBe("stack");
    expect(compactLayoutForViewport(600, 600)).toBe("stack");
  });
});

describe("compactSplitForViewport", () => {
  it("shows the chain and the effect together in a tall, wide enough stacked window", async () => {
    const { compactSplitForViewport } = await loadCompactMode();
    expect(compactSplitForViewport(960, 1040)).toBe(true);
    expect(compactSplitForViewport(768, 1024)).toBe(true);
    expect(compactSplitForViewport(600, 800)).toBe(true);
  });

  it("keeps the tabs on a phone held upright, a short window and the rail", async () => {
    const { compactSplitForViewport } = await loadCompactMode();
    expect(compactSplitForViewport(390, 844)).toBe(false);
    expect(compactSplitForViewport(700, 780)).toBe(false);
    expect(compactSplitForViewport(900, 820)).toBe(false);
  });
});

describe("initCompactMode", () => {
  const root = document.documentElement;

  afterEach(() => {
    delete root.dataset.density;
    delete root.dataset.densityPref;
    delete root.dataset.compactLayout;
    delete root.dataset.compactSplit;
  });

  it("stamps the split on a window snapped to half a monitor, and drops it when it gets short", async () => {
    setViewport(960, 1040);
    const { initCompactMode, isCompactSplit, isCompactStaged } = await loadCompactMode();
    initCompactMode();
    expect(root.dataset.compactLayout).toBe("stack");
    expect(root.dataset.compactSplit).toBe("");
    expect(isCompactSplit()).toBe(true);
    expect(isCompactStaged()).toBe(false);

    const seen = captureNextDensityChange();
    setViewport(960, 760);
    initCompactMode();
    expect(root.dataset.compactSplit).toBeUndefined();
    expect(isCompactStaged()).toBe(true);
    expect(seen).toHaveLength(1);
  });

  it("never splits at full density", async () => {
    setViewport(1280, 1040);
    const { initCompactMode, isCompactSplit, isCompactStaged } = await loadCompactMode();
    initCompactMode();
    expect(root.dataset.compactSplit).toBeUndefined();
    expect(isCompactSplit()).toBe(false);
    expect(isCompactStaged()).toBe(false);
  });

  it("stamps the rail on a phone held landscape", async () => {
    setViewport(844, 390);
    const { initCompactMode, getCompactLayout } = await loadCompactMode();
    initCompactMode();
    expect(root.dataset.density).toBe("compact");
    expect(root.dataset.compactLayout).toBe("rail");
    expect(getCompactLayout()).toBe("rail");
  });

  it("stacks the bars in a narrow portrait window", async () => {
    setViewport(600, 900);
    const { initCompactMode } = await loadCompactMode();
    initCompactMode();
    expect(root.dataset.density).toBe("compact");
    expect(root.dataset.compactLayout).toBe("stack");
  });

  it("carries no layout at full density", async () => {
    setViewport(1280, 800);
    const { initCompactMode, getCompactLayout } = await loadCompactMode();
    initCompactMode();
    expect(root.dataset.density).toBe("full");
    expect(root.dataset.compactLayout).toBeUndefined();
    expect(getCompactLayout()).toBeNull();
  });

  it("drops the layout when Full is pinned, and says so", async () => {
    setViewport(844, 390);
    const { initCompactMode, applyDensityPreference } = await loadCompactMode();
    initCompactMode();
    const seen = captureNextDensityChange();
    applyDensityPreference("full");
    expect(root.dataset.compactLayout).toBeUndefined();
    expect(seen).toEqual([{ density: "full", layout: null, preference: "full" }]);
  });

  it("announces a layout change even when the density stays compact", async () => {
    setViewport(900, 700);
    const { initCompactMode } = await loadCompactMode();
    initCompactMode();
    expect(root.dataset.compactLayout).toBe("rail");

    const seen = captureNextDensityChange();
    setViewport(700, 900);
    initCompactMode();
    expect(root.dataset.compactLayout).toBe("stack");
    expect(seen).toEqual([{ density: "compact", layout: "stack", preference: "auto" }]);
  });
});
