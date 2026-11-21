/**
 * The "Full Level on Mono Outputs" toggle in Settings → DSP Performance → Levels & Switching.
 *
 * A centred rig sits 3 dB under its input on each side of a stereo output, the equal-power pan
 * law. On a mono output those 3 dB buy nothing, and the toggle makes them up there. The
 * standalone takes the setting at once. A plugin instance takes it only when it is new and keeps
 * its own value in the project from then on, so the toggle sets the default for instances added
 * later, and the line under it says what this instance runs (the engine's `mixer.monoOutputUnity`).
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { uiState } from "../state.js";
import { DSP_MONO_OUTPUT_UNITY_SETTING } from "./keys.js";
import { getSettingValue } from "./values.js";

/** What this instance runs, from the engine's full state; null until it has said. */
let instanceValue: boolean | null = null;

/** Records `mixer.monoOutputUnity` from a full state; the settings refresh that follows shows it. */
export function applyMonoOutputUnityInstance(value: unknown): void {
  if (typeof value === "boolean") {
    instanceValue = value;
  }
}

function toggle(): HTMLInputElement | null {
  return document.getElementById("dsp-mono-output-unity-toggle") as HTMLInputElement | null;
}

function instanceNote(): HTMLElement | null {
  return document.getElementById("dsp-mono-output-unity-instance");
}

export function initMonoOutputUnityToggle(): void {
  const input = toggle();
  if (!input || input.dataset.bound === "true") return;
  input.dataset.bound = "true";
  input.checked = Boolean(getSettingValue(DSP_MONO_OUTPUT_UNITY_SETTING));
  input.addEventListener("change", () => {
    updateAppSetting(DSP_MONO_OUTPUT_UNITY_SETTING, Boolean(input.checked));
  });
}

/** Shows the stored setting, and in a plugin what this instance runs; both arrive after binding. */
export function refreshMonoOutputUnityToggle(): void {
  const setting = Boolean(getSettingValue(DSP_MONO_OUTPUT_UNITY_SETTING));
  const input = toggle();
  if (input) {
    input.checked = setting;
  }

  const note = instanceNote();
  if (!note) return;
  if (uiState.environment?.standalone || instanceValue === null) {
    note.hidden = true;
    note.textContent = "";
    return;
  }
  note.hidden = false;
  note.textContent =
    instanceValue === setting
      ? `This instance: ${instanceValue ? "on" : "off"}.`
      : `This instance keeps its own value (${instanceValue ? "on" : "off"}); the setting applies to instances added from now on.`;
}
