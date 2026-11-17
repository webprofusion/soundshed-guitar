/**
 * Parameter changes that come from the engine rather than the panel: MIDI and host
 * automation, which arrive as `signalPathNodeParamUpdated` up to 30 times a second per
 * parameter. Rebuilding the params panel for each one cost ~15% of the renderer's main
 * thread during a sweep, so a change the open panel can show on its knob is shown there,
 * and only what it cannot show rebuilds the panel, at most every REBUILD_INTERVAL_MS.
 *
 * A rebuild replaces every control in the panel, so one that lands while a pointer is down
 * takes the control out from under it: with a host lane moving any setting the panel cannot
 * show in place, no knob, slider or menu on the panel could be dragged while the transport
 * ran. A rebuild waits for the pointer to come up.
 */

import { showEngineParamChange } from "./paramsPanel.js";
import { requestNodeParamsRefresh } from "./render.js";
import { getSelectedNodeId } from "./state.js";
import type { GraphNode } from "../types.js";

/** The longest a change that needs a rebuild waits for it; also the most often one runs. */
const REBUILD_INTERVAL_MS = 100;

let rebuildTimer: ReturnType<typeof setTimeout> | null = null;
let rebuildWaitingForPointerUp = false;
const pointersDown = new Set<number>();
let pointerTrackingInstalled = false;

function runRebuild(): void {
  rebuildWaitingForPointerUp = false;
  requestNodeParamsRefresh();
}

function onPointerDown(event: PointerEvent): void {
  pointersDown.add(event.pointerId);
}

function onPointerUp(event: PointerEvent): void {
  pointersDown.delete(event.pointerId);
  if (pointersDown.size === 0 && rebuildWaitingForPointerUp) {
    runRebuild();
  }
}

/** Listens at the document, in the capture phase, so no control can hide a press from it. */
function installPointerTracking(): void {
  if (pointerTrackingInstalled || typeof document === "undefined") {
    return;
  }
  pointerTrackingInstalled = true;
  document.addEventListener("pointerdown", onPointerDown, true);
  document.addEventListener("pointerup", onPointerUp, true);
  document.addEventListener("pointercancel", onPointerUp, true);
  // A release outside the WebView never arrives; losing focus clears what is held.
  window.addEventListener("blur", () => {
    pointersDown.clear();
    if (rebuildWaitingForPointerUp) {
      runRebuild();
    }
  });
}

function scheduleRebuild(): void {
  installPointerTracking();
  if (rebuildTimer !== null || rebuildWaitingForPointerUp) {
    return;
  }
  rebuildTimer = setTimeout(() => {
    rebuildTimer = null;
    if (pointersDown.size > 0) {
      rebuildWaitingForPointerUp = true;
      return;
    }
    runRebuild();
  }, REBUILD_INTERVAL_MS);
}

/**
 * Shows an engine-side change to `node.params[key]`, which the caller has already applied.
 * Only the selected node's controls are on screen, so a change to any other node draws
 * nothing; `changed` false (the engine echoing a value the UI already had) draws nothing.
 */
export function showEngineNodeParamChange(node: GraphNode, key: string, value: number, changed: boolean): void {
  if (!changed || node.id !== getSelectedNodeId()) {
    return;
  }
  if (!showEngineParamChange(node, key, value)) {
    scheduleRebuild();
  }
}
