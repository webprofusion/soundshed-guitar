/**
 * What the scene badges and the add menu's "Add scene" do. Scene edits are engine commands:
 * the engine changes its working copy and answers with "presetLoaded", which re-renders the
 * bar, and reports the unsaved change itself.
 */

import { addScene, moveScene, removeScene, renameScene, selectScene } from "../bridge.js";
import { setActivePresetSceneId } from "../presetLibraryStore.js";
import { selectPresetScene } from "../presetScenes.js";
import { getSignalPathPreset, uiState } from "../state.js";
import type { Preset } from "../types.js";
import { getEditableSignalPathPreset } from "./mixer.js";
import { requestSignalPathRender } from "./render.js";
import type { SceneBadgeActions } from "./sceneBadges.js";

export function addSceneFromToolbar(): void {
  const activePreset = getSignalPathPreset();
  if (!activePreset) {
    return;
  }

  // Makes sure the engine is editing the preset on screen (a focused mixer slot).
  getEditableSignalPathPreset(activePreset);
  addScene(uiState.activePresetSceneId);
}

export function sceneBadgeActions(renderedPreset: Preset): SceneBadgeActions {
  return {
    select(sceneId) {
      if (!sceneId || sceneId === uiState.activePresetSceneId) {
        return;
      }
      // Draw the switch now; the engine's reply confirms it once the scene is built. In a
      // multi-preset mix the engine rebuilds only this preset's slot.
      const editablePreset = getEditableSignalPathPreset(renderedPreset);
      setActivePresetSceneId(selectPresetScene(editablePreset, sceneId));
      selectScene(sceneId);
      requestSignalPathRender();
    },
    rename(sceneId, title) {
      getEditableSignalPathPreset(renderedPreset);
      renameScene(sceneId, title);
    },
    remove(sceneId) {
      // Makes sure the engine is editing the preset on screen (a focused mixer slot).
      getEditableSignalPathPreset(renderedPreset);
      removeScene(sceneId);
    },
    move(sceneId, toIndex) {
      getEditableSignalPathPreset(renderedPreset);
      moveScene(sceneId, toIndex);
    },
    add: addSceneFromToolbar,
  };
}
