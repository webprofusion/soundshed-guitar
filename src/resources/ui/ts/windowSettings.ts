import { postMessage } from "./bridge.js";
import type { UiSettings } from "./types.js";

let currentSettings: UiSettings = { zoom: 1 };
let pendingSend = false;
const sendDelayMs = 250;

function captureBounds() {
  return {
    x: typeof window.screenX === "number" ? window.screenX : 0,
    y: typeof window.screenY === "number" ? window.screenY : 0,
    width: typeof window.outerWidth === "number" ? window.outerWidth : window.innerWidth,
    height: typeof window.outerHeight === "number" ? window.outerHeight : window.innerHeight,
  };
}

function captureZoom(): number {
  const viewport = window.visualViewport;
  if (viewport && typeof viewport.scale === "number") {
    return viewport.scale;
  }
  const zoomStyle = (document.body.style.zoom || "").trim();
  const parsed = zoomStyle ? parseFloat(zoomStyle) : NaN;
  return Number.isFinite(parsed) ? parsed : 1;
}

function scheduleSend() {
  if (pendingSend) return;
  pendingSend = true;
  window.setTimeout(() => {
    pendingSend = false;
    postMessage({ type: "uiSettingsChanged", settings: currentSettings });
  }, sendDelayMs);
}

export function applyUiSettings(settings?: UiSettings): void {
  if (!settings) return;
  currentSettings = { ...currentSettings, ...settings };

  const zoom = settings.zoom ?? currentSettings.zoom ?? 1;
  document.body.style.zoom = `${zoom}`;

  const bounds = settings.bounds;
  if (bounds && typeof window.resizeTo === "function") {
    try {
      window.resizeTo(bounds.width, bounds.height);
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
      zoom: captureZoom(),
      bounds: captureBounds(),
    };
  };

  updateMetrics();
  scheduleSend();

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

export function getCurrentUiSettings(): UiSettings {
  return currentSettings;
}
