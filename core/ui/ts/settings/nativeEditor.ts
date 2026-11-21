/**
 * The experimental "Native UI (Nano)" switch in Settings → User Interface → Interface.
 *
 * Only where the host can build either editor (environment.nativeEditor: the Windows x64 app and
 * plugins). The setting is the engine's (guitarfx::kNativeEditorSettingKey): the standalone app
 * swaps its editor as soon as it changes, and a plugin's next window follows it. The native UI's
 * own Settings page has the switch back.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { uiState } from "../state.js";
import { getSettingValue } from "./values.js";

export const NATIVE_EDITOR_SETTING = "ui.nativeEditor";

const STANDALONE_HINT = "Draws the app with the native UI from Soundshed Nano Q instead of this one. It starts faster and uses less memory, but not everything is there yet. Switch back from its Settings page.";
const PLUGIN_HINT = "Draws the plugin window with the native UI from Soundshed Nano Q instead of this one. It starts faster and uses less memory, but not everything is there yet. Takes effect when the plugin window is next opened, for every instance. Switch back from its Settings page.";

function nativeEditorToggle(): HTMLInputElement | null {
  return document.getElementById("native-editor-toggle") as HTMLInputElement | null;
}

export function initNativeEditorToggle(): void {
  const toggle = nativeEditorToggle();
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.addEventListener("change", () => {
    updateAppSetting(NATIVE_EDITOR_SETTING, Boolean(toggle.checked));
  });
  refreshNativeEditorToggle();
}

/** Shows the switch where the host supports it; settings and environment arrive after the panel binds. */
export function refreshNativeEditorToggle(): void {
  const supported = Boolean(uiState.environment?.nativeEditor);
  document.getElementById("native-editor-settings-section")?.toggleAttribute("hidden", !supported);

  const hint = document.getElementById("native-editor-hint");
  if (hint) {
    hint.textContent = uiState.environment?.standalone ? STANDALONE_HINT : PLUGIN_HINT;
  }

  const toggle = nativeEditorToggle();
  if (toggle) {
    toggle.checked = getSettingValue(NATIVE_EDITOR_SETTING) === true;
  }
}
