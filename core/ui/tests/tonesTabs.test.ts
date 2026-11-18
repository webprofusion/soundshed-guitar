import { beforeEach, describe, expect, it, vi } from "vitest";

const mocks = vi.hoisted(() => ({
  enabled: new Set<string>(),
  initializeToneSharingPanel: vi.fn(),
  initTone3000Browser: vi.fn(),
  refreshTone3000BrowserOnShow: vi.fn(),
  initLocalPresetsBrowser: vi.fn(),
}));

vi.mock("../ts/featureFlags.js", () => ({
  FEATURE_FLAGS_CHANGED_EVENT: "featureFlagsChanged",
  Features: { ToneSharing: "toneSharing", Tone3000: "tone3000" },
  isFeatureEnabled: (id: string) => mocks.enabled.has(id),
}));
vi.mock("../ts/toneSharingPanel.js", () => ({ initializeToneSharingPanel: mocks.initializeToneSharingPanel }));
vi.mock("../ts/tone3000Browser.js", () => ({ initTone3000Browser: mocks.initTone3000Browser, refreshTone3000BrowserOnShow: mocks.refreshTone3000BrowserOnShow }));
vi.mock("../ts/tonesPanel/localPresets.js", () => ({ initLocalPresetsBrowser: mocks.initLocalPresetsBrowser }));

import { activateTonesTab, initializeTonesPanel, resolveTonesTabId } from "../ts/tonesPanel/tabs.js";
import { openTonesPanel, requestTonesTab, setTonesTabChangeListener } from "../ts/tonesPanelLink.js";

function mountPanel(active: boolean): void {
  document.body.innerHTML = `
    <div class="tab-panel${active ? " active" : ""}" id="panel-sharing">
      <button class="tones-tab-btn" data-tones-tab="community"></button>
      <button class="tones-tab-btn" data-tones-tab="local"></button>
      <button class="tones-tab-btn" data-tones-tab="tone3000"></button>
      <div class="tones-tab-panel" id="tones-tab-community"></div>
      <div class="tones-tab-panel" id="tones-tab-local"><button id="to-community" data-tones-open-tab="community"></button></div>
      <div class="tones-tab-panel" id="tones-tab-tone3000"></div>
    </div>`;
}

const activePanelId = (): string | undefined => document.querySelector(".tones-tab-panel.active")?.id;
const tabButton = (id: string): HTMLElement => document.querySelector<HTMLElement>(`[data-tones-tab="${id}"]`)!;

beforeEach(() => {
  mocks.enabled.clear();
  mocks.enabled.add("toneSharing");
  mocks.enabled.add("tone3000");
  vi.clearAllMocks();
  setTonesTabChangeListener(() => {});
});

describe("resolveTonesTabId", () => {
  it("keeps a tab that exists and is on", () => {
    expect(resolveTonesTabId("tone3000")).toBe("tone3000");
    expect(resolveTonesTabId("local")).toBe("local");
  });

  it("falls back to the first enabled tab for an unknown or disabled one", () => {
    expect(resolveTonesTabId("nonsense")).toBe("community");
    expect(resolveTonesTabId(undefined)).toBe("community");
    mocks.enabled.delete("tone3000");
    expect(resolveTonesTabId("tone3000")).toBe("community");
  });

  it("always has Your Presets, with every feature off", () => {
    mocks.enabled.clear();
    expect(resolveTonesTabId("community")).toBe("local");
    expect(resolveTonesTabId("tone3000")).toBe("local");
  });
});

describe("activateTonesTab", () => {
  it("selects the tab without starting it while Tones is closed", () => {
    mountPanel(false);
    activateTonesTab("tone3000");
    expect(activePanelId()).toBe("tones-tab-tone3000");
    expect(tabButton("tone3000").getAttribute("aria-selected")).toBe("true");
    expect(tabButton("community").getAttribute("aria-selected")).toBe("false");
    expect(mocks.initTone3000Browser).not.toHaveBeenCalled();
    expect(mocks.initializeToneSharingPanel).not.toHaveBeenCalled();
  });

  it("starts the tab it restored, and only that one, when Tones opens", () => {
    mountPanel(false);
    requestTonesTab("local");
    document.getElementById("panel-sharing")!.classList.add("active");
    openTonesPanel();
    expect(activePanelId()).toBe("tones-tab-local");
    expect(mocks.initLocalPresetsBrowser).toHaveBeenCalledTimes(1);
    expect(mocks.initializeToneSharingPanel).not.toHaveBeenCalled();
    expect(mocks.initTone3000Browser).not.toHaveBeenCalled();
  });

  it("starts Tone 3000 once, and on later visits only checks whether its results are stale", () => {
    mountPanel(true);
    activateTonesTab("tone3000");
    activateTonesTab("local");
    activateTonesTab("tone3000");
    expect(mocks.initTone3000Browser).toHaveBeenCalledTimes(1);
    expect(mocks.refreshTone3000BrowserOnShow).toHaveBeenCalledTimes(1);
  });

  it("opens on a named tab and reports it", () => {
    mountPanel(true);
    const changed = vi.fn();
    setTonesTabChangeListener(changed);
    openTonesPanel("community");
    expect(activePanelId()).toBe("tones-tab-community");
    expect(mocks.initializeToneSharingPanel).toHaveBeenCalled();
    expect(changed).toHaveBeenLastCalledWith("community");
  });

  it("hides the tabs whose feature is off and moves off a disabled one", () => {
    mountPanel(true);
    initializeTonesPanel("community");
    mocks.enabled.delete("toneSharing");
    initializeTonesPanel();
    expect(tabButton("community").hasAttribute("hidden")).toBe(true);
    expect(tabButton("tone3000").hasAttribute("hidden")).toBe(false);
    expect(activePanelId()).toBe("tones-tab-local");
  });
});
