import { uiState } from "../state.js";
import { generateResourceId } from "../archiveUtils.js";
import type { Preset, PresetFolder } from "../types.js";
import type { PresetArchiveFolder } from "./archiveTypes.js";
import { findFolderByNameInList, persistPresetFolders, removePresetFromFolders } from "./folders.js";
import { PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID, sortPresetsAlphabetically } from "./sorting.js";
import { findFolderById } from "./folders.js";
import { collectPresetIds } from "./folders.js";
import { getRecentPresets, loadFavoritePresetIds } from "./favorites.js";
import { isVirtualPresetFolderId } from "./folders.js";

export const PRESET_FOLDER_ALL_ID = "__all__";

export function buildArchivePresetFolder(folder: PresetFolder, allowedPresetIds: Set<string>): PresetArchiveFolder | null {
  const presetIds = (folder.presetIds ?? []).filter((presetId) => allowedPresetIds.has(presetId));
  const children = (folder.children ?? [])
    .map((child) => buildArchivePresetFolder(child, allowedPresetIds))
    .filter((child): child is PresetArchiveFolder => Boolean(child));

  if (presetIds.length === 0 && children.length === 0) {
    return null;
  }

  return {
    name: folder.name,
    ...(presetIds.length > 0 ? { presetIds } : {}),
    ...(children.length > 0 ? { children } : {}),
  };
}

export function buildArchivePresetFoldersForExport(folderId: string, presets: Preset[]): PresetArchiveFolder[] {
  if (isVirtualPresetFolderId(folderId) && folderId !== PRESET_FOLDER_ALL_ID) {
    return [];
  }

  const allowedPresetIds = new Set(presets.map((preset) => preset.id));
  if (allowedPresetIds.size === 0) {
    return [];
  }

  if (folderId !== PRESET_FOLDER_ALL_ID) {
    const folder = findFolderById(uiState.presetFolders ?? [], folderId);
    const serializedFolder = folder ? buildArchivePresetFolder(folder, allowedPresetIds) : null;
    return serializedFolder ? [serializedFolder] : [];
  }

  return (uiState.presetFolders ?? [])
    .map((folder) => buildArchivePresetFolder(folder, allowedPresetIds))
    .filter((folder): folder is PresetArchiveFolder => Boolean(folder));
}

export function getPresetsForFolderId(folderId: string): Preset[] {
  let presets = uiState.presets.slice();
  if (folderId === PRESET_FOLDER_FAVORITES_ID) {
    const favorites = loadFavoritePresetIds();
    presets = presets.filter((preset) => favorites.has(preset.id));
    return sortPresetsAlphabetically(presets);
  }
  if (folderId === PRESET_FOLDER_RECENTS_ID) {
    return getRecentPresets();
  }
  if (folderId !== PRESET_FOLDER_ALL_ID) {
    const folder = findFolderById(uiState.presetFolders ?? [], folderId);
    if (!folder) {
      return [];
    }
    const allowedIds = collectPresetIds(folder);
    presets = presets.filter((preset) => allowedIds.has(preset.id));
  }
  return sortPresetsAlphabetically(presets);
}

export function getPresetFolderExportName(folderId: string): string {
  if (folderId === PRESET_FOLDER_ALL_ID) {
    return "All-Presets";
  }
  if (folderId === PRESET_FOLDER_FAVORITES_ID) {
    return "Favorite-Presets";
  }
  if (folderId === PRESET_FOLDER_RECENTS_ID) {
    return "Recent-Presets";
  }
  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  return folder?.name || "Preset-Folder";
}

export function applyImportedPresetFolders(
  archiveFolders: PresetArchiveFolder[],
  presetIdMap: Map<string, string>,
  importedPresetIds: string[],
  topLevelFolderName?: string,
): void {
  const folders = uiState.presetFolders ?? [];
  const assignedPresetIds = new Set<string>();
  const ensureChildFolder = (siblings: PresetFolder[], name: string): PresetFolder => {
    const existing = findFolderByNameInList(siblings, name);
    if (existing) {
      existing.children = existing.children ?? [];
      existing.presetIds = existing.presetIds ?? [];
      return existing;
    }

    const created: PresetFolder = {
      id: generateResourceId(name),
      name,
      children: [],
      presetIds: [],
    };
    siblings.push(created);
    return created;
  };

  const applyFolderNodes = (siblings: PresetFolder[], nodes: PresetArchiveFolder[]): void => {
    nodes.forEach((node) => {
      const folder = ensureChildFolder(siblings, node.name);
      (node.presetIds ?? []).forEach((sourcePresetId) => {
        const importedPresetId = presetIdMap.get(sourcePresetId);
        if (!importedPresetId) {
          return;
        }
        if (!folder.presetIds.includes(importedPresetId)) {
          folder.presetIds.push(importedPresetId);
        }
        assignedPresetIds.add(importedPresetId);
      });
      applyFolderNodes(folder.children, node.children ?? []);
    });
  };

  let targetFolders = folders;
  if (topLevelFolderName?.trim()) {
    const topLevelFolder = ensureChildFolder(folders, topLevelFolderName.trim());
    targetFolders = topLevelFolder.children ?? [];
    topLevelFolder.children = targetFolders;
  }

  applyFolderNodes(targetFolders, archiveFolders);

  if (assignedPresetIds.size > 0 || importedPresetIds.length > 0) {
    persistPresetFolders();
  }
}

export function assignImportedPresetsToTopLevelFolder(folderName: string, importedPresetIds: string[]): void {
  const resolvedName = folderName.trim();
  const targetName = resolvedName || "Imported Pack";
  if (importedPresetIds.length === 0) {
    return;
  }

  uiState.presetFolders = uiState.presetFolders ?? [];
  const rootFolders = uiState.presetFolders;
  const existing = findFolderByNameInList(rootFolders, targetName);
  const targetFolder = existing ?? {
    id: generateResourceId(targetName),
    name: targetName,
    children: [],
    presetIds: [],
  };

  if (!existing) {
    rootFolders.push(targetFolder);
  }

  targetFolder.children = targetFolder.children ?? [];
  targetFolder.presetIds = targetFolder.presetIds ?? [];

  importedPresetIds.forEach((presetId) => {
    removePresetFromFolders(rootFolders, presetId);
    if (!targetFolder.presetIds.includes(presetId)) {
      targetFolder.presetIds.push(presetId);
    }
  });

  persistPresetFolders();
}
