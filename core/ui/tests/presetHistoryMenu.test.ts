/**
 * The preset load history's menu entries (compact density shows them in the preset "..." menu in
 * place of the toolbar's back/forward buttons): they follow the buttons' disabled state as the
 * history changes, and step the same way.
 */
import { describe, expect, it, vi } from "vitest";

vi.mock("../ts/presets/filter.js", () => ({ updatePresetDropdownSelection: vi.fn() }));

document.body.innerHTML = `
  <button id="preset-undo" disabled></button>
  <button id="preset-redo" disabled></button>
  <div id="preset-extra-actions-menu" role="menu">
    <button id="preset-history-back-item" role="menuitem" disabled>Back to previous preset</button>
    <button id="preset-history-forward-item" role="menuitem" disabled>Forward to next preset</button>
  </div>
`;

const { uiState } = await import("../ts/state.js");
const { recordPresetInHistory, stepPresetHistory } = await import("../ts/presets/history.js");

const byId = (id: string) => document.getElementById(id) as HTMLButtonElement;
const states = () => ({
  back: byId("preset-undo").disabled,
  backItem: byId("preset-history-back-item").disabled,
  forward: byId("preset-redo").disabled,
  forwardItem: byId("preset-history-forward-item").disabled,
});

async function load(presetId: string): Promise<void> {
  uiState.activePresetId = presetId;
}

describe("preset load history in the menu", () => {
  it("enables Back once there is somewhere to go back to, Forward after stepping back", async () => {
    recordPresetInHistory("factory-a");
    expect(states()).toEqual({ back: true, backItem: true, forward: true, forwardItem: true });

    recordPresetInHistory("factory-b");
    expect(states()).toEqual({ back: false, backItem: false, forward: true, forwardItem: true });

    await stepPresetHistory(-1, load);
    expect(uiState.activePresetId).toBe("factory-a");
    expect(states()).toEqual({ back: true, backItem: true, forward: false, forwardItem: false });

    await stepPresetHistory(1, load);
    expect(uiState.activePresetId).toBe("factory-b");
    expect(states()).toEqual({ back: false, backItem: false, forward: true, forwardItem: true });
  });
});
