import { afterEach, describe, expect, it, vi } from "vitest";

// A wrap choice is saved with the UI settings, which go to the engine; these tests
// are about which choice applies, not about a message to an engine that is not there.
vi.mock("../ts/bridge.js", () => ({ postMessage: vi.fn(), setAppSetting: vi.fn() }));

function setViewport(width: number, height: number): void {
  Object.defineProperty(window, "innerWidth", { configurable: true, value: width });
  Object.defineProperty(window, "innerHeight", { configurable: true, value: height });
}

// Density and the UI settings are module state, so each test starts from fresh copies.
async function load(width: number, height: number) {
  setViewport(width, height);
  vi.resetModules();
  const compactMode = await import("../ts/compactMode.js");
  const windowSettings = await import("../ts/windowSettings.js");
  const chainRow = await import("../ts/signalPath/chainRow.js");
  compactMode.initCompactMode();
  return { ...compactMode, ...windowSettings, ...chainRow };
}

describe("signal chain wrap", () => {
  afterEach(() => {
    const root = document.documentElement;
    delete root.dataset.density;
    delete root.dataset.densityPref;
    delete root.dataset.compactLayout;
  });

  it("wraps on a small display and not on a large one, by default", async () => {
    expect((await load(844, 390)).isSignalChainWrapEnabled()).toBe(true);
    expect((await load(1280, 800)).isSignalChainWrapEnabled()).toBe(false);
  });

  it("follows the window across the compact threshold", async () => {
    const m = await load(1280, 800);
    expect(m.isSignalChainWrapEnabled()).toBe(false);
    setViewport(900, 700);
    m.initCompactMode();
    expect(m.isCompact()).toBe(true);
    expect(m.isSignalChainWrapEnabled()).toBe(true);
  });

  it("keeps a choice made on a small display to that density", async () => {
    const m = await load(844, 390);
    m.toggleSignalChainWrap();
    expect(m.isSignalChainWrapEnabled()).toBe(false);
    expect(m.getCurrentUiSettings()).toMatchObject({ signalPathWrapCompact: false });
    expect(m.getCurrentUiSettings().signalPathWrap).toBeUndefined();

    m.applyDensityPreference("full");
    expect(m.isSignalChainWrapEnabled()).toBe(false);
    m.toggleSignalChainWrap();
    expect(m.isSignalChainWrapEnabled()).toBe(true);

    m.applyDensityPreference("compact");
    expect(m.isSignalChainWrapEnabled()).toBe(false);
  });

  it("reads a stored choice for each density", async () => {
    const m = await load(844, 390);
    m.applyUiSettings({ zoom: 1, signalPathWrap: true, signalPathWrapCompact: false });
    expect(m.isSignalChainWrapEnabled()).toBe(false);
    m.applyDensityPreference("full");
    expect(m.isSignalChainWrapEnabled()).toBe(true);
  });
});
