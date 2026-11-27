/**
 * The theme flyout's "App UI" choice: this UI, or the native UI from Soundshed Nano Q (beta).
 *
 * Only where the host can build either editor (environment.nativeEditor: the desktop app and
 * plugins on Windows, macOS and Linux, where SOUNDSHED_NANOQ_UI is on). Never on iOS, iPadOS or
 * Android: those ship Soundshed Nano Q's own apps (nanoq/ios, nanoq/android). The setting is the
 * engine's (guitarfx::kNativeEditorSettingKey): the standalone app swaps its editor as soon as it
 * changes, and a plugin's next window follows it. The native UI's theme flyout has the same
 * choice, the way back, and says it is a beta the first time it opens.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { uiState } from "../state.js";
import { getSettingValue } from "./values.js";

export const NATIVE_EDITOR_SETTING = "ui.nativeEditor";

const STANDALONE_HINT = "Native (Nano) draws the app with the native UI from Soundshed Nano Q: it starts faster and uses less memory, but it is a beta and not everything is there yet.";
const PLUGIN_HINT = "Native (Nano) draws the plugin window with the native UI from Soundshed Nano Q: it starts faster and uses less memory, but it is a beta and not everything is there yet. A change takes effect when the plugin window is next opened, for every instance.";

const sectionEl = (): HTMLElement | null => document.getElementById("app-ui");

function choices(): HTMLButtonElement[] {
  const section = sectionEl();
  return section ? Array.from(section.querySelectorAll<HTMLButtonElement>(".app-ui-choice")) : [];
}

export function initNativeEditorToggle(): void {
  const section = sectionEl();
  if (!section || section.dataset.bound === "true") return;
  section.dataset.bound = "true";
  section.addEventListener("click", (event) => {
    const choice = (event.target as Element | null)?.closest<HTMLButtonElement>(".app-ui-choice");
    if (!choice) return;
    const native = choice.dataset.nativeEditor === "true";
    if (native !== (getSettingValue(NATIVE_EDITOR_SETTING) === true)) {
      updateAppSetting(NATIVE_EDITOR_SETTING, native);
    }
    // Marked at once; the stored setting coming back marks it again.
    markChoice(native);
  });
  section.addEventListener("keydown", (event) => {
    if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") return;
    const all = choices();
    const current = all.indexOf(document.activeElement as HTMLButtonElement);
    if (current < 0) return;
    event.preventDefault();
    all[(current + (event.key === "ArrowRight" ? 1 : all.length - 1)) % all.length].focus();
  });
  refreshNativeEditorToggle();
}

function markChoice(native: boolean): void {
  for (const choice of choices()) {
    const selected = (choice.dataset.nativeEditor === "true") === native;
    choice.setAttribute("aria-checked", String(selected));
    choice.tabIndex = selected ? 0 : -1;
  }
}

/** Shows the choice where the host supports it; settings and environment arrive after the flyout binds. */
export function refreshNativeEditorToggle(): void {
  const supported = Boolean(uiState.environment?.nativeEditor);
  sectionEl()?.toggleAttribute("hidden", !supported);

  const hint = document.getElementById("app-ui-hint");
  if (hint) {
    hint.textContent = uiState.environment?.standalone ? STANDALONE_HINT : PLUGIN_HINT;
  }

  markChoice(getSettingValue(NATIVE_EDITOR_SETTING) === true);
}
