/**
 * The folder picker: on a short screen (`data-compact-short`) the folder tree
 * folds into one button at the start of the tag row, showing the current
 * folder's name and count. It opens the tree, with the folder tools at its foot,
 * as a drop-down over the preset list; picking a folder closes it.
 *
 * The tree and the tools are the same elements as on a taller screen; CSS
 * (css/compact/overlays.css) places them, and this only keeps the open state and
 * the button's label. Elsewhere the button is not shown and the tree stays open.
 */

import { presetFolderPickerButton, presetFolderTreeElement, presetLibraryPresetsPanel } from "./dom.js";

const OPEN_CLASS = "folders-open";

let initialized = false;

export function isPresetFolderPickerOpen(): boolean {
  return presetLibraryPresetsPanel?.classList.contains(OPEN_CLASS) ?? false;
}

export function setPresetFolderPickerOpen(open: boolean): void {
  presetLibraryPresetsPanel?.classList.toggle(OPEN_CLASS, open);
  presetFolderPickerButton?.setAttribute("aria-expanded", String(open));
}

/** Copies the active folder's name and count off the rendered tree onto the button. */
export function syncPresetFolderPickerLabel(): void {
  if (!presetFolderPickerButton || !presetFolderTreeElement) {
    return;
  }
  const active = presetFolderTreeElement.querySelector<HTMLElement>(".preset-folder-item.active");
  const name = active?.querySelector(".folder-name")?.textContent?.trim() || "All Presets";
  const count = active?.querySelector(".folder-count")?.textContent?.trim() ?? "";
  const nameElement = presetFolderPickerButton.querySelector(".preset-folder-picker-name");
  const countElement = presetFolderPickerButton.querySelector(".preset-folder-picker-count");
  if (nameElement) {
    nameElement.textContent = name;
  }
  if (countElement) {
    countElement.textContent = count;
  }
  presetFolderPickerButton.setAttribute("aria-label", count ? `Folder: ${name}, ${count} presets` : `Folder: ${name}`);
}

export function initPresetFolderPicker(): void {
  if (initialized || !presetFolderPickerButton) {
    return;
  }
  initialized = true;
  presetFolderPickerButton.addEventListener("click", () => setPresetFolderPickerOpen(!isPresetFolderPickerOpen()));

  // A folder picked from the drop-down closes it. Each row has its own click
  // handler (views.ts), which runs first; this sees the click as it bubbles.
  presetFolderTreeElement?.addEventListener("click", (event) => {
    if (event.target instanceof Element && event.target.closest(".preset-folder-item")) {
      setPresetFolderPickerOpen(false);
    }
  });

  // A press anywhere else in the library (the list it covers, the tags, the
  // search) closes it too. The folder tools are part of the drop-down.
  document.addEventListener("pointerdown", (event) => {
    if (!isPresetFolderPickerOpen() || !(event.target instanceof Element)) {
      return;
    }
    if (event.target.closest("#preset-folder-picker, #preset-folder-tree, #preset-folder-toolbar, .preset-folder-menu, .modal")) {
      return;
    }
    setPresetFolderPickerOpen(false);
  }, true);
}
