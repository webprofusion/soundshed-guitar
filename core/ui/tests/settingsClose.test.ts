/**
 * Settings has a labelled Close button, and Escape and the gear close it too. Closing returns to
 * the panel Settings was opened from, not always to Play. Escape leaves an open dialog, or a field
 * being typed in, to deal with the key first.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/compactMode.js", () => ({ isCompact: () => false }));
vi.mock("../ts/settings.js", () => ({
  initSettingsPanel: vi.fn(),
  refreshTone3000ApiKeyField: vi.fn(),
  activateEquipmentTab: vi.fn(),
  activateLibraryTab: vi.fn(),
  activateAdvancedSubTab: vi.fn(),
  setSettingsViewStateSuppressed: vi.fn(),
}));
vi.mock("../ts/tone3000.js", () => ({ ensureTone3000Session: vi.fn(async () => {}) }));
vi.mock("../ts/jam.js", () => ({ handleJamPanelActivated: vi.fn(), initializeJamPanel: vi.fn() }));
vi.mock("../ts/toneSharingPanel.js", () => ({ initializeToneSharingPanel: vi.fn() }));

vi.stubGlobal("matchMedia", () => ({ matches: false, addEventListener: () => {}, removeEventListener: () => {} }));
window.IPlugSendMsg = () => {};

const SETTINGS_PANEL = readFileSync(join(__dirname, "..", "ui-components", "panels", "settings-panel.html"), "utf8");
const CLOSE_BUTTON = SETTINGS_PANEL.match(/<button[^>]*id="settings-close-btn"[\s\S]*?<\/button>/)?.[0] ?? "";

const { closeSettings, initializeIconBarTabs, initSettingsClose, switchMainPanel } = await import("../ts/navigation.js");

document.body.innerHTML = `
  <header class="icon-bar">
    <button class="icon-btn" data-panel="visualizer"></button>
    <button class="icon-btn" data-panel="sharing"></button>
    <button class="icon-btn nav-settings-btn" data-panel="settings"></button>
  </header>
  <div class="main-content">
    <div class="tab-panel" id="panel-visualizer"></div>
    <div class="tab-panel" id="panel-sharing"></div>
    <div class="tab-panel" id="panel-settings">${CLOSE_BUTTON}<input type="text" id="field" /></div>
  </div>
  <div class="modal" id="some-modal" style="display: none;"></div>`;
initializeIconBarTabs();
initSettingsClose();

const panel = () => document.documentElement.dataset.mainPanel;
const escape = (target: Element = document.body) =>
  target.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true }));
const gear = () => document.querySelector<HTMLButtonElement>(".nav-settings-btn")?.click();

beforeEach(() => {
  switchMainPanel("visualizer");
  (document.getElementById("some-modal") as HTMLElement).style.display = "none";
});

describe("closing Settings", () => {
  it("ships a labelled Close button", () => {
    expect(CLOSE_BUTTON).toContain('aria-label="Close Settings"');
    expect(CLOSE_BUTTON).toContain("<span>Close</span>");
  });

  it("returns to the panel Settings was opened from", () => {
    switchMainPanel("sharing");
    switchMainPanel("settings");
    expect(panel()).toBe("settings");
    document.getElementById("settings-close-btn")?.click();
    expect(panel()).toBe("sharing");
  });

  it("goes to Play when Settings was the first thing shown, and does nothing elsewhere", () => {
    closeSettings();
    expect(panel()).toBe("visualizer");
    switchMainPanel("sharing");
    closeSettings();
    expect(panel()).toBe("sharing");
  });

  it("is a toggle on the gear", () => {
    switchMainPanel("sharing");
    gear();
    expect(panel()).toBe("settings");
    gear();
    expect(panel()).toBe("sharing");
  });

  it("closes on Escape", () => {
    switchMainPanel("settings");
    escape();
    expect(panel()).toBe("visualizer");
  });

  it("leaves Escape to an open dialog", () => {
    switchMainPanel("settings");
    (document.getElementById("some-modal") as HTMLElement).style.display = "flex";
    escape();
    expect(panel()).toBe("settings");
  });

  it("leaves Escape to a text field being edited", () => {
    switchMainPanel("settings");
    escape(document.getElementById("field") as HTMLElement);
    expect(panel()).toBe("settings");
  });

  it("ignores Escape when Settings is not showing", () => {
    switchMainPanel("sharing");
    escape();
    expect(panel()).toBe("sharing");
  });
});
