/**
 * The theme flyout's App UI choice (ts/settings/nativeEditor.ts): shown only where the host can
 * build either editor, bound at startup rather than when Settings first opens, and storing the
 * engine's ui.nativeEditor setting.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeAll, describe, expect, it } from "vitest";

const sent: string[] = [];
window.IPlugSendMsg = (message: unknown) => {
  sent.push(typeof message === "string" ? message : JSON.stringify(message));
};

const FOOTER = readFileSync(join(__dirname, "..", "ui-components", "footer-bar.html"), "utf8");
document.body.innerHTML = FOOTER;

const { uiState } = await import("../ts/state.js");
const { initNativeEditorToggle, refreshNativeEditorToggle, NATIVE_EDITOR_SETTING } = await import("../ts/settings/nativeEditor.js");

const section = () => document.getElementById("app-ui") as HTMLElement;
const choice = (native: boolean) =>
  section().querySelector<HTMLButtonElement>(`.app-ui-choice[data-native-editor="${native}"]`) as HTMLButtonElement;

describe("the App UI choice", () => {
  beforeAll(() => {
    initNativeEditorToggle();
  });

  it("is hidden where the host builds only one editor", () => {
    expect(section().hidden).toBe(true);
  });

  it("shows where the host can build either, marking the standard UI by default", () => {
    uiState.environment = { ...(uiState.environment ?? {}), nativeEditor: true, standalone: true } as typeof uiState.environment;
    refreshNativeEditorToggle();
    expect(section().hidden).toBe(false);
    expect(choice(false).getAttribute("aria-checked")).toBe("true");
    expect(choice(true).getAttribute("aria-checked")).toBe("false");
  });

  it("stores the native UI when chosen, and marks it", () => {
    sent.length = 0;
    choice(true).click();
    expect(uiState.appSettings?.[NATIVE_EDITOR_SETTING]).toBe(true);
    expect(sent.some((message) => message.includes(NATIVE_EDITOR_SETTING))).toBe(true);
    expect(choice(true).getAttribute("aria-checked")).toBe("true");
    expect(choice(false).getAttribute("aria-checked")).toBe("false");
  });

  it("sends nothing when the choice already on is chosen again", () => {
    sent.length = 0;
    choice(true).click();
    expect(sent).toEqual([]);
  });

  it("says a plugin changes when its window next opens", () => {
    uiState.environment = { ...(uiState.environment ?? {}), standalone: false } as typeof uiState.environment;
    refreshNativeEditorToggle();
    expect(document.getElementById("app-ui-hint")?.textContent).toContain("next opened");
  });
});
