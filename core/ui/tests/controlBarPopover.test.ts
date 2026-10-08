/**
 * The compact controls sheet closes on a click outside the control bar, except in a popover the
 * bar opened but hung from the body (the input mode flyout): choosing an input mode on a phone
 * must not close the sheet it was chosen from.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it, vi } from "vitest";

vi.mock("../ts/compactMode.js", () => ({ isCompact: () => true }));
vi.mock("../ts/settings.js", () => ({
  initSettingsPanel: vi.fn(),
  refreshTone3000ApiKeyField: vi.fn(),
  activateEquipmentTab: vi.fn(),
  activateLibraryTab: vi.fn(),
  activateAdvancedSubTab: vi.fn(),
  setSettingsViewStateSuppressed: vi.fn(),
}));
vi.mock("../ts/tone3000.js", () => ({ ensureTone3000Session: vi.fn() }));
vi.mock("../ts/jam.js", () => ({ handleJamPanelActivated: vi.fn(), initializeJamPanel: vi.fn() }));
vi.mock("../ts/toneSharingPanel.js", () => ({ initializeToneSharingPanel: vi.fn() }));

// jsdom has no matchMedia; the bar asks it whether the window is narrow.
vi.stubGlobal("matchMedia", () => ({ matches: false, addEventListener: () => {}, removeEventListener: () => {} }));
window.IPlugSendMsg = () => {};

const INPUT_GROUP = readFileSync(join(__dirname, "..", "ui-components", "input-control-group.html"), "utf8");

const { initializeControlBarTabs } = await import("../ts/navigation.js");

/** A control bar at compact density with its Controls sheet open, holding `controls`. */
function openSheet(controls: string, outside: string): HTMLElement {
  document.body.innerHTML = `
    <section class="control-bar">
      <button type="button" data-control-bar-tab="preset"></button>
      <button type="button" data-control-bar-tab="controls"></button>
      <div class="control-bar-panel" id="control-bar-preset-panel"></div>
      <div class="control-bar-panel" id="control-bar-controls-panel">${controls}</div>
    </section>
    ${outside}
    <button type="button" id="elsewhere"></button>
  `;
  initializeControlBarTabs();
  document.querySelector<HTMLButtonElement>('[data-control-bar-tab="controls"]')?.click();
  const bar = document.querySelector<HTMLElement>(".control-bar");
  if (!bar) {
    throw new Error("no control bar");
  }
  return bar;
}

describe("the compact controls sheet", () => {
  it("stays open for a click in a popover the bar hung from the body, and closes for one elsewhere", () => {
    const bar = openSheet("", '<div data-control-bar-popover><button type="button" id="in-popover"></button></div>');
    expect(bar.dataset.compactControls).toBe("open");

    document.getElementById("in-popover")?.click();
    expect(bar.dataset.compactControls).toBe("open");

    document.getElementById("elsewhere")?.click();
    expect(bar.dataset.compactControls).toBe("closed");
  });

  it("stays open when an input mode is chosen from the flyout", async () => {
    const bar = openSheet(`<div class="control-bar-group">${INPUT_GROUP}</div>`, "");
    const inputMode = await import("../ts/inputMode.js");
    inputMode.initializeInputModeControls();

    document.getElementById("input-mode-trigger")?.click();
    const stereo = Array.from(document.querySelectorAll<HTMLButtonElement>(".input-mode-option"))
      .find((option) => option.dataset.mode === "stereo");
    stereo?.click();

    expect(document.getElementById("input-mode-flyout")?.hidden).toBe(true);
    expect(document.querySelector(".input-mode-trigger-label")?.textContent).toBe("Stereo");
    expect(bar.dataset.compactControls).toBe("open");
  });
});
