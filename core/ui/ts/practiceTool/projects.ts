/**
 * Practice Tool persistence: the per-file loop store, and saved "projects".
 *
 * A project is everything the Practice Tool needs to pick a practice session
 * back up — which backing track was loaded, the loops defined on it, the four
 * fader settings, the backing-track EQ, and optionally the tone preset that was
 * selected. None of it
 * exists engine-side: it all lives in app settings, exactly like the per-file
 * loop autosave this module also owns.
 *
 * This module deliberately does not import `practiceTool.ts` — that facade
 * imports *this*, and the reverse edge would close a cycle through
 * presets -> navigation -> jam -> practiceTool. The two things it needs from
 * that side (apply a recalled project, load a preset by id) come in as
 * registered implementations, the same seam pattern as `signalPath/render.ts`.
 */

import { setAppSetting } from "../bridge.js";
import { uiState } from "../state.js";
import type { AppSettingValue, PracticeToolLoopRegion, PracticeToolProject } from "../types.js";
import { sanitizePracticeToolEq } from "./eq.js";

const LOOPS_SETTING_KEY = "practiceTool.loops";
const PROJECTS_SETTING_KEY = "practiceTool.projects";

/** Nothing enforces this in the UI beyond refusing to add the next one — it
 * exists so a runaway caller can't grow the settings blob without bound. */
export const MAX_PRACTICE_TOOL_PROJECTS = 200;

// ── Seams ────────────────────────────────────────────────────────────────────

let applyProject: ((project: PracticeToolProject) => void) | null = null;

/** Called once by practiceTool.ts, which owns the faders and the renderer. */
export function setPracticeToolProjectApplier(fn: (project: PracticeToolProject) => void): void {
  applyProject = fn;
}

/** Pushes a recalled project into the live player state. */
export function requestPracticeToolProjectApply(project: PracticeToolProject): void {
  applyProject?.(project);
}

let recallPreset: ((presetId: string) => Promise<void> | void) | null = null;

/** Called once by main.ts with `applyPresetFromLibrary`. */
export function setPracticeToolPresetRecaller(fn: (presetId: string) => Promise<void> | void): void {
  recallPreset = fn;
}

/** True when a preset recaller has been registered — the Practice Tool only
 * offers "include preset" when something can actually restore one. */
export function canRecallPresets(): boolean {
  return recallPreset !== null;
}

export function requestPracticeToolPresetRecall(presetId: string): void {
  void recallPreset?.(presetId);
}

// ── Per-file loop store ──────────────────────────────────────────────────────

/** `path` + `durationSec` are the only stable identifiers `practiceToolFileLoaded`
 * gives us, so they are what loops (and project matching) key off. */
export function getFileFingerprint(filePath: string, durationSec: number): string {
  return `${filePath.trim().toLowerCase()}|${durationSec.toFixed(2)}`;
}

function isPersistedLoop(value: unknown): value is PracticeToolLoopRegion {
  if (!value || typeof value !== "object") {
    return false;
  }
  const record = value as Record<string, unknown>;
  return typeof record.id === "string"
    && typeof record.name === "string"
    && typeof record.startSec === "number"
    && typeof record.endSec === "number";
}

export function loadLoopsForFingerprint(fingerprint: string): PracticeToolLoopRegion[] {
  if (!fingerprint) {
    return [];
  }
  const stored = uiState.appSettings?.[LOOPS_SETTING_KEY];
  if (!stored || typeof stored !== "object" || Array.isArray(stored)) {
    return [];
  }
  const entry = (stored as unknown as Record<string, unknown>)[fingerprint];
  if (!Array.isArray(entry)) {
    return [];
  }
  return entry.filter(isPersistedLoop).map((loop) => ({ ...loop }));
}

/** Autosaves the loop list against the loaded file's fingerprint, so loops
 * reappear when the same track is reopened without any project involved. */
export function persistLoopsForCurrentFile(): void {
  const player = uiState.practiceTool;
  if (!player) {
    return;
  }
  const fingerprint = getFileFingerprint(player.filePath, player.durationSec);
  if (!fingerprint) {
    return;
  }
  const stored = uiState.appSettings?.[LOOPS_SETTING_KEY];
  const map: Record<string, PracticeToolLoopRegion[]> = (stored && typeof stored === "object" && !Array.isArray(stored))
    ? { ...(stored as unknown as Record<string, PracticeToolLoopRegion[]>) }
    : {};
  if (player.loops.length > 0) {
    map[fingerprint] = player.loops.map((loop) => ({ ...loop }));
  } else {
    delete map[fingerprint];
  }
  uiState.appSettings[LOOPS_SETTING_KEY] = map as unknown as AppSettingValue;
  setAppSetting(LOOPS_SETTING_KEY, map);
}

// ── Projects ─────────────────────────────────────────────────────────────────

function generateProjectId(): string {
  return `ptproj-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}

function readNumber(value: unknown, fallback: number): number {
  return typeof value === "number" && isFinite(value) ? value : fallback;
}

/**
 * Narrows one stored entry back to a project, filling in anything a
 * previously-written (or hand-edited) settings blob is missing. Returns null
 * only when the entry is too broken to name a track at all.
 */
export function parsePracticeToolProject(value: unknown): PracticeToolProject | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return null;
  }
  const record = value as Record<string, unknown>;
  const id = typeof record.id === "string" && record.id ? record.id : "";
  const name = typeof record.name === "string" ? record.name.trim() : "";
  const filePath = typeof record.filePath === "string" ? record.filePath : "";
  if (!id || !name || !filePath) {
    return null;
  }
  const loops = Array.isArray(record.loops) ? record.loops.filter(isPersistedLoop).map((loop) => ({ ...loop })) : [];
  const activeLoopId = typeof record.activeLoopId === "string" && loops.some((loop) => loop.id === record.activeLoopId)
    ? record.activeLoopId
    : null;
  const presetId = typeof record.presetId === "string" && record.presetId ? record.presetId : undefined;
  const presetName = typeof record.presetName === "string" && record.presetName ? record.presetName : undefined;
  return {
    id,
    name,
    filePath,
    fileTitle: typeof record.fileTitle === "string" && record.fileTitle
      ? record.fileTitle
      : (filePath.split(/[\\/]/).pop() ?? filePath),
    durationSec: Math.max(0, readNumber(record.durationSec, 0)),
    loops,
    activeLoopId,
    gain: readNumber(record.gain, 1),
    balance: readNumber(record.balance, 0),
    speed: readNumber(record.speed, 1),
    pitchSemitones: readNumber(record.pitchSemitones, 0),
    // Left undefined for a project saved before the EQ existed, so a recall of
    // one can tell "no EQ was captured" apart from "a deliberately flat EQ".
    ...(record.eq ? { eq: sanitizePracticeToolEq(record.eq) } : {}),
    ...(presetId ? { presetId } : {}),
    ...(presetId && presetName ? { presetName } : {}),
    savedAt: Math.max(0, readNumber(record.savedAt, 0)),
  };
}

/** Most recently saved first — the list is short and read on every render, so
 * it is sorted here rather than kept ordered in storage. */
export function readPracticeToolProjects(): PracticeToolProject[] {
  const stored = uiState.appSettings?.[PROJECTS_SETTING_KEY];
  if (!Array.isArray(stored)) {
    return [];
  }
  return stored
    .map(parsePracticeToolProject)
    .filter((project): project is PracticeToolProject => project !== null)
    .sort((a, b) => b.savedAt - a.savedAt);
}

function writePracticeToolProjects(projects: PracticeToolProject[]): void {
  const payload = projects as unknown as AppSettingValue;
  uiState.appSettings[PROJECTS_SETTING_KEY] = payload;
  setAppSetting(PROJECTS_SETTING_KEY, projects);
}

export function findPracticeToolProject(projectId: string): PracticeToolProject | null {
  return readPracticeToolProjects().find((project) => project.id === projectId) ?? null;
}

/**
 * Builds a project from the live player state. `projectId` overwrites an
 * existing project in place (keeping its id, so nothing else has to be
 * re-pointed); omit it for a new one. Returns null when there is no track
 * loaded to save.
 */
export function capturePracticeToolProject(
  name: string,
  options: { includePreset: boolean; projectId?: string } = { includePreset: false }
): PracticeToolProject | null {
  const player = uiState.practiceTool;
  const trimmedName = name.trim();
  if (!player || !player.filePath || !trimmedName) {
    return null;
  }
  const presetId = options.includePreset ? uiState.activePresetId : null;
  const presetName = presetId
    ? (uiState.presets.find((preset) => preset.id === presetId)?.name ?? "")
    : "";
  return {
    id: options.projectId ?? generateProjectId(),
    name: trimmedName,
    filePath: player.filePath,
    fileTitle: player.title || (player.filePath.split(/[\\/]/).pop() ?? player.filePath),
    durationSec: player.durationSec,
    loops: player.loops.map((loop) => ({ ...loop })),
    activeLoopId: player.activeLoopId,
    gain: player.gain,
    balance: player.balance,
    speed: player.speed,
    pitchSemitones: player.pitchSemitones,
    eq: sanitizePracticeToolEq(player.eq),
    ...(presetId ? { presetId } : {}),
    ...(presetId && presetName ? { presetName } : {}),
    savedAt: Date.now(),
  };
}

/** Inserts or replaces (by id) and persists. Returns false only when the cap
 * would be exceeded by a genuinely new project. */
export function storePracticeToolProject(project: PracticeToolProject): boolean {
  const projects = readPracticeToolProjects();
  const existingIndex = projects.findIndex((entry) => entry.id === project.id);
  if (existingIndex >= 0) {
    projects[existingIndex] = project;
  } else {
    if (projects.length >= MAX_PRACTICE_TOOL_PROJECTS) {
      return false;
    }
    projects.push(project);
  }
  writePracticeToolProjects(projects);
  return true;
}

export function deletePracticeToolProject(projectId: string): boolean {
  const projects = readPracticeToolProjects();
  const remaining = projects.filter((project) => project.id !== projectId);
  if (remaining.length === projects.length) {
    return false;
  }
  writePracticeToolProjects(remaining);
  return true;
}

/** Case-insensitive name match, used to offer "overwrite?" instead of silently
 * creating a second project with the same name. */
export function findPracticeToolProjectByName(name: string): PracticeToolProject | null {
  const needle = name.trim().toLowerCase();
  if (!needle) {
    return null;
  }
  return readPracticeToolProjects().find((project) => project.name.trim().toLowerCase() === needle) ?? null;
}

// ── Recall ───────────────────────────────────────────────────────────────────

/**
 * How a project's track can be brought back:
 * - `loaded`  — it is already the loaded track, so apply the settings directly.
 * - `reload`  — a real path we can hand to `loadPracticeToolFile`.
 * - `unavailable` — the track was dropped in, so all we ever recorded was its
 *   file name; the user has to re-open it before the project can be recalled.
 */
export type ProjectTrackAvailability = "loaded" | "reload" | "unavailable";

/** Windows drive letter, UNC share, or POSIX absolute — anything the native
 * side can actually re-open by path. */
function isReloadablePath(filePath: string): boolean {
  const trimmed = filePath.trim();
  return /^[a-zA-Z]:[\\/]/.test(trimmed) || trimmed.startsWith("\\\\") || trimmed.startsWith("/");
}

/** Paths only, deliberately: the loop store keys on path + duration, but a
 * re-decode at a different host sample rate can round the reported duration by
 * a hundredth of a second, and "same track" must not hinge on that. */
function isSameTrackPath(a: string, b: string): boolean {
  return Boolean(a) && a.trim().toLowerCase() === b.trim().toLowerCase();
}

export function getProjectTrackAvailability(project: PracticeToolProject): ProjectTrackAvailability {
  const player = uiState.practiceTool;
  if (player && isSameTrackPath(player.filePath, project.filePath)) {
    return "loaded";
  }
  return isReloadablePath(project.filePath) ? "reload" : "unavailable";
}

// A project whose track has to be re-decoded first is applied in two beats:
// the load is requested here, and `applyPracticeToolFileLoaded` cashes this in
// once the engine answers. Only one can be outstanding — a second recall
// before the first lands supersedes it, which is what the user just asked for.
let pendingRecall: PracticeToolProject | null = null;

export function setPendingProjectRecall(project: PracticeToolProject | null): void {
  pendingRecall = project;
}

/**
 * Hands back the pending project if it is the one this file load is for.
 * A file the user opened by other means (Browse, drag-and-drop) while a recall
 * was outstanding clears it rather than being silently reconfigured.
 */
export function consumePendingProjectRecall(filePath: string): PracticeToolProject | null {
  const project = pendingRecall;
  pendingRecall = null;
  return project && isSameTrackPath(project.filePath, filePath) ? project : null;
}
