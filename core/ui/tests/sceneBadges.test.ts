/**
 * Scene badges over the signal chain: one pill per scene once a preset has two, a + badge
 * to add one, the × removing that scene once confirmed, and a double-click renaming it in
 * place without a re-render throwing the half-typed name away.
 */
import { beforeEach, describe, expect, it, vi, type Mock } from "vitest";
import type { Preset } from "../ts/types";

const showConfirm = vi.hoisted(() => vi.fn<(message: string, title?: string, labels?: { ok?: string }) => Promise<boolean>>());
vi.mock("../ts/dialogs.js", () => ({ showConfirm }));

import { clearSceneBadges, renderSceneBadges, type SceneBadgeActions } from "../ts/signalPath/sceneBadges";

function presetWithScenes(...titles: string[]): Preset {
  return {
    id: "p1",
    name: "Test",
    scenes: titles.map((title, index) => ({ id: `s${index + 1}`, title })),
  } as unknown as Preset;
}

// Mock-typed properties, not methods, so expect(actions.select) reads a plain property.
function makeActions(): { [K in keyof SceneBadgeActions]: Mock } {
  return { select: vi.fn(), rename: vi.fn(), remove: vi.fn(), move: vi.fn(), add: vi.fn() };
}

function host(): HTMLElement {
  return document.getElementById("signal-path-scene-badges")!;
}

function badgeLabels(): string[] {
  return [...host().querySelectorAll(".scene-badge-label")].map((label) => label.textContent ?? "");
}

beforeEach(() => {
  showConfirm.mockReset();
  document.body.innerHTML = `<div id="signal-path-scene-badges" hidden></div>`;
});

describe("scene badges", () => {
  it("shows one badge per scene, marks the active one, and ends with a + badge", () => {
    renderSceneBadges(presetWithScenes("Rhythm", "Solo"), "s2", true, makeActions());

    expect(host().hidden).toBe(false);
    expect(badgeLabels()).toEqual(["Rhythm", "Solo"]);
    const active = host().querySelector(".scene-badge.is-active");
    expect(active?.getAttribute("data-scene-id")).toBe("s2");
    expect(host().lastElementChild?.className).toBe("scene-badge-add");
    expect(host().lastElementChild?.textContent).toBe("+");
  });

  it("shows only a labelled + badge for a preset with one scene", () => {
    renderSceneBadges(presetWithScenes("Default"), "s1", true, makeActions());

    expect(badgeLabels()).toEqual([]);
    expect(host().querySelector(".scene-badge-add")?.textContent).toBe("+ Scene");
  });

  it("hides the strip when there is nothing to show", () => {
    renderSceneBadges(presetWithScenes("Default"), "s1", false, makeActions());
    expect(host().hidden).toBe(true);

    renderSceneBadges(presetWithScenes("A", "B"), "s1", true, makeActions());
    clearSceneBadges();
    expect(host().hidden).toBe(true);
    expect(host().children).toHaveLength(0);
  });

  it("selects on click, removes that scene on × once confirmed, and adds on +", async () => {
    const actions = makeActions();
    renderSceneBadges(presetWithScenes("Rhythm", "Solo"), "s1", true, actions);

    host().querySelector<HTMLButtonElement>('[data-scene-id="s2"] .scene-badge-label')!.click();
    expect(actions.select).toHaveBeenCalledWith("s2");

    const close = host().querySelector<HTMLButtonElement>('[data-scene-id="s2"] .scene-badge-close')!;
    showConfirm.mockResolvedValueOnce(false);
    close.click();
    await vi.waitFor(() => expect(showConfirm).toHaveBeenCalledTimes(1));
    expect(showConfirm.mock.calls[0][0]).toContain("Solo");
    expect(actions.remove).not.toHaveBeenCalled();

    showConfirm.mockResolvedValueOnce(true);
    close.click();
    await vi.waitFor(() => expect(actions.remove).toHaveBeenCalledWith("s2"));
    expect(actions.select).toHaveBeenCalledTimes(1);

    host().querySelector<HTMLButtonElement>(".scene-badge-add")!.click();
    expect(actions.add).toHaveBeenCalledTimes(1);
  });

  it("selects and renames on a click that lands on the badge, as one does after a press", () => {
    // A press on the label captures the pointer to the badge (a drag may follow), so the
    // browser fires the click and double-click at the badge rather than the label.
    const actions = makeActions();
    renderSceneBadges(presetWithScenes("Rhythm", "Solo"), "s1", true, actions);

    const badge = host().querySelector<HTMLElement>('[data-scene-id="s2"]')!;
    badge.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    expect(actions.select).toHaveBeenCalledWith("s2");

    badge.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    const field = host().querySelector<HTMLInputElement>(".scene-badge-input")!;
    expect(field).not.toBeNull();

    // Clicks inside the name field are editing, not choosing the scene again.
    field.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    field.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    expect(actions.select).toHaveBeenCalledTimes(1);
    expect(host().querySelectorAll(".scene-badge-input")).toHaveLength(1);
  });

  it("renames in place on double-click and Enter, trimming the name", () => {
    const actions = makeActions();
    renderSceneBadges(presetWithScenes("Rhythm", "Solo"), "s1", true, actions);

    const label = host().querySelector<HTMLButtonElement>('[data-scene-id="s1"] .scene-badge-label')!;
    label.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    const field = host().querySelector<HTMLInputElement>(".scene-badge-input")!;
    expect(field).not.toBeNull();
    expect(document.activeElement).toBe(field);

    field.value = "  Verse  ";
    field.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));

    expect(actions.rename).toHaveBeenCalledWith("s1", "Verse");
    expect(host().querySelector(".scene-badge-input")).toBeNull();
    expect(badgeLabels()[0]).toBe("Verse");
  });

  it("drops the edit on Escape and ignores an unchanged or empty name", () => {
    const actions = makeActions();
    renderSceneBadges(presetWithScenes("Rhythm", "Solo"), "s1", true, actions);

    const openField = (): HTMLInputElement => {
      host().querySelector<HTMLButtonElement>('[data-scene-id="s1"] .scene-badge-label')!
        .dispatchEvent(new KeyboardEvent("keydown", { key: "F2", bubbles: true }));
      return host().querySelector<HTMLInputElement>(".scene-badge-input")!;
    };

    let field = openField();
    field.value = "Chorus";
    field.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));

    field = openField();
    field.value = "   ";
    field.blur();

    expect(actions.rename).not.toHaveBeenCalled();
    expect(badgeLabels()[0]).toBe("Rhythm");
  });

  it("keeps a rename in progress through a re-render", () => {
    const actions = makeActions();
    const preset = presetWithScenes("Rhythm", "Solo");
    renderSceneBadges(preset, "s1", true, actions);

    host().querySelector<HTMLButtonElement>('[data-scene-id="s1"] .scene-badge-label')!
      .dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    const field = host().querySelector<HTMLInputElement>(".scene-badge-input")!;
    field.value = "Half-typ";

    renderSceneBadges(preset, "s1", true, actions);
    clearSceneBadges();

    expect(host().querySelector(".scene-badge-input")).toBe(field);
    expect(field.value).toBe("Half-typ");
  });
});
