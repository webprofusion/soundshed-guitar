/**
 * Right-click on a folder in the preset chooser's folder tree, or a long press on
 * a touch screen (touchLongPress.ts). It offers "Create Pack…", which starts a new
 * Tone Sharing draft pack holding the folder's presets (its subfolders' included).
 * The same on a preset card offers the folders to move it to.
 *
 * The menu hangs from <body> so the popover's clipping can't trap it, which means
 * its position is divided by the app's CSS zoom.
 */

import { switchMainPanel } from "../navigation.js";
import { showNotification } from "../notifications.js";
import { getUiZoom } from "../pointerDrag.js";
import { clonePreset, uiState } from "../state.js";
import { initializeTouchLongPress } from "../touchLongPress.js";
import { ensureToneSharingSignedIn, openFolderPackModal, openToneSharingSignInModal } from "../toneSharingPanel.js";
import type { Preset, PresetFolder } from "../types.js";
import { escapeHtml } from "../utils.js";
import { presetFolderTreeElement, presetListElement } from "./dom.js";
import { getPresetsForFolderId } from "./folderArchive.js";
import { movePresetToFolder } from "./folderControls.js";
import { findFolderById, findFolderForPreset, isVirtualPresetFolderId, sortPresetFoldersAlphabetically } from "./folders.js";
import { requestPresetFromBackend } from "./fetch.js";
import { closePresetLibraryPopover } from "./popover.js";
import { PRESET_FOLDER_ALL_ID } from "./sorting.js";

/** How far inside the window the menu's edges are kept. */
const VIEWPORT_MARGIN_PX = 4;

let menuElement: HTMLElement | null = null;
let listenersBound = false;

function bindDismissListeners(): void {
  if (listenersBound) {
    return;
  }
  listenersBound = true;
  // Capture phase, so a control that stops the event still closes the menu.
  document.addEventListener("pointerdown", (event) => {
    if (menuElement && !menuElement.contains(event.target as Node | null)) {
      closePresetFolderMenu();
    }
  }, true);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      closePresetFolderMenu();
    }
  });
  document.addEventListener("scroll", closePresetFolderMenu, true);
  window.addEventListener("resize", closePresetFolderMenu);
  window.addEventListener("blur", closePresetFolderMenu);
}

/**
 * Listens on the folder tree itself, which outlives the rows it re-renders, so
 * the rows need no listener of their own.
 */
export function initializePresetFolderMenu(): void {
  initializeTouchLongPress();
  presetFolderTreeElement?.addEventListener("contextmenu", (event) => {
    const row = event.target instanceof Element ? event.target.closest<HTMLElement>(".preset-folder-item[data-folder-id]") : null;
    const folderId = row?.dataset.folderId ?? "";
    if (!folderId || isVirtualPresetFolderId(folderId)) {
      return;
    }
    event.preventDefault();
    openPresetFolderMenu(folderId, event.clientX, event.clientY);
  });
  presetListElement?.addEventListener("contextmenu", (event) => {
    const card = event.target instanceof Element ? event.target.closest<HTMLElement>("article.preset-item[data-id]") : null;
    const presetId = card?.dataset.id ?? "";
    if (!presetId) {
      return;
    }
    event.preventDefault();
    openPresetMoveMenu(presetId, event.clientX, event.clientY);
  });
}

export function closePresetFolderMenu(): void {
  menuElement?.remove();
  menuElement = null;
}

/** Opens the folder's menu at the pointer. Virtual folders (All, Favourites, Recents) have none. */
export function openPresetFolderMenu(folderId: string, clientX: number, clientY: number): void {
  closePresetFolderMenu();
  if (isVirtualPresetFolderId(folderId)) {
    return;
  }
  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  if (!folder) {
    return;
  }
  bindDismissListeners();

  const presetCount = getPresetsForFolderId(folderId).length;
  const menu = document.createElement("div");
  menu.className = "preset-folder-menu";
  menu.setAttribute("role", "menu");
  menu.setAttribute("aria-label", `${folder.name} folder`);
  // The compact Controls sheet closes on clicks outside the bar unless they land in one of its popovers.
  menu.setAttribute("data-control-bar-popover", "");
  menu.innerHTML = `
    <div class="preset-folder-menu-title">${escapeHtml(folder.name)}</div>
    <button class="preset-folder-menu-item" type="button" role="menuitem" data-action="createPack"${presetCount === 0 ? " disabled" : ""}>
      <span>Create Pack…</span>
      <span class="preset-folder-menu-hint">${presetCount === 1 ? "1 preset" : `${presetCount} presets`}</span>
    </button>
  `;

  menu.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest<HTMLButtonElement>(".preset-folder-menu-item") : null;
    if (!button || button.disabled) {
      return;
    }
    closePresetFolderMenu();
    if (button.dataset.action === "createPack") {
      void createPackFromFolder(folderId);
    }
  });
  showMenuAt(menu, clientX, clientY);
}

/** Measured in place, then pulled back inside the window if it would overhang. */
function showMenuAt(menu: HTMLElement, clientX: number, clientY: number): void {
  menu.addEventListener("contextmenu", (event) => event.preventDefault());
  document.body.appendChild(menu);
  const zoom = getUiZoom();
  const { width, height } = menu.getBoundingClientRect();
  const viewWidth = window.innerWidth / zoom;
  const viewHeight = window.innerHeight / zoom;
  const x = clientX / zoom;
  const y = clientY / zoom;
  menu.style.left = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(x, viewWidth - width / zoom - VIEWPORT_MARGIN_PX))}px`;
  menu.style.top = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(y, viewHeight - height / zoom - VIEWPORT_MARGIN_PX))}px`;
  menuElement = menu;
  menu.querySelector<HTMLButtonElement>(".preset-folder-menu-item:not(:disabled)")?.focus({ preventScroll: true });
}

/**
 * Right-click or long press on a preset card: "Move to folder", one row per
 * folder. Dragging a card onto a folder is the other way, but on a short screen
 * the folder tree is folded into a drop-down while the list shows, and an iPhone
 * user may never find a drag; this needs neither.
 */
export function openPresetMoveMenu(presetId: string, clientX: number, clientY: number): void {
  closePresetFolderMenu();
  const preset = uiState.presets.find((candidate) => candidate.id === presetId);
  if (!preset) {
    return;
  }
  bindDismissListeners();

  const current = findFolderForPreset(uiState.presetFolders ?? [], presetId)?.id ?? PRESET_FOLDER_ALL_ID;
  const rows: string[] = [];
  const addRows = (folders: PresetFolder[], depth: number): void => {
    sortPresetFoldersAlphabetically(folders).forEach((folder) => {
      rows.push(moveRow(folder.id, folder.name, depth, folder.id === current));
      addRows(folder.children ?? [], depth + 1);
    });
  };
  addRows(uiState.presetFolders ?? [], 0);

  const menu = document.createElement("div");
  menu.className = "preset-folder-menu preset-move-menu";
  menu.setAttribute("role", "menu");
  menu.setAttribute("aria-label", `Move ${preset.name} to folder`);
  menu.setAttribute("data-control-bar-popover", "");
  menu.innerHTML = `
    <div class="preset-folder-menu-title">Move “${escapeHtml(preset.name)}” to</div>
    ${moveRow(PRESET_FOLDER_ALL_ID, "No folder", 0, current === PRESET_FOLDER_ALL_ID)}
    ${rows.length ? rows.join("") : '<div class="preset-folder-menu-hint preset-move-menu-empty">No folders yet: make one with the folder tools.</div>'}
  `;
  menu.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest<HTMLButtonElement>(".preset-folder-menu-item") : null;
    if (!button || button.disabled) {
      return;
    }
    closePresetFolderMenu();
    movePresetToFolder(presetId, button.dataset.folderId ?? PRESET_FOLDER_ALL_ID);
  });
  showMenuAt(menu, clientX, clientY);
}

function moveRow(folderId: string, name: string, depth: number, isCurrent: boolean): string {
  return `
    <button class="preset-folder-menu-item" type="button" role="menuitem" data-folder-id="${escapeHtml(folderId)}"${isCurrent ? " disabled" : ""}>
      <span style="padding-left: ${depth * 12}px">${escapeHtml(name)}</span>
      ${isCurrent ? '<span class="preset-folder-menu-hint">Here now</span>' : ""}
    </button>
  `;
}

/** The preset with its full graph: the cached copy when it has one, else the engine's. */
async function loadFullPreset(presetId: string): Promise<Preset> {
  const cached = uiState.presetCache.get(presetId);
  if (cached?.graph?.nodes?.length) {
    return clonePreset(cached);
  }
  return requestPresetFromBackend(presetId);
}

async function createPackFromFolder(folderId: string): Promise<void> {
  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  if (!folder) {
    return;
  }
  const presets = getPresetsForFolderId(folderId);
  closePresetLibraryPopover();

  let signedIn = false;
  try {
    signedIn = await ensureToneSharingSignedIn();
  } catch {
    signedIn = false;
  }
  if (!signedIn) {
    showNotification("Sign in to create a pack", "Packs are shared through Tone Sharing.");
    switchMainPanel("sharing");
    openToneSharingSignInModal();
    return;
  }

  await openFolderPackModal(folder.name, presets, loadFullPreset);
}
