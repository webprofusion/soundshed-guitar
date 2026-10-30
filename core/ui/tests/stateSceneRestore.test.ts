/**
 * A reopened editor's first "state" carries the scene the engine is playing. Applying the app
 * settings in the same message redraws the chain from whatever preset was shown before, which
 * left that preset's scene in uiState; the editor then opened on the first scene while the
 * engine played the last one chosen.
 */
import { describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/updateCheck.js", () => ({ triggerUpdateCheck: vi.fn() }));

// The redraw the recents pass sets off, as it leaves uiState: the placeholder preset's scene.
const strayRedraw = vi.fn();
vi.mock("../ts/presets.js", () => ({
  applyPresetArchiveSessionState: vi.fn(),
  applyPresetRecentsFromAppSettings: () => strayRedraw(),
  populatePresetDropdown: vi.fn(),
  rejectPendingPresetRequest: vi.fn(),
  renderActivePreset: vi.fn(),
  updatePresetActionButtons: vi.fn(),
  updatePresetDropdownSelection: vi.fn(),
}));

window.IPlugSendMsg = () => undefined;
document.body.innerHTML = `<button id="output-mute-toggle"></button><div id="footer-demo-audio-container"></div>`;

const { uiState } = await import("../ts/state.js");
const { onState } = await import("../ts/messages/stateHandlers.js");

function sceneGraph(nodeId: string) {
  return { nodes: [{ id: nodeId, type: "gain", params: {} }], edges: [] };
}

describe("the first state after an editor opens", () => {
  it("opens on the engine's scene even when the settings pass moved uiState's", () => {
    strayRedraw.mockImplementation(() => {
      uiState.activePresetSceneId = "scene-1";
    });

    onState({
      type: "state",
      activePresetId: "rig",
      activeSceneId: "scene-4",
      appSettings: {},
      preset: {
        id: "rig",
        name: "Rig",
        graph: sceneGraph("d"),
        scenes: [
          { id: "scene-5", title: "Clean", graph: sceneGraph("a") },
          { id: "scene-2", title: "Crunch", graph: sceneGraph("b") },
          { id: "scene-4", title: "Lead", graph: sceneGraph("d") },
        ],
      },
    });

    expect(strayRedraw).toHaveBeenCalled();
    expect(uiState.activePresetSceneId).toBe("scene-4");
    expect(uiState.activePresetDraft?.graph.nodes[0]?.id).toBe("d");
  });
});
