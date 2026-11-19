/**
 * The footer's Theme button opens a flyout holding the zoom picker and a tile per theme. Choosing a
 * tile applies the theme and leaves the flyout open, marking the tile in place rather than
 * redrawing the list the click came from.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeAll, describe, expect, it } from "vitest";

window.IPlugSendMsg = () => {};

const FOOTER = readFileSync(join(__dirname, "..", "ui-components", "footer-bar.html"), "utf8");
document.body.innerHTML = FOOTER;

const { THEME_OPTIONS, themeSwitcher } = await import("../ts/theme-switcher.js");
const { initThemeFlyout, themeThumbnailUrl } = await import("../ts/themeFlyout.js");

const trigger = () => document.getElementById("footer-theme-btn") as HTMLButtonElement;
const flyout = () => document.getElementById("theme-flyout") as HTMLDivElement;
const tiles = () => Array.from(flyout().querySelectorAll<HTMLButtonElement>(".theme-flyout-tile"));

describe("the theme flyout", () => {
  beforeAll(() => {
    themeSwitcher.setTheme("dark", false);
    initThemeFlyout();
  });

  it("is hung from the body and holds the zoom picker", () => {
    expect(flyout().parentElement).toBe(document.body);
    expect(flyout().hidden).toBe(true);
    expect(flyout().querySelector("#zoom-level-select")).not.toBeNull();
    expect(document.querySelector(".footer-bar #zoom-level-select")).toBeNull();
  });

  it("opens from the button with a tile and a picture for every theme, the current one marked", () => {
    trigger().click();
    expect(flyout().hidden).toBe(false);
    expect(trigger().getAttribute("aria-expanded")).toBe("true");

    expect(tiles().map((tile) => tile.dataset.theme)).toEqual(THEME_OPTIONS.map((option) => option.value));
    expect(tiles().map((tile) => tile.textContent?.trim())).toEqual(THEME_OPTIONS.map((option) => option.label));
    for (const tile of tiles()) {
      expect(tile.querySelector("img")?.getAttribute("src")).toBe(themeThumbnailUrl(tile.dataset.theme as never));
    }

    const checked = tiles().filter((tile) => tile.getAttribute("aria-checked") === "true");
    expect(checked.map((tile) => tile.dataset.theme)).toEqual(["dark"]);
  });

  it("applies a chosen theme, stays open, and marks the same tile without rebuilding the list", () => {
    const before = tiles();
    const stage = before.find((tile) => tile.dataset.theme === "stage");
    stage?.click();

    expect(themeSwitcher.getCurrentTheme()).toBe("stage");
    expect(document.body.classList.contains("theme-stage")).toBe(true);
    expect(flyout().hidden).toBe(false);
    expect(stage?.isConnected).toBe(true);
    expect(tiles()).toEqual(before);
    expect(stage?.getAttribute("aria-checked")).toBe("true");
    expect(stage?.tabIndex).toBe(0);
    expect(before.find((tile) => tile.dataset.theme === "dark")?.getAttribute("aria-checked")).toBe("false");
  });

  it("follows a theme changed elsewhere", () => {
    themeSwitcher.setTheme("studio", false);
    const checked = tiles().filter((tile) => tile.getAttribute("aria-checked") === "true");
    expect(checked.map((tile) => tile.dataset.theme)).toEqual(["studio"]);
  });

  it("closes on Escape and on a press outside it", () => {
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    expect(flyout().hidden).toBe(true);
    expect(trigger().getAttribute("aria-expanded")).toBe("false");

    trigger().click();
    expect(flyout().hidden).toBe(false);
    flyout().querySelector("select")?.dispatchEvent(new Event("pointerdown", { bubbles: true }));
    expect(flyout().hidden).toBe(false);
    document.querySelector(".footer-bar")?.dispatchEvent(new Event("pointerdown", { bubbles: true }));
    expect(flyout().hidden).toBe(true);
  });

  it("marks itself as the footer's and the control bar's, so neither sheet closes for a click in it", () => {
    expect(flyout().hasAttribute("data-footer-popover")).toBe(true);
    expect(flyout().hasAttribute("data-control-bar-popover")).toBe(true);
  });
});
