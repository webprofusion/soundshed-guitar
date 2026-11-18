/** Persisted panel and tab selection, owned by navigation rather than uiState. */

import type { UiViewState } from "./types.js";

let viewState: UiViewState = {
  mainPanel: "visualizer",
  playView: "visualizer",
  presetTab: "details",
  tonesTab: "community",
  settings: {
    equipmentTab: "settings",
    libraryTab: "resources",
    advancedTab: "composites",
  },
};

export function getNavigationViewState(): UiViewState {
  return viewState;
}

/** Host restore replaces the saved view, including fields it intentionally omits. */
export function replaceNavigationViewState(state: UiViewState): void {
  viewState = state;
}

/** Merge a local panel or tab change while preserving the other settings tabs. */
export function mergeNavigationViewState(update: UiViewState): boolean {
  const next: UiViewState = {
    ...viewState,
    ...update,
    settings: {
      ...viewState.settings,
      ...update.settings,
    },
  };
  if (JSON.stringify(viewState) === JSON.stringify(next)) {
    return false;
  }
  viewState = next;
  return true;
}
