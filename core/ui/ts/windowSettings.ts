import { postMessage } from "./bridge.js";
import type { UiSettings } from "./types.js";

let currentSettings: UiSettings = { zoom: 1 };
let pendingSend = false;
const sendDelayMs = 250;
const DEFAULT_ZOOM = 1;

function clampZoom(value: number): number {
  if (!Number.isFinite(value)) {
    return DEFAULT_ZOOM;
  }
  return Math.max(0.5, Math.min(2.0, value));
}

function readAppliedZoom(): number {
  const inlineZoom = (document.body.style.zoom || "").trim();
  if (inlineZoom.length > 0) {
    const parsedInline = Number.parseFloat(inlineZoom);
    if (Number.isFinite(parsedInline)) {
      return clampZoom(parsedInline);
    }
  }

  const computedZoom = window.getComputedStyle(document.body).zoom;
  if (computedZoom && computedZoom !== "normal") {
    const parsedComputed = Number.parseFloat(computedZoom);
    if (Number.isFinite(parsedComputed)) {
      return clampZoom(parsedComputed);
    }
  }

  return DEFAULT_ZOOM;
}

function applyZoomToDocument(zoom: number): number {
  const clampedZoom = clampZoom(zoom);
  document.body.style.zoom = `${clampedZoom}`;
  return clampedZoom;
}

function notifyUiSettingsApplied(): void {
  window.dispatchEvent(new CustomEvent("uiSettingsApplied", {
    detail: { settings: currentSettings },
  }));
}

function captureBounds() {
  return {
    x: typeof window.screenX === "number" ? window.screenX : 0,
    y: typeof window.screenY === "number" ? window.screenY : 0,
    width: typeof window.outerWidth === "number" ? window.outerWidth : window.innerWidth,
    height: typeof window.outerHeight === "number" ? window.outerHeight : window.innerHeight,
  };
}

function captureZoom(): number {
  return readAppliedZoom();
}

function scheduleSend() {
  if (pendingSend) return;
  pendingSend = true;
  window.setTimeout(() => {
    pendingSend = false;
    postMessage({ type: "uiSettingsChanged", settings: currentSettings });
  }, sendDelayMs);
}

export function updateUiSettings(patch: Partial<UiSettings>): void {
  currentSettings = { ...currentSettings, ...patch };
  if (typeof patch.zoom === "number") {
    currentSettings.zoom = applyZoomToDocument(patch.zoom);
  }
  if (typeof patch.signalPathHeight === "number" && Number.isFinite(patch.signalPathHeight)) {
    currentSettings.signalPathHeight = patch.signalPathHeight;
  }
  notifyUiSettingsApplied();
  scheduleSend();
}

export function applyUiSettings(settings?: UiSettings): void {
  if (!settings) return;
  currentSettings = { ...currentSettings, ...settings };

  const zoom = settings.zoom ?? currentSettings.zoom ?? DEFAULT_ZOOM;
  currentSettings.zoom = applyZoomToDocument(zoom);

  if (typeof settings.signalPathHeight === "number" && Number.isFinite(settings.signalPathHeight)) {
    currentSettings.signalPathHeight = settings.signalPathHeight;
  }

  notifyUiSettingsApplied();

  const bounds = settings.bounds;
  if (bounds && typeof window.resizeTo === "function") {
    try {
      let targetWidth = bounds.width;
      let targetHeight = bounds.height;

      if (window.screen) {
        const maxWidth = window.screen.availWidth || window.screen.width;
        const maxHeight = window.screen.availHeight || window.screen.height;
        if (maxWidth && targetWidth > maxWidth) {
          targetWidth = maxWidth;
        }
        if (maxHeight && targetHeight > maxHeight) {
          targetHeight = maxHeight;
        }
      }

      window.resizeTo(targetWidth, targetHeight);
      if (typeof window.moveTo === "function") {
        window.moveTo(bounds.x, bounds.y);
      }
    } catch {
      // Some hosts may block programmatic window moves/resizes; ignore.
    }
  }
}

export function startUiSettingsTracking(): void {
  const updateMetrics = () => {
    currentSettings = {
      ...currentSettings,
      zoom: captureZoom(),
      bounds: captureBounds(),
    };
  };

  updateMetrics();

  const onResize = () => {
    updateMetrics();
    scheduleSend();
  };

  window.addEventListener("resize", onResize);
  if (window.visualViewport) {
    window.visualViewport.addEventListener("resize", onResize);
  }

  window.addEventListener("beforeunload", () => {
    updateMetrics();
    postMessage({ type: "uiSettingsChanged", settings: currentSettings });
  });
}

/**
 * Takes values the engine keeps in the UI settings itself (presetRecents) into the blob this
 * module sends back whole, without sending it. uiSettingsChanged replaces the engine's copy,
 * so the blob must carry the engine's current list, never an older one.
 */
export function adoptEngineUiSettings(patch: Partial<UiSettings>): void {
  currentSettings = { ...currentSettings, ...patch };
}

export function getCurrentUiSettings(): UiSettings {
  return currentSettings;
}
