/**
 * The looper strip: a bar docked above the footer, opened from the footer's Loop button (or the
 * compact rail's Looper), with the transport, the loop's waveform and time, the record modes, and
 * the layers panel. Behind the Looper feature flag, and only in the standalone app (the engine
 * says whether it is available).
 *
 * The engine is the looper (LooperService); this sends presses and settings and shows what it
 * reports. docs/plans/looper.md is the design.
 */

import {
  requestLooperProjects,
  requestLooperState,
  sendDeleteLooperProject,
  sendDismissLooperRecovery,
  sendExportLooperLayer,
  sendLoadLooperProject,
  sendLooperCommand,
  sendLooperLayer,
  sendLooperSettings,
  sendRecoverLooperSession,
  sendAddRiffAsLooperLayer,
  sendRemoveLooperLayer,
  sendRetoneLooperLayers,
  sendSaveLooperProject,
  setMetronome,
} from "./bridge.js";
import { isCompact } from "./compactMode.js";
import { showConfirm } from "./dialogs.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "./featureFlags.js";
import { registerLooperRenderer } from "./looper/refresh.js";
import {
  getLooperLayers,
  getLooperProjects,
  getLooperStatus,
  isLooperLayersOpen,
  isLooperStripOpen,
  setLooperLayersOpen,
  setLooperStripOpen,
} from "./looper/state.js";
import { renderLooperStrip, updateLooperStrip } from "./looper/strip.js";
import { showNotification } from "./notifications.js";

const NOTICES: Record<string, string> = {
  disabled: "The looper is off. Turn it on in Settings → Features.",
  noCapture: "The looper is still getting ready. Try again in a moment.",
  tooLong: "That many bars is longer than the longest loop. Choose fewer bars, or a longer loop in the looper's options.",
  noSpare: "The next layer was not ready in time. Press again.",
  noLayerFree: "A loop holds 16 layers at most. Remove or undo one first.",
  noTempo: "Set a tempo in the metronome first.",
  tooShort: "Too short to loop.",
  memory: "The looper is out of memory. Remove or undo some layers, or raise its memory limit.",
  unknownCommand: "The looper did not understand that press.",
  nothingToSave: "There is no loop to save yet.",
  saveFailed: "The loop could not be saved.",
  loadFailed: "That loop could not be opened.",
  exported: "Layer saved as a WAV file.",
  exportFailed: "The layer could not be saved as a WAV file.",
  retoneFailed: "That preset cannot be rendered offline.",
  riffFailed: "That riff could not be read.",
};

let strip: HTMLElement | null = null;
/** The footer's Loop button and the compact rail's Looper button. */
let buttons: HTMLButtonElement[] = [];
let lastShape = "";
let lastNotice = "";
let enabledSent: boolean | null = null;
let initialized = false;

function isLooperOn(): boolean {
  return isFeatureEnabled(Features.Looper);
}

/** What changes the strip's markup, as opposed to what moves while it plays. */
function shapeKey(): string {
  const status = getLooperStatus();

  if (!status) {
    return "";
  }

  const { positionSec: _p, armWaitingSec: _a, recordedSec: _r, barIndex: _b, memoryMb: _m, notice: _n, ...shape } = status;
  const layers = getLooperLayers().map((l) => [l.id, l.title, l.muted, l.solo, l.recording, l.presetName, l.toneChanged, l.wetSource, l.canRevert, l.retoneProgress]);
  const projects = getLooperProjects().map((p) => [p.id, p.title, p.layerCount]);
  return JSON.stringify([shape, isLooperLayersOpen() ? layers : layers.length, projects]);
}

function render(full: boolean): void {
  const status = getLooperStatus();
  const on = isLooperOn();
  const available = Boolean(status?.available);

  // The engine follows the flag.
  if (status && available && status.settings.enabled !== on && enabledSent !== on) {
    enabledSent = on;
    sendLooperSettings({ enabled: on });
  } else if (status && status.settings.enabled === on) {
    enabledSent = null;
  }

  for (const button of buttons) {
    button.hidden = !(on && available);
    button.setAttribute("aria-pressed", String(isLooperStripOpen()));
  }

  if (status?.notice && status.notice !== lastNotice) {
    showNotification(NOTICES[status.notice] ?? status.notice, status.noticeDetail ?? "");
  }

  lastNotice = status?.notice ?? "";

  if (!strip) {
    return;
  }

  const visible = on && available && isLooperStripOpen();
  strip.hidden = !visible;

  if (!visible) {
    strip.classList.remove("is-more-open");
    lastShape = "";
    return;
  }

  const shape = shapeKey();

  if (full || shape !== lastShape) {
    // What a redraw would lose: which menus are open, and a loop name being typed.
    const open = Array.from(strip.querySelectorAll<HTMLDetailsElement>("details")).filter((d) => d.open).map((d) => d.className);
    const title = strip.querySelector<HTMLInputElement>("[data-looper-title]");
    const typing = title && document.activeElement === title ? title.value : null;
    renderLooperStrip(strip, isCompact());

    for (const details of Array.from(strip.querySelectorAll<HTMLDetailsElement>("details"))) {
      details.open = open.includes(details.className);
    }

    const nextTitle = strip.querySelector<HTMLInputElement>("[data-looper-title]");

    if (typing !== null && nextTitle) {
      nextTitle.value = typing;
      nextTitle.focus();
    }

    lastShape = shape;
  } else {
    updateLooperStrip(strip);
  }
}

/**
 * The compact strip's "More" sheet (Redo, Clear, the modes, the loop's project). Open while the
 * strip has is-more-open; the class outlives a redraw, which only rewrites the strip's contents.
 */
function setMoreOpen(open: boolean, focusButton = false): void {
  if (!strip) {
    return;
  }

  strip.classList.toggle("is-more-open", open);
  const more = strip.querySelector<HTMLButtonElement>("[data-looper-more]");
  more?.setAttribute("aria-expanded", String(open));

  if (focusButton) {
    more?.focus();
  }
}

function isMoreOpen(): boolean {
  return Boolean(strip?.classList.contains("is-more-open"));
}

/** A tap anywhere but the sheet, its button or a dialog it raised (Clear, Open) closes it. */
function closeMoreOnOutsideClick(event: MouseEvent): void {
  if (!isMoreOpen() || !strip) {
    return;
  }

  // The path as dispatched: a press that redrew the strip has already detached its target.
  const inside = event.composedPath().some((node) =>
    node instanceof HTMLElement && (node.matches(".looper-more-sheet, [data-looper-more]") || node.id === "dialog-modal"));

  if (!inside) {
    setMoreOpen(false);
  }
}

function layerIdOf(target: Element): string {
  return target.closest<HTMLElement>(".looper-layer-row")?.dataset.layerId ?? "";
}

function handleClick(event: MouseEvent): void {
  // Element, not HTMLElement: a tap on a button's icon lands on its SVG.
  const target = event.target instanceof Element ? event.target : null;
  const status = getLooperStatus();

  if (!target || !status) {
    return;
  }

  if (target.closest("[data-looper-more]")) {
    setMoreOpen(!isMoreOpen());
    return;
  }

  const commandButton = target.closest<HTMLElement>("[data-looper-command]");

  if (commandButton) {
    const command = commandButton.dataset.looperCommand ?? "";

    if (command === "clear" && status.dirty) {
      void showConfirm("Clear the loop? It has not been saved.", "Clear loop", { ok: "Clear" }).then((ok) => {
        if (ok) {
          sendLooperCommand(command);
        }
      });
      return;
    }

    sendLooperCommand(command);
    return;
  }

  if (target.closest("[data-looper-save]")) {
    sendSaveLooperProject(strip?.querySelector<HTMLInputElement>("[data-looper-title]")?.value.trim() ?? "");
    setMoreOpen(false);
    return;
  }

  const projectId = target.closest<HTMLElement>(".looper-project-row")?.dataset.projectId ?? "";

  if (projectId && target.closest("[data-looper-open]")) {
    setMoreOpen(false);

    if (!status.dirty) {
      sendLoadLooperProject(projectId);
      return;
    }

    void showConfirm("Open this loop? The one playing has not been saved.", "Open loop", { ok: "Open" }).then((ok) => {
      if (ok) {
        sendLoadLooperProject(projectId);
      }
    });
    return;
  }

  if (projectId && target.closest("[data-looper-delete]")) {
    void showConfirm("Delete this saved loop?", "Delete loop", { ok: "Delete" }).then((ok) => {
      if (ok) {
        sendDeleteLooperProject(projectId);
      }
    });
    return;
  }

  const riffTake = target.closest<HTMLElement>("[data-looper-add-riff]")?.dataset.looperAddRiff;

  if (riffTake) {
    sendAddRiffAsLooperLayer(riffTake);
    return;
  }

  if (target.closest("[data-looper-retone-all]")) {
    sendRetoneLooperLayers("all");
    return;
  }

  if (target.closest("[data-looper-recover]")) {
    sendRecoverLooperSession();
    return;
  }

  if (target.closest("[data-looper-dismiss]")) {
    sendDismissLooperRecovery();
    return;
  }

  const setting = target.closest<HTMLElement>("[data-looper-set]");

  if (setting) {
    sendLooperSettings({ [setting.dataset.looperSet ?? ""]: setting.dataset.value });
    return;
  }

  const step = target.closest<HTMLElement>("[data-looper-bars]");

  if (step) {
    const bars = Math.min(64, Math.max(1, status.settings.bars + Number(step.dataset.looperBars ?? 0)));
    sendLooperSettings({ bars });
    return;
  }

  if (target.closest("[data-looper-click]")) {
    setMetronome({ enabled: !status.click });
    return;
  }

  if (target.closest("[data-looper-tempo]")) {
    document.getElementById("footer-metronome-btn")?.click();
    return;
  }

  if (target.closest("[data-looper-layers]")) {
    setLooperLayersOpen(!isLooperLayersOpen());
    render(true);
    return;
  }

  const id = layerIdOf(target);
  const layer = getLooperLayers().find((l) => l.id === id);

  if (!layer) {
    return;
  }

  if (target.closest("[data-looper-layer-mute]")) {
    sendLooperLayer(id, { muted: !layer.muted });
  } else if (target.closest("[data-looper-layer-solo]")) {
    sendLooperLayer(id, { solo: !layer.solo });
  } else if (target.closest("[data-looper-layer-remove]")) {
    sendRemoveLooperLayer(id);
  } else if (target.closest("[data-looper-layer-retone]")) {
    sendRetoneLooperLayers([id]);
  } else if (target.closest("[data-looper-layer-revert]")) {
    sendRetoneLooperLayers([id], true);
  } else {
    const exportAs = target.closest<HTMLElement>("[data-looper-layer-export]")?.dataset.looperLayerExport;

    if (exportAs === "riff" || exportAs === "wetWav") {
      sendExportLooperLayer(id, exportAs);
    }
  }
}

function handleInput(event: Event): void {
  const input = event.target instanceof HTMLInputElement ? event.target : null;

  if (!input) {
    return;
  }

  const id = layerIdOf(input);

  if (id && input.matches("[data-looper-layer-level]")) {
    sendLooperLayer(id, { level: Number(input.value) });
  } else if (id && input.matches("[data-looper-layer-balance]")) {
    sendLooperLayer(id, { balance: Number(input.value) });
  }
}

function handleChange(event: Event): void {
  const field = event.target instanceof HTMLInputElement || event.target instanceof HTMLSelectElement ? event.target : null;

  if (!field) {
    return;
  }

  const id = layerIdOf(field);

  if (id && field.matches("[data-looper-layer-title]")) {
    sendLooperLayer(id, { title: field.value });
    return;
  }

  const key = field.dataset.looperOption;

  if (!key) {
    return;
  }

  if (field instanceof HTMLInputElement && field.type === "checkbox") {
    sendLooperSettings({ [key]: field.checked });
  } else if (key === "afterRecord") {
    sendLooperSettings({ [key]: field.value });
  } else if (Number.isFinite(Number(field.value))) {
    sendLooperSettings({ [key]: Number(field.value) });
  }
}

export function initializeLooper(): void {
  // Once: a second call only asks for the state again.
  if (initialized) {
    requestLooperState();
    return;
  }

  initialized = true;
  strip = document.getElementById("looper-strip");
  buttons = ["footer-looper-btn", "nav-rail-looper-btn"]
    .map((id) => document.getElementById(id))
    .filter((element): element is HTMLButtonElement => element instanceof HTMLButtonElement);
  registerLooperRenderer(render);

  for (const button of buttons) {
    button.addEventListener("click", () => {
      setLooperStripOpen(!isLooperStripOpen());
      render(true);

      if (isLooperStripOpen()) {
        requestLooperProjects();
      }
    });
  }

  strip?.addEventListener("click", handleClick);
  strip?.addEventListener("input", handleInput);
  strip?.addEventListener("change", handleChange);
  // The saved loops are asked for when their menu opens.
  strip?.addEventListener("toggle", (event) => {
    if (event.target instanceof HTMLDetailsElement && event.target.open && event.target.matches("[data-looper-projects]")) {
      requestLooperProjects();
    }
  }, true);
  document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => render(true));
  document.addEventListener("click", closeMoreOnOutsideClick);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && isMoreOpen()) {
      setMoreOpen(false, true);
    }
  });
  // The strip's markup differs by density (the sheet is compact-only), and the sheet goes with it.
  window.addEventListener("densityChanged", () => {
    if (!isCompact()) {
      setMoreOpen(false);
    }

    render(true);
  });
  // The first state is asked for by main.ts after uiReady, once replies have somewhere to go.
}
