/**
 * The MODE control beside the IN knob: the trigger shows the mode's icon and name,
 * the flyout lists the modes with what each needs, choosing one sends it (and stores it in the
 * standalone app), and a DAW leaves only the track's own layout and dual mono.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as InputModeModule from "../ts/inputMode.js";

type Sent = { type: string; [key: string]: unknown };

const INPUT_GROUP = readFileSync(join(__dirname, "..", "ui-components", "input-control-group.html"), "utf8");

let inputMode: typeof InputModeModule;
let sent: Sent[];

const trigger = (): HTMLButtonElement => document.getElementById("input-mode-trigger") as HTMLButtonElement;
const flyout = (): HTMLDivElement => document.getElementById("input-mode-flyout") as HTMLDivElement;
const options = (): HTMLButtonElement[] => Array.from(flyout().querySelectorAll<HTMLButtonElement>(".input-mode-option"));
const option = (mode: string): HTMLButtonElement => {
  const found = options().find((candidate) => candidate.dataset.mode === mode);
  if (!found) {
    throw new Error(`no option for ${mode}`);
  }
  return found;
};
const describedAs = (mode: string): string => option(mode).querySelector(".input-mode-option-description")?.textContent ?? "";
const triggerLabel = (): string => trigger().querySelector(".input-mode-trigger-label")?.textContent ?? "";
const triggerIcon = (): string | undefined => trigger().querySelector<HTMLElement>(".input-mode-trigger-icon")?.dataset.icon;
const inputModeMessages = (): Sent[] => sent.filter((message) => message.type === "setInputMode");
const settingsWritten = (): Sent[] => sent.filter((message) => message.type === "setSetting");

function key(target: HTMLElement, name: string): void {
  target.dispatchEvent(new KeyboardEvent("keydown", { key: name, bubbles: true }));
}

async function start(standalone: boolean, appSettings: Record<string, unknown> = {}): Promise<void> {
  document.body.className = standalone ? "is-standalone" : "";
  document.body.innerHTML = `<section class="control-bar"><div class="control-bar-group">${INPUT_GROUP}</div></section>`;
  vi.resetModules();
  const { uiState } = await import("../ts/state.js");
  uiState.appSettings = appSettings;
  inputMode = await import("../ts/inputMode.js");
  inputMode.initializeInputModeControls();
}

beforeEach(() => {
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as Sent);
  };
});

afterEach(() => {
  delete window.IPlugSendMsg;
});

describe("in the standalone app", () => {
  it("shows the stored mode under the knob and applies it", async () => {
    await start(true, { "inputChannel.monoMode": true, "inputChannel.mono": 1 });

    expect(triggerLabel()).toBe("Mono In 2");
    expect(triggerIcon()).toBe("mono2");
    expect(trigger().querySelector(".input-mode-trigger-icon svg")).not.toBeNull();
    expect(inputModeMessages().at(-1)).toMatchObject({ mode: "mono2", monoMode: true, inputChannel: 1, dualMono: false });
  });

  it("lists every mode, ticks the current one, and switches on a click", async () => {
    await start(true, { "inputChannel.monoMode": true, "inputChannel.mono": 1 });

    trigger().click();
    expect(flyout().hidden).toBe(false);
    expect(trigger().getAttribute("aria-expanded")).toBe("true");
    expect(flyout().parentElement).toBe(document.body);
    expect(options().map((candidate) => candidate.dataset.mode)).toEqual(["mono1", "mono2", "monoSum", "stereo", "dualMono"]);
    expect(options().filter((candidate) => candidate.getAttribute("aria-checked") === "true").map((c) => c.dataset.mode)).toEqual(["mono2"]);
    expect(options().every((candidate) => candidate.querySelector(".input-mode-option-icon svg"))).toBe(true);
    expect(flyout().querySelector(".input-mode-flyout-running")).toBeNull();

    sent = [];
    option("stereo").click();

    expect(inputModeMessages()).toEqual([{ type: "setInputMode", mode: "stereo", monoMode: false, inputChannel: 0, dualMono: false }]);
    expect(settingsWritten().map((message) => [message.key, message.value])).toEqual([
      ["inputChannel.monoMode", false],
      ["inputChannel.mono", 0],
      ["inputChannel.dualMono", false],
    ]);
    expect(flyout().hidden).toBe(true);
    expect(document.activeElement).toBe(trigger());
    expect(triggerLabel()).toBe("Stereo");
    expect(triggerIcon()).toBe("stereo");
  });

  it("greys what a one-input device cannot give, and flags the mode that fell back", async () => {
    await start(true);
    inputMode.handleInputModeChanged({ mode: "stereo", effectiveMode: "mono", inputChannels: 1, outputChannels: 2 });

    expect(trigger().classList.contains("is-fallback")).toBe(true);
    expect(trigger().title).toContain("Running in mono");

    trigger().click();
    expect(option("mono1").disabled).toBe(false);
    for (const mode of ["mono2", "monoSum", "stereo"]) {
      expect(option(mode).disabled).toBe(true);
      expect(describedAs(mode)).toBe("Needs a second input");
    }
    expect(describedAs("dualMono")).toBe("Needs two inputs and two outputs");
    expect(flyout().querySelector(".input-mode-flyout-running")?.textContent).toBe("Running mono");
    expect(flyout().querySelector(".input-mode-note")?.textContent).toContain("one input");

    sent = [];
    option("stereo").click();
    expect(inputModeMessages()).toEqual([]);
  });

  it("marks dual mono when hosted plugins still hear both inputs", async () => {
    await start(true);
    inputMode.handleInputModeChanged({ mode: "dualMono", effectiveMode: "dualMono", inputChannels: 2, outputChannels: 2, dualMonoShared: 2 });

    expect(triggerLabel()).toBe("Dual mono");
    expect(trigger().classList.contains("has-shared-plugins")).toBe(true);
    expect(trigger().classList.contains("is-fallback")).toBe(false);
    expect(trigger().title).toContain("2 hosted plugins hear both inputs");
  });

  it("works from the keyboard and closes on Escape or a click elsewhere", async () => {
    await start(true, { "inputChannel.monoMode": true, "inputChannel.mono": 2 });

    trigger().focus();
    key(trigger(), "ArrowDown");
    expect(flyout().hidden).toBe(false);
    expect(document.activeElement).toBe(option("monoSum"));

    key(flyout(), "ArrowDown");
    expect(document.activeElement).toBe(option("stereo"));
    key(flyout(), "End");
    expect(document.activeElement).toBe(option("dualMono"));
    key(flyout(), "ArrowDown");
    expect(document.activeElement).toBe(option("mono1"));

    key(flyout(), "Escape");
    expect(flyout().hidden).toBe(true);
    expect(document.activeElement).toBe(trigger());

    trigger().click();
    expect(flyout().hidden).toBe(false);
    document.body.dispatchEvent(new MouseEvent("pointerdown", { bubbles: true }));
    expect(flyout().hidden).toBe(true);
  });
});

describe("in a DAW", () => {
  it("offers the track's own layout and dual mono, which a mono track cannot have", async () => {
    await start(false);
    expect(inputModeMessages()).toEqual([{ type: "setInputMode" }]);

    inputMode.handleInputModeChanged({ mode: "stereo", hostControlled: true, effectiveMode: "mono", inputChannels: 1, outputChannels: 2 });
    expect(triggerLabel()).toBe("Track");
    expect(triggerIcon()).toBe("mono");
    expect(trigger().classList.contains("is-fallback")).toBe(false);

    trigger().click();
    expect(options().map((candidate) => candidate.dataset.mode)).toEqual(["stereo", "dualMono"]);
    expect(describedAs("stereo")).toBe("Mono, as the track is");
    expect(option("dualMono").disabled).toBe(true);
    expect(describedAs("dualMono")).toBe("Needs a stereo track");
  });

  it("keeps dual mono with the instance, not in shared settings", async () => {
    await start(false);
    inputMode.handleInputModeChanged({ mode: "stereo", hostControlled: true, effectiveMode: "stereo", inputChannels: 2, outputChannels: 2 });
    expect(triggerIcon()).toBe("stereo");

    trigger().click();
    sent = [];
    option("dualMono").click();

    expect(inputModeMessages()).toEqual([{ type: "setInputMode", mode: "dualMono", monoMode: false, inputChannel: 0, dualMono: true }]);
    expect(settingsWritten()).toEqual([]);
    expect(triggerLabel()).toBe("Dual mono");
  });
});
