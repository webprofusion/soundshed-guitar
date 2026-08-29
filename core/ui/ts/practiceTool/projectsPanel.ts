/**
 * The Practice Tool's project bar: save the current track + loops + fader
 * settings (optionally with the selected preset) under a name, and recall it.
 *
 * Storage and the recall seams live in `./projects.js`; this module is only the
 * controls. Like the rest of the Practice Tool it never imports `practiceTool.ts`
 * — recall goes back through `requestPracticeToolProjectApply`.
 */

import { loadPracticeToolFile } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { uiState } from "../state.js";
import type { PracticeToolProject } from "../types.js";
import { escapeHtml } from "../utils.js";
import {
  canRecallPresets,
  capturePracticeToolProject,
  deletePracticeToolProject,
  findPracticeToolProject,
  findPracticeToolProjectByName,
  getProjectTrackAvailability,
  readPracticeToolProjects,
  requestPracticeToolPresetRecall,
  requestPracticeToolProjectApply,
  setPendingProjectRecall,
  storePracticeToolProject,
} from "./projects.js";

// Which saved project the bar is pointing at. Not persisted: it is a cursor
// into the list, not part of the session, and starting fresh each launch keeps
// "Save" from ever defaulting to overwriting something from a previous sitting.
let selectedProjectId: string | null = null;

function getSelect(): HTMLSelectElement | null {
  return document.getElementById("practice-tool-project-select") as HTMLSelectElement | null;
}

function getNameInput(): HTMLInputElement | null {
  return document.getElementById("practice-tool-project-name") as HTMLInputElement | null;
}

function getIncludePresetToggle(): HTMLInputElement | null {
  return document.getElementById("practice-tool-project-include-preset") as HTMLInputElement | null;
}

function describeProjectOption(project: PracticeToolProject): string {
  return project.presetId ? `${project.name} ♪` : project.name;
}

/** The one line under the bar that explains what Load will actually do — which
 * track and preset the selection carries, and whether its file can be reopened. */
function buildHintText(projects: readonly PracticeToolProject[]): string {
  const player = uiState.practiceTool;
  const selected = selectedProjectId ? projects.find((project) => project.id === selectedProjectId) ?? null : null;

  if (selected) {
    const parts = [`${selected.fileTitle} · ${selected.loops.length} loop${selected.loops.length === 1 ? "" : "s"}`];
    if (selected.presetName) {
      parts.push(`preset "${selected.presetName}"`);
    } else if (selected.presetId) {
      parts.push("a saved preset");
    }
    if (getProjectTrackAvailability(selected) === "unavailable") {
      return `${parts.join(" · ")} — this track was dragged in, so its file can't be reopened automatically. Open it again, then load this project.`;
    }
    return parts.join(" · ");
  }

  if (!projects.length) {
    return player?.filePath
      ? "Save the loaded track, its loops and the Volume/Balance/Speed/Pitch settings as a project you can recall later."
      : "Load a track to save it, its loops and its settings as a project.";
  }
  return "Pick a saved project to load it, or type a name and save the current one.";
}

export function renderPracticeToolProjects(): void {
  const select = getSelect();
  const nameInput = getNameInput();
  const includePreset = getIncludePresetToggle();
  const loadBtn = document.getElementById("practice-tool-project-load") as HTMLButtonElement | null;
  const deleteBtn = document.getElementById("practice-tool-project-delete") as HTMLButtonElement | null;
  const saveBtn = document.getElementById("practice-tool-project-save") as HTMLButtonElement | null;
  const hint = document.getElementById("practice-tool-project-hint");
  if (!select) {
    return;
  }

  const projects = readPracticeToolProjects();
  if (selectedProjectId && !projects.some((project) => project.id === selectedProjectId)) {
    selectedProjectId = null;
  }

  // The panel re-renders on every loop edit and every panel activation, but
  // the option set rarely changes — and rewriting innerHTML would drop focus
  // out of an open dropdown. Rebuild only when the list actually differs.
  const signature = projects.map((project) => `${project.id}:${describeProjectOption(project)}`).join("|");
  if (select.dataset.signature !== signature) {
    select.dataset.signature = signature;
    select.innerHTML = `<option value="">${projects.length ? "Saved projects…" : "No saved projects"}</option>`
      + projects
        .map((project) => `<option value="${escapeHtml(project.id)}">${escapeHtml(describeProjectOption(project))}</option>`)
        .join("");
  }
  select.value = selectedProjectId ?? "";
  select.disabled = projects.length === 0;

  const player = uiState.practiceTool;
  const hasTrack = Boolean(player?.filePath);
  const typedName = nameInput?.value.trim() ?? "";

  if (loadBtn) {
    loadBtn.disabled = !selectedProjectId;
  }
  if (deleteBtn) {
    deleteBtn.disabled = !selectedProjectId;
  }
  if (saveBtn) {
    saveBtn.disabled = !hasTrack || !typedName;
  }
  if (includePreset) {
    // Nothing to attach without a selected preset, and nothing to restore one
    // with unless main.ts registered the recaller.
    includePreset.disabled = !uiState.activePresetId || !canRecallPresets();
    if (includePreset.disabled) {
      includePreset.checked = false;
    }
  }
  if (hint) {
    hint.textContent = buildHintText(projects);
  }
}

async function saveCurrentProject(): Promise<void> {
  const nameInput = getNameInput();
  const name = nameInput?.value.trim() ?? "";
  const player = uiState.practiceTool;

  if (!player?.filePath) {
    showNotification("Nothing to save", "Load a backing track first");
    return;
  }
  if (!name) {
    showNotification("Name the project", "Type a name before saving");
    nameInput?.focus();
    return;
  }

  // Saving over the same name is the natural way to update a project, so it
  // asks rather than quietly accumulating duplicates that only differ by id.
  const existing = findPracticeToolProjectByName(name);
  if (existing && !(await showConfirm(`Overwrite the saved project "${existing.name}"?`, "Overwrite project"))) {
    return;
  }

  const includePreset = getIncludePresetToggle()?.checked ?? false;
  const project = capturePracticeToolProject(name, { includePreset, projectId: existing?.id });
  if (!project) {
    showNotification("Unable to save project", "The Practice Tool has no track loaded");
    return;
  }
  if (!storePracticeToolProject(project)) {
    showNotification("Too many saved projects", "Delete one before saving another");
    return;
  }

  selectedProjectId = project.id;
  renderPracticeToolProjects();
  appendLog(`practice tool project saved → ${project.name} (${project.loops.length} loops${project.presetId ? ", with preset" : ""})`);
  showNotification(existing ? `Project "${project.name}" updated` : `Project "${project.name}" saved`);
}

function loadSelectedProject(): void {
  if (!selectedProjectId) {
    return;
  }
  const project = findPracticeToolProject(selectedProjectId);
  if (!project) {
    renderPracticeToolProjects();
    return;
  }

  const availability = getProjectTrackAvailability(project);
  if (availability === "unavailable") {
    showNotification(
      "Track file unavailable",
      `"${project.fileTitle}" was dragged in, so only its name was recorded — open the file again, then load this project`
    );
    return;
  }

  // Fired first and left to run on its own: the preset load has its own
  // unsaved-changes confirmation, and the track is restored either way.
  if (project.presetId) {
    requestPracticeToolPresetRecall(project.presetId);
  }

  if (availability === "loaded") {
    requestPracticeToolProjectApply(project);
  } else {
    // The settings can only be applied once the engine has the audio; parked
    // here and cashed in by applyPracticeToolFileLoaded.
    setPendingProjectRecall(project);
    loadPracticeToolFile(project.filePath);
  }
  appendLog(`practice tool project loaded → ${project.name} (${availability === "loaded" ? "track already open" : project.filePath})`);
}

async function deleteSelectedProject(): Promise<void> {
  if (!selectedProjectId) {
    return;
  }
  const project = findPracticeToolProject(selectedProjectId);
  if (!project) {
    renderPracticeToolProjects();
    return;
  }
  if (!(await showConfirm(`Delete the saved project "${project.name}"? The track file and its loops are not affected.`, "Delete project"))) {
    return;
  }
  deletePracticeToolProject(project.id);
  selectedProjectId = null;
  renderPracticeToolProjects();
  appendLog(`practice tool project deleted → ${project.name}`);
}

export function bindPracticeToolProjectActions(): void {
  const select = getSelect();
  if (select && select.dataset.bound !== "true") {
    select.dataset.bound = "true";
    select.addEventListener("change", () => {
      selectedProjectId = select.value || null;
      const project = selectedProjectId ? findPracticeToolProject(selectedProjectId) : null;
      const nameInput = getNameInput();
      // Selecting a project arms Save to update *that* project, so the name
      // field follows the selection — Save then reads as "save these settings
      // to the project I picked", and clearing the field opts back out.
      if (nameInput && project) {
        nameInput.value = project.name;
      }
      const includePreset = getIncludePresetToggle();
      if (includePreset && project && !includePreset.disabled) {
        includePreset.checked = Boolean(project.presetId);
      }
      renderPracticeToolProjects();
    });
  }

  const nameInput = getNameInput();
  if (nameInput && nameInput.dataset.bound !== "true") {
    nameInput.dataset.bound = "true";
    nameInput.addEventListener("input", () => renderPracticeToolProjects());
    nameInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        void saveCurrentProject();
      }
    });
  }

  const saveBtn = document.getElementById("practice-tool-project-save") as HTMLButtonElement | null;
  if (saveBtn && saveBtn.dataset.bound !== "true") {
    saveBtn.dataset.bound = "true";
    saveBtn.addEventListener("click", () => void saveCurrentProject());
  }

  const loadBtn = document.getElementById("practice-tool-project-load") as HTMLButtonElement | null;
  if (loadBtn && loadBtn.dataset.bound !== "true") {
    loadBtn.dataset.bound = "true";
    loadBtn.addEventListener("click", () => loadSelectedProject());
  }

  const deleteBtn = document.getElementById("practice-tool-project-delete") as HTMLButtonElement | null;
  if (deleteBtn && deleteBtn.dataset.bound !== "true") {
    deleteBtn.dataset.bound = "true";
    deleteBtn.addEventListener("click", () => void deleteSelectedProject());
  }
}
