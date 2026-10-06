import { beforeEach, describe, expect, it, vi } from "vitest";
import type { PracticeToolProject } from "../ts/types";

const recallPreset = vi.fn();
const applyProject = vi.fn();

const project = {
  id: "project-1",
  name: "Another Day",
  filePath: "C:/Tracks/Another Day.mp3",
  fileTitle: "Another Day.mp3",
  loops: [],
  presetId: "solo-tone",
  presetName: "Solo Tone",
} as unknown as PracticeToolProject;

vi.mock("../ts/bridge.js", () => ({ loadPracticeToolFile: vi.fn() }));
vi.mock("../ts/dialogs.js", () => ({ showConfirm: vi.fn() }));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));
vi.mock("../ts/practiceTool/projects.js", () => ({
  canRecallPresets: () => true,
  capturePracticeToolProject: vi.fn(),
  deletePracticeToolProject: vi.fn(),
  findPracticeToolProject: (id: string) => (id === project.id ? project : null),
  findPracticeToolProjectByName: vi.fn(),
  getProjectTrackAvailability: () => "loaded",
  readPracticeToolProjects: () => [project],
  requestPracticeToolPresetRecall: (presetId: string) => recallPreset(presetId),
  requestPracticeToolProjectApply: (recalled: PracticeToolProject) => applyProject(recalled),
  setPendingProjectRecall: vi.fn(),
  storePracticeToolProject: vi.fn(),
}));

const { uiState } = await import("../ts/state.js");
const { bindPracticeToolProjectActions, renderPracticeToolProjects } = await import("../ts/practiceTool/projectsPanel.js");

// Picking a project from the list loads it — there is no Load button.
function loadProject(): void {
  const select = document.getElementById("practice-tool-project-select") as HTMLSelectElement;
  select.value = project.id;
  select.dispatchEvent(new Event("change"));
}

beforeEach(() => {
  recallPreset.mockReset();
  applyProject.mockReset();
  document.body.innerHTML = `
    <select id="practice-tool-project-select"></select>
    <input id="practice-tool-project-name" />
  `;
  bindPracticeToolProjectActions();
  renderPracticeToolProjects();
});

describe("loading a practice tool project", () => {
  it("leaves its preset alone when it is the one already loaded, keeping unsaved edits", () => {
    uiState.activePresetId = "solo-tone";
    uiState.presetDirty = true;

    loadProject();

    expect(recallPreset).not.toHaveBeenCalled();
    expect(applyProject).toHaveBeenCalledWith(project);
  });

  it("recalls its preset when another one is loaded", () => {
    uiState.activePresetId = "rhythm-tone";

    loadProject();

    expect(recallPreset).toHaveBeenCalledWith("solo-tone");
    expect(applyProject).toHaveBeenCalledWith(project);
  });
});
