import { getCurrentUiSettings } from "../windowSettings.js";
import { updateUiSettings } from "../windowSettings.js";
import {
  signalPathNodesElement,
} from "./state.js";
import { isMixTabActive } from "./state.js";
export const SIGNAL_PATH_FULL_HEIGHT = 96;

export const SIGNAL_PATH_COMPACT_HEIGHT = 48;

export const SIGNAL_PATH_LEGACY_COMPACT_HEIGHT_THRESHOLD = 80;

export const SIGNAL_PATH_MODE_GESTURE_THRESHOLD = 12;

export type SignalPathDensity = "compact" | "full";

export let signalPathScrollHeight = SIGNAL_PATH_FULL_HEIGHT;

export let signalPathResizeInitialized = false;

export let signalPathLayoutAdaptRaf = 0;

export function getSignalPathBarElement(): HTMLElement | null {
  return document.getElementById("signal-path-bar");
}

export function getSignalPathScrollElement(): HTMLElement | null {
  return document.querySelector<HTMLElement>(".signal-path-scroll");
}

export function getSignalPathResizeHandle(): HTMLElement | null {
  return document.getElementById("signal-path-resize-handle");
}

export function heightForSignalPathDensity(density: SignalPathDensity): number {
  return density === "compact" ? SIGNAL_PATH_COMPACT_HEIGHT : SIGNAL_PATH_FULL_HEIGHT;
}

export function densityFromSignalPathHeight(height: number): SignalPathDensity {
  return height <= SIGNAL_PATH_LEGACY_COMPACT_HEIGHT_THRESHOLD ? "compact" : "full";
}

export function syncSignalPathResizeHandleAria(height: number): void {
  const handle = getSignalPathResizeHandle();
  if (!handle) {
    return;
  }
  const density = densityFromSignalPathHeight(height);
  handle.setAttribute("aria-valuemin", String(SIGNAL_PATH_COMPACT_HEIGHT));
  handle.setAttribute("aria-valuemax", String(SIGNAL_PATH_FULL_HEIGHT));
  handle.setAttribute("aria-valuenow", String(height));
  handle.setAttribute("aria-valuetext", density === "compact" ? "Compact signal chain" : "Full signal chain");
}

/**
 * Apply the signal-chain mode selected by the splitter gesture. Node
 * dimensions remain fixed and never react to the available panel space.
 */
export function updateSignalPathLayoutAdapt(): void {
  const bar = getSignalPathBarElement();
  const scroll = getSignalPathScrollElement();
  const nodes = signalPathNodesElement;

  if (!bar || !scroll || !nodes || scroll.hidden || isMixTabActive()) {
    bar?.removeAttribute("data-density");
    return;
  }

  const density = densityFromSignalPathHeight(signalPathScrollHeight);
  if (bar.dataset.density !== density) {
    bar.dataset.density = density;
  }

  const minimumHeight = heightForSignalPathDensity(density);
  bar.style.setProperty("--signal-path-scroll-height", `${minimumHeight}px`);
  const viewportHeight = Math.max(minimumHeight, Math.ceil(scroll.scrollHeight));
  bar.style.setProperty("--signal-path-scroll-height", `${viewportHeight}px`);
}

export function scheduleSignalPathLayoutAdapt(): void {
  if (signalPathLayoutAdaptRaf) {
    cancelAnimationFrame(signalPathLayoutAdaptRaf);
  }
  signalPathLayoutAdaptRaf = requestAnimationFrame(() => {
    signalPathLayoutAdaptRaf = 0;
    updateSignalPathLayoutAdapt();
  });
}

/**
 * Apply one of the signal-chain's fixed visual modes. Legacy arbitrary saved
 * heights are normalized to the closest mode.
 */
export function setSignalPathScrollHeight(height: number, options?: { persist?: boolean }): number {
  const nextHeight = heightForSignalPathDensity(densityFromSignalPathHeight(height));
  signalPathScrollHeight = nextHeight;

  const bar = getSignalPathBarElement();
  if (bar) {
    bar.style.setProperty("--signal-path-scroll-height", `${nextHeight}px`);
  }
  syncSignalPathResizeHandleAria(nextHeight);
  scheduleSignalPathLayoutAdapt();

  if (options?.persist) {
    updateUiSettings({
      signalPathHeight: nextHeight,
    });
  }

  return nextHeight;
}

export function getSignalPathScrollHeight(): number {
  return signalPathScrollHeight;
}

export function onSignalPathResizePointerDown(event: PointerEvent): void {
  if (event.button !== 0) {
    return;
  }

  const handle = getSignalPathResizeHandle();
  if (!handle) {
    return;
  }

  event.preventDefault();
  const startY = event.clientY;
  const startDensity = densityFromSignalPathHeight(signalPathScrollHeight);
  let targetDensity = startDensity;

  handle.setPointerCapture(event.pointerId);
  document.body.classList.add("signal-path-resizing");

  const onMove = (moveEvent: PointerEvent): void => {
    const delta = moveEvent.clientY - startY;
    const nextDensity = delta <= -SIGNAL_PATH_MODE_GESTURE_THRESHOLD
      ? "compact"
      : delta >= SIGNAL_PATH_MODE_GESTURE_THRESHOLD
        ? "full"
        : startDensity;
    if (nextDensity === targetDensity) {
      return;
    }
    targetDensity = nextDensity;
    setSignalPathScrollHeight(heightForSignalPathDensity(targetDensity), { persist: false });
  };

  const finish = (endEvent: PointerEvent): void => {
    handle.releasePointerCapture(endEvent.pointerId);
    handle.removeEventListener("pointermove", onMove);
    handle.removeEventListener("pointerup", finish);
    handle.removeEventListener("pointercancel", finish);
    document.body.classList.remove("signal-path-resizing");
    if (endEvent.type === "pointercancel") {
      setSignalPathScrollHeight(heightForSignalPathDensity(startDensity), { persist: false });
      return;
    }
    setSignalPathScrollHeight(heightForSignalPathDensity(targetDensity), { persist: true });
  };

  handle.addEventListener("pointermove", onMove);
  handle.addEventListener("pointerup", finish);
  handle.addEventListener("pointercancel", finish);
}

export function onSignalPathResizeKeyDown(event: KeyboardEvent): void {
  let nextDensity: SignalPathDensity | null = null;
  if (event.key === "ArrowUp" || event.key === "ArrowLeft") {
    nextDensity = "compact";
  } else if (event.key === "ArrowDown" || event.key === "ArrowRight") {
    nextDensity = "full";
  } else if (event.key === "Home") {
    nextDensity = "compact";
  } else if (event.key === "End") {
    nextDensity = "full";
  } else if (event.key === "Enter" || event.key === " ") {
    nextDensity = "full";
  }

  if (nextDensity === null) {
    return;
  }

  event.preventDefault();
  setSignalPathScrollHeight(heightForSignalPathDensity(nextDensity), { persist: true });
}

export function onSignalPathResizeDoubleClick(event: MouseEvent): void {
  event.preventDefault();
  setSignalPathScrollHeight(SIGNAL_PATH_FULL_HEIGHT, { persist: true });
}

/** Restores the persisted signal-path height, snapped to the nearest mode. */
function applySignalPathHeightFromSettings(): void {
  const settings = getCurrentUiSettings();
  const stored = typeof settings.signalPathHeight === "number" && Number.isFinite(settings.signalPathHeight)
    ? settings.signalPathHeight
    : signalPathScrollHeight;
  const density = densityFromSignalPathHeight(stored);
  setSignalPathScrollHeight(heightForSignalPathDensity(density), { persist: false });
}

export function initSignalPathResize(): void {
  if (signalPathResizeInitialized) {
    applySignalPathHeightFromSettings();
    return;
  }

  const handle = getSignalPathResizeHandle();
  if (!handle) {
    return;
  }

  signalPathResizeInitialized = true;
  handle.addEventListener("pointerdown", onSignalPathResizePointerDown);
  handle.addEventListener("keydown", onSignalPathResizeKeyDown);
  handle.addEventListener("dblclick", onSignalPathResizeDoubleClick);

  window.addEventListener("uiSettingsApplied", () => {
    applySignalPathHeightFromSettings();
  });

  window.addEventListener("resize", () => {
    scheduleSignalPathLayoutAdapt();
  });

  applySignalPathHeightFromSettings();
  scheduleSignalPathLayoutAdapt();
}
