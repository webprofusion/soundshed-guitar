import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/presets/saveModal.js", () => ({ createDefaultPreset: vi.fn() }));
vi.mock("../ts/pointerDrag.js", () => ({ getUiZoom: () => 1.5 }));

import { createDefaultPreset } from "../ts/presets/saveModal.js";
import { initializeNewPresetMenu } from "../ts/presets/newPresetMenu.js";

initializeNewPresetMenu();

beforeEach(() => {
  window.dispatchEvent(new Event("resize"));
  document.body.innerHTML = `
    <button id="preset-new-btn" aria-expanded="false">New</button>
    <button id="outside">Outside</button>
  `;
  vi.clearAllMocks();
});

const trigger = () => document.getElementById("preset-new-btn") as HTMLButtonElement;
const options = () => Array.from(document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]'));
const key = (element: Element, value: string) =>
  element.dispatchEvent(new KeyboardEvent("keydown", { key: value, bubbles: true }));

describe("new preset flyout", () => {
  it("opens without creating a preset, and sends only the selected configuration", () => {
    trigger().click();
    expect(trigger().getAttribute("aria-expanded")).toBe("true");
    expect(options().map((option) => option.textContent)).toEqual(["Empty Preset", "Basic Signal Chain"]);
    expect(createDefaultPreset).not.toHaveBeenCalled();
    options()[0].click();
    expect(createDefaultPreset).toHaveBeenCalledTimes(1);
    expect(createDefaultPreset).toHaveBeenCalledWith("empty");
    expect(options()).toHaveLength(0);
    expect(trigger().getAttribute("aria-expanded")).toBe("false");
    expect(document.activeElement).toBe(trigger());

    trigger().click();
    options()[1].click();
    expect(createDefaultPreset).toHaveBeenLastCalledWith("basic");
  });

  it("supports arrow keys, Home, End and Escape with focus restoration", () => {
    key(trigger(), "ArrowUp");
    expect(document.activeElement).toBe(options()[1]);
    key(options()[1], "ArrowDown");
    expect(document.activeElement).toBe(options()[0]);
    key(options()[0], "ArrowUp");
    expect(document.activeElement).toBe(options()[1]);
    key(options()[1], "Home");
    expect(document.activeElement).toBe(options()[0]);
    key(options()[0], "End");
    expect(document.activeElement).toBe(options()[1]);
    key(options()[1], "Escape");
    expect(options()).toHaveLength(0);
    expect(document.activeElement).toBe(trigger());
    expect(createDefaultPreset).not.toHaveBeenCalled();
  });

  it("toggles closed and dismisses on outside interaction, focus, resize and scroll", () => {
    trigger().click();
    trigger().click();
    expect(options()).toHaveLength(0);
    const outside = document.getElementById("outside")!;
    for (const dismiss of [
      () => outside.dispatchEvent(new Event("pointerdown", { bubbles: true })),
      () => outside.focus(),
      () => window.dispatchEvent(new Event("resize")),
      () => document.dispatchEvent(new Event("scroll")),
    ]) {
      trigger().click();
      expect(options()).toHaveLength(2);
      dismiss();
      expect(options()).toHaveLength(0);
    }
    expect(createDefaultPreset).not.toHaveBeenCalled();
  });

  it("stays open through a scroll elsewhere and a window blur, so the choice still lands", () => {
    document.body.insertAdjacentHTML("beforeend", '<div id="elsewhere"><p>list</p></div>');
    trigger().click();
    document.getElementById("elsewhere")!.dispatchEvent(new Event("scroll"));
    window.dispatchEvent(new Event("blur"));
    expect(options()).toHaveLength(2);
    options()[1].click();
    expect(createDefaultPreset).toHaveBeenCalledWith("basic");
  });

  it("stays open when pressing a template moves focus to the page, as WebKit on macOS does", () => {
    trigger().click();
    const [, basic] = options();
    basic.dispatchEvent(new Event("pointerdown", { bubbles: true }));
    // WebKit does not focus a clicked button: focus leaves the highlighted item for <html>.
    document.documentElement.dispatchEvent(new FocusEvent("focusin", { bubbles: true }));
    document.body.dispatchEvent(new FocusEvent("focusin", { bubbles: true }));
    expect(options()).toHaveLength(2);
    basic.click();
    expect(createDefaultPreset).toHaveBeenCalledWith("basic");
  });

  it("closes when a scroll moves its button", () => {
    document.body.insertAdjacentHTML("beforeend", '<div id="bar"></div>');
    const bar = document.getElementById("bar")!;
    bar.append(trigger());
    trigger().click();
    bar.dispatchEvent(new Event("scroll"));
    expect(options()).toHaveLength(0);
  });

  it("handles a dynamically rendered details button and ignores disabled triggers", () => {
    document.body.insertAdjacentHTML("beforeend", '<button id="create-default-preset">New Preset</button>');
    document.getElementById("create-default-preset")!.click();
    expect(options()).toHaveLength(2);
    options()[1].click();
    expect(createDefaultPreset).toHaveBeenCalledTimes(1);
    expect(createDefaultPreset).toHaveBeenCalledWith("basic");
    trigger().disabled = true;
    key(trigger(), "ArrowDown");
    expect(options()).toHaveLength(0);
  });
});
