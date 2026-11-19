/**
 * The player's own picture behind the play page's effect panel (#panel-visualizer), drawn in place
 * of whatever the theme puts there (Stage's stage, High Gain's grille, the plain panel colour of
 * the others) under every theme.
 *
 * The engine keeps the picture (controller/UiBackgroundService.h): "browseUiBackgroundImage" opens
 * a file dialog and answers "uiBackgroundImageSelected" with the URL to draw it from, which is kept
 * in an app setting so every instance shows it, as the theme is. "clearUiBackgroundImage" removes
 * the engine's copy. A Fade setting veils the picture toward the theme's own background (black on
 * the dark themes, white on the light ones) so the text over it stays readable.
 *
 * The look itself is css/custom-background.css, keyed off the body class this sets.
 */

import { getAppSetting, updateAppSetting } from "./appSettingsStore.js";
import { postMessage } from "./bridge.js";

export const VISUALIZER_BACKGROUND_IMAGE_SETTING = "ui.visualizerBackground.image";
export const VISUALIZER_BACKGROUND_FADE_SETTING = "ui.visualizerBackground.fade";

/** Percent the picture is veiled by until the player sets it. */
export const VISUALIZER_BACKGROUND_FADE_DEFAULT = 30;
export const VISUALIZER_BACKGROUND_FADE_MAX = 90;

const BODY_CLASS = "has-custom-visualizer-bg";

/** Only what the engine hands out: the setting is shared, so nothing else may reach a CSS url(). */
const URL_PATTERN = /^ui-backgrounds\/[0-9a-f]{16}\.(?:png|jpg|gif|webp)$/;

/** The picture's URL, or null when there is none (or the stored value is not one the engine gave). */
export function visualizerBackgroundUrl(): string | null {
  const stored = getAppSetting(VISUALIZER_BACKGROUND_IMAGE_SETTING);
  return typeof stored === "string" && URL_PATTERN.test(stored) ? stored : null;
}

/** The fade, as a whole percent within 0..VISUALIZER_BACKGROUND_FADE_MAX. */
export function visualizerBackgroundFade(): number {
  const stored = Number(getAppSetting(VISUALIZER_BACKGROUND_FADE_SETTING));
  if (getAppSetting(VISUALIZER_BACKGROUND_FADE_SETTING) === null || !Number.isFinite(stored)) {
    return VISUALIZER_BACKGROUND_FADE_DEFAULT;
  }
  return Math.round(Math.max(0, Math.min(VISUALIZER_BACKGROUND_FADE_MAX, stored)));
}

function setFadeProperty(percent: number): void {
  document.body.style.setProperty("--custom-visualizer-bg-fade", String(percent / 100));
}

/** Draws the stored picture and fade, or the theme's own backdrop when there is no picture. */
export function applyVisualizerBackground(): void {
  const url = visualizerBackgroundUrl();
  const body = document.body;
  if (url) {
    // Absolute: a relative url() in a custom property resolves against the stylesheet that uses
    // it (css/), not the page.
    body.style.setProperty("--custom-visualizer-bg", `url("${new URL(url, document.baseURI).href}")`);
    setFadeProperty(visualizerBackgroundFade());
    body.classList.add(BODY_CLASS);
  } else {
    body.classList.remove(BODY_CLASS);
    body.style.removeProperty("--custom-visualizer-bg");
    body.style.removeProperty("--custom-visualizer-bg-fade");
  }
  refreshVisualizerBackgroundControls();
}

/** The engine stored the picture the player chose. */
export function onUiBackgroundImageSelected(payload: { url?: unknown }): void {
  if (typeof payload.url !== "string" || !URL_PATTERN.test(payload.url)) {
    return;
  }
  updateAppSetting(VISUALIZER_BACKGROUND_IMAGE_SETTING, payload.url);
  applyVisualizerBackground();
}

function chooseButton(): HTMLButtonElement | null {
  return document.getElementById("visualizer-background-choose") as HTMLButtonElement | null;
}

function removeButton(): HTMLButtonElement | null {
  return document.getElementById("visualizer-background-remove") as HTMLButtonElement | null;
}

function fadeInput(): HTMLInputElement | null {
  return document.getElementById("visualizer-background-fade") as HTMLInputElement | null;
}

function fadeValue(): HTMLElement | null {
  return document.getElementById("visualizer-background-fade-value");
}

function preview(): HTMLElement | null {
  return document.getElementById("visualizer-background-preview");
}

/** Shows the stored choice; app settings arrive after the panel binds. */
export function refreshVisualizerBackgroundControls(): void {
  const url = visualizerBackgroundUrl();
  const fade = visualizerBackgroundFade();
  const remove = removeButton();
  if (remove) remove.disabled = !url;
  const choose = chooseButton();
  if (choose) choose.textContent = url ? "Change Image…" : "Choose Image…";
  const input = fadeInput();
  if (input) {
    input.disabled = !url;
    // Leave a slider being dragged where the pointer has it.
    if (document.activeElement !== input) input.value = String(fade);
  }
  const value = fadeValue();
  if (value) value.textContent = `${input && document.activeElement === input ? input.value : fade}%`;
  const box = preview();
  if (box) {
    box.hidden = !url;
    box.style.backgroundImage = url ? `url("${url}")` : "";
  }
}

export function initVisualizerBackgroundControls(): void {
  const choose = chooseButton();
  if (!choose || choose.dataset.bound === "true") return;
  choose.dataset.bound = "true";

  choose.addEventListener("click", () => {
    postMessage({ type: "browseUiBackgroundImage" });
  });

  removeButton()?.addEventListener("click", () => {
    updateAppSetting(VISUALIZER_BACKGROUND_IMAGE_SETTING, null);
    postMessage({ type: "clearUiBackgroundImage" });
    applyVisualizerBackground();
  });

  const input = fadeInput();
  if (input) {
    input.min = "0";
    input.max = String(VISUALIZER_BACKGROUND_FADE_MAX);
    input.step = "5";
    // Drawn while dragging, stored once let go.
    input.addEventListener("input", () => {
      setFadeProperty(Number(input.value));
      const value = fadeValue();
      if (value) value.textContent = `${input.value}%`;
    });
    input.addEventListener("change", () => {
      updateAppSetting(VISUALIZER_BACKGROUND_FADE_SETTING, Number(input.value));
      applyVisualizerBackground();
    });
  }

  refreshVisualizerBackgroundControls();
}
