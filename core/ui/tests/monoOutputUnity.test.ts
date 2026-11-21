import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";

const postMessage = vi.fn();
vi.mock("../ts/bridge.js", () => ({
  setAppSetting: (key: string, value: unknown) => postMessage({ type: "setSetting", key, value }),
}));

const PERFORMANCE_PANEL = readFileSync(join(__dirname, "..", "ui-components", "panels", "settings-performance.html"), "utf8");
const KEY = "audio.dsp.monoOutputUnity";

const { uiState } = await import("../ts/state.js");
const toggleModule = await import("../ts/settings/monoOutputUnity.js");

function toggle(): HTMLInputElement {
  return document.getElementById("dsp-mono-output-unity-toggle") as HTMLInputElement;
}

function note(): HTMLElement {
  return document.getElementById("dsp-mono-output-unity-instance") as HTMLElement;
}

beforeEach(() => {
  postMessage.mockClear();
  document.body.innerHTML = PERFORMANCE_PANEL;
  uiState.appSettings = {};
  uiState.environment = { standalone: false };
});

describe("Full Level on Mono Outputs", () => {
  it("shows the stored setting and sends a change", () => {
    uiState.appSettings = { [KEY]: true };
    toggleModule.initMonoOutputUnityToggle();
    expect(toggle().checked).toBe(true);

    toggle().checked = false;
    toggle().dispatchEvent(new Event("change"));
    expect(postMessage).toHaveBeenCalledWith({ type: "setSetting", key: KEY, value: false });
  });

  it("says when a plugin instance keeps a value other than the setting", () => {
    uiState.appSettings = { [KEY]: true };
    toggleModule.applyMonoOutputUnityInstance(false);
    toggleModule.refreshMonoOutputUnityToggle();
    expect(note().hidden).toBe(false);
    expect(note().textContent).toContain("keeps its own value (off)");

    toggleModule.applyMonoOutputUnityInstance(true);
    toggleModule.refreshMonoOutputUnityToggle();
    expect(note().textContent).toBe("This instance: on.");
  });

  it("says nothing about an instance in the standalone, which follows the setting", () => {
    uiState.environment = { standalone: true };
    toggleModule.applyMonoOutputUnityInstance(false);
    toggleModule.refreshMonoOutputUnityToggle();
    expect(note().hidden).toBe(true);
  });
});
