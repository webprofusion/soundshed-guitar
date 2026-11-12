/**
 * At compact density the preset toolbar gives way to the preset name: its buttons leave the bar
 * in order (Random, Publish, Edit, Save) until the name has its room, and each one that left
 * shows as a "..." menu entry carrying the button's disabled state.
 */
import { describe, expect, it } from "vitest";

const { fitToolbar, isShortOfRoom, nameRoomNeeded, syncOverflowItems, OVERFLOW_ORDER } = await import("../ts/presets/toolbarFit.js");

document.body.innerHTML = `
  <div class="preset-toolbar">
    ${OVERFLOW_ORDER.map((id) => `<button id="${id}" class="preset-action-btn"></button>`).join("")}
  </div>
  <div id="menu">
    ${OVERFLOW_ORDER.map((id) => `<button data-overflow-for="${id}" hidden></button>`).join("")}
  </div>`;

const buttons = OVERFLOW_ORDER.map((id) => document.getElementById(id) as HTMLButtonElement);
const menu = document.getElementById("menu") as HTMLElement;
const away = () => buttons.filter((b) => b.classList.contains("is-toolbar-overflow")).map((b) => b.id);
const item = (id: string) => menu.querySelector<HTMLButtonElement>(`[data-overflow-for="${id}"]`) as HTMLButtonElement;

/** A strip where each button that leaves gives the name 40px more. */
function strip(width: number, nameWithAll: number) {
  return () => ({ name: nameWithAll + 40 * away().length, strip: width, toolbarOverflows: false });
}

describe("preset toolbar fit", () => {
  it("wants the larger of 200px and 40% of the strip for the name", () => {
    expect(nameRoomNeeded(400)).toBe(200);
    expect(nameRoomNeeded(1000)).toBe(400);
    expect(isShortOfRoom({ name: 199, strip: 400, toolbarOverflows: false })).toBe(true);
    expect(isShortOfRoom({ name: 200, strip: 400, toolbarOverflows: false })).toBe(false);
    expect(isShortOfRoom({ name: 300, strip: 400, toolbarOverflows: true })).toBe(true);
  });

  it("gives a preset with a status badge the badge's room", () => {
    expect(isShortOfRoom({ name: 320, strip: 760, toolbarOverflows: false })).toBe(false);
    expect(isShortOfRoom({ name: 320, strip: 760, toolbarOverflows: false, badge: true })).toBe(true);
    expect(isShortOfRoom({ name: 340, strip: 760, toolbarOverflows: false, badge: true })).toBe(false);
  });

  it("moves buttons for the badge only when that makes it show", () => {
    const withBadge = (width: number, nameWithAll: number) => () => ({ ...strip(width, nameWithAll)(), badge: true });
    // 760 wide: the name needs 304 and has it; the badge needs 340, one button away.
    expect(fitToolbar(buttons, withBadge(760, 310), true)).toBe(1);
    expect(away()).toEqual(["preset-random-btn"]);
    // 600 wide: the name needs 240 (three buttons); the badge is out of reach even
    // with the fourth gone, so Save stays.
    expect(fitToolbar(buttons, withBadge(600, 140), true)).toBe(3);
    expect(away()).toEqual(["preset-random-btn", "preset-publish-btn", "preset-edit-btn"]);
  });

  it("moves nothing when the name has its room", () => {
    expect(fitToolbar(buttons, strip(667, 300), true)).toBe(0);
    expect(away()).toEqual([]);
  });

  it("moves Random first, then Publish, then Edit, then Save, only as far as needed", () => {
    expect(fitToolbar(buttons, strip(667, 190), true)).toBe(2);
    expect(away()).toEqual(["preset-random-btn", "preset-publish-btn"]);
    expect(fitToolbar(buttons, strip(667, 20), true)).toBe(4);
    expect(away()).toEqual([...OVERFLOW_ORDER]);
  });

  it("puts everything back when the room returns or the density is not compact", () => {
    fitToolbar(buttons, strip(667, 20), true);
    expect(fitToolbar(buttons, strip(1024, 500), true)).toBe(0);
    expect(away()).toEqual([]);
    fitToolbar(buttons, strip(667, 20), true);
    expect(fitToolbar(buttons, strip(667, 20), false)).toBe(0);
    expect(away()).toEqual([]);
  });

  it("skips a button that is hidden anyway", () => {
    buttons[1]!.hidden = true; // Publish hidden (signed out, say)
    fitToolbar(buttons, strip(667, 190), true);
    expect(away()).toEqual(["preset-random-btn", "preset-edit-btn"]);
    buttons[1]!.hidden = false;
  });

  it("shows a menu entry only for a button that left, with its disabled state", () => {
    fitToolbar(buttons, strip(667, 190), true);
    buttons[0]!.disabled = true;
    syncOverflowItems(menu);
    expect(item("preset-random-btn").hidden).toBe(false);
    expect(item("preset-random-btn").disabled).toBe(true);
    expect(item("preset-publish-btn").hidden).toBe(false);
    expect(item("preset-publish-btn").disabled).toBe(false);
    expect(item("preset-edit-btn").hidden).toBe(true);
    expect(item("preset-save-btn").hidden).toBe(true);
    buttons[0]!.disabled = false;
  });
});
