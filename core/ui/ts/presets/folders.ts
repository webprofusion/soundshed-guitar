import { uiState } from "../state.js";
import type { PresetFolder } from "../types.js";
import { postMessage } from "../bridge.js";
import { PRESET_FOLDER_ALL_ID } from "./folderArchive.js";
import { PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } from "./sorting.js";
import { presetNameCollator } from "./sorting.js";

export function normalizeFolderName(name: string): string {
  return name.trim().toLowerCase();
}

export function compareFolderNames(left: PresetFolder, right: PresetFolder): number {
  const nameComparison = presetNameCollator.compare(left.name.trim(), right.name.trim());
  if (nameComparison !== 0) {
    return nameComparison;
  }
  return presetNameCollator.compare(left.id, right.id);
}

export function sortPresetFoldersAlphabetically(folders: PresetFolder[]): PresetFolder[] {
  return folders
    .map((folder) => ({
      ...folder,
      children: sortPresetFoldersAlphabetically(folder.children ?? []),
      presetIds: [...(folder.presetIds ?? [])],
    }))
    .sort(compareFolderNames);
}

export function isVirtualPresetFolderId(folderId: string | null | undefined): boolean {
  return folderId === PRESET_FOLDER_ALL_ID
    || folderId === PRESET_FOLDER_FAVORITES_ID
    || folderId === PRESET_FOLDER_RECENTS_ID;
}

export function loadPresetFoldersFromState(): PresetFolder[] {
  return uiState.presetFolders ? sortPresetFoldersAlphabetically(uiState.presetFolders) : [];
}

export function savePresetFoldersToBackend(folders: PresetFolder[], activeFolderId?: string | null): void {
  postMessage({
    type: "setPresetFolders",
    folders,
    activeFolderId: activeFolderId ?? uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID,
  });
}

export function findFolderById(folders: PresetFolder[], folderId: string): PresetFolder | undefined {
  for (const folder of folders) {
    if (folder.id === folderId) {
      return folder;
    }
    const childMatch = findFolderById(folder.children ?? [], folderId);
    if (childMatch) {
      return childMatch;
    }
  }
  return undefined;
}

export function findFolderWithParent(
  folders: PresetFolder[],
  folderId: string,
  parent: PresetFolder | null = null,
): { folder: PresetFolder; parent: PresetFolder | null } | null {
  for (const folder of folders) {
    if (folder.id === folderId) {
      return { folder, parent };
    }
    const childMatch = findFolderWithParent(folder.children ?? [], folderId, folder);
    if (childMatch) {
      return childMatch;
    }
  }
  return null;
}

export function isDescendantFolder(folder: PresetFolder, targetId: string): boolean {
  if (!folder.children?.length) {
    return false;
  }
  return folder.children.some((child) => child.id === targetId || isDescendantFolder(child, targetId));
}

export function findFolderForPreset(folders: PresetFolder[], presetId: string): PresetFolder | undefined {
  for (const folder of folders) {
    if ((folder.presetIds ?? []).includes(presetId)) {
      return folder;
    }
    const childMatch = findFolderForPreset(folder.children ?? [], presetId);
    if (childMatch) {
      return childMatch;
    }
  }
  return undefined;
}

export function findFolderPath(folders: PresetFolder[], targetId: string, trail: string[] = []): string[] | null {
  for (const folder of folders) {
    const nextTrail = [...trail, folder.name];
    if (folder.id === targetId) {
      return nextTrail;
    }
    const childTrail = findFolderPath(folder.children ?? [], targetId, nextTrail);
    if (childTrail) {
      return childTrail;
    }
  }
  return null;
}

export function getPresetFolderPath(presetId: string): string | null {
  const folders = uiState.presetFolders ?? [];
  const folder = findFolderForPreset(folders, presetId);
  if (!folder) {
    return null;
  }
  const path = findFolderPath(folders, folder.id);
  return path ? path.join(" > ") : folder.name;
}

export function ensurePresetFolders(persistChanges: boolean = true): void {
  const stored = loadPresetFoldersFromState();
  uiState.presetFolders = stored;
  const requestedActive = uiState.activePresetFolderId;

  const resolvedActive = requestedActive && !isVirtualPresetFolderId(requestedActive)
    ? findFolderById(stored, requestedActive)?.id
    : requestedActive;
  uiState.activePresetFolderId = resolvedActive || PRESET_FOLDER_ALL_ID;

  if (persistChanges && (requestedActive ?? PRESET_FOLDER_ALL_ID) !== uiState.activePresetFolderId) {
    savePresetFoldersToBackend(uiState.presetFolders ?? [], uiState.activePresetFolderId);
  }
}

export function persistPresetFolders(): void {
  savePresetFoldersToBackend(uiState.presetFolders ?? [], uiState.activePresetFolderId);
}

export function removePresetFromFolders(folders: PresetFolder[], presetId: string): void {
  folders.forEach((folder) => {
    folder.presetIds = (folder.presetIds ?? []).filter((id) => id !== presetId);
    if (folder.children?.length) {
      removePresetFromFolders(folder.children, presetId);
    }
  });
}

export function collectPresetIds(folder: PresetFolder): Set<string> {
  const ids = new Set<string>(folder.presetIds ?? []);
  (folder.children ?? []).forEach((child) => {
    collectPresetIds(child).forEach((id) => ids.add(id));
  });
  return ids;
}

export function findFolderByNameInList(folders: PresetFolder[], name: string): PresetFolder | undefined {
  const normalized = normalizeFolderName(name);
  return folders.find((folder) => normalizeFolderName(folder.name) === normalized);
}
