/**
 * Shared range-selection gesture for the waveform editors.
 *
 * The Practice Tool (loop regions over a backing track) and the Riff Capture
 * editor (crop markers over a captured take) present the same interaction: two
 * handles on a canvas, dragged with the pointer or stepped with the keyboard.
 * They had grown separate implementations of that gesture, and only one of the
 * two had learned each lesson — the Practice Tool had a handle hit radius and a
 * click/drag threshold the Riff editor lacked, the Riff editor had Home/End
 * keys the Practice Tool lacked, and neither survived the pointer leaving the
 * window. This is the single implementation.
 *
 * What stays with the host: owning the range, drawing it, and deciding what a
 * committed change means. This module owns only the gesture — which handle is
 * being steered, when a press becomes a drag, and when a gesture is finished.
 *
 * Pointer events rather than mouse events, for the reasons set out at the top
 * of pointerDrag.ts: touch and pen work, and capture keeps a drag alive when
 * the pointer leaves the canvas or the window entirely.
 */

import { clampRatioRange, otherHandle, ratioFromPointer, type RangeHandle, type RatioRange } from "./range.js";

/**
 * A press within this many pixels of a handle grabs it. Without a radius every
 * press anywhere on the canvas yanks the nearest edge to the pointer, which
 * makes it impossible to ever click for any other purpose and leaves the
 * selection feeling like it is constantly slipping.
 */
export const HANDLE_HIT_PX = 10;

/**
 * Below this much movement a press/release is a click, not a drag. Real clicks
 * always carry a pixel or two of jitter, which would otherwise sweep out
 * zero-width ranges constantly.
 */
export const CLICK_DRAG_THRESHOLD_PX = 4;

/**
 * How long a press must be held before the resize cursor appears. A resting
 * hover or a quick click should keep the normal pointer; long enough that a
 * click never flashes it, short enough that a real drag feels immediate.
 */
export const CURSOR_HOLD_MS = 150;

/**
 * Keyboard edits commit after this much idle rather than on each key, so
 * holding an arrow key down (which auto-repeats) collapses into one save
 * instead of one disk write per repeat.
 */
const KEYBOARD_COMMIT_IDLE_MS = 400;

/** Applied to the canvas while a drag is genuinely in progress. */
export const RESIZING_CLASS = "is-resizing";

export type RangeSelectGesture = "resize" | "create" | "keyboard";

export interface RangeSelectSpec {
  canvas: HTMLCanvasElement;

  /** Gestures are ignored entirely when there is nothing to select over. */
  isEnabled(): boolean;

  /** The live bounds, or null when nothing is selected yet. */
  getRange(): RatioRange | null;

  /** Shortest allowed span, as a ratio. Re-read on every event. */
  getMinSpanRatio(): number;

  /**
   * Total length the 0..1 ratios span. Keyboard steps are specified in seconds
   * and converted through this, so a nudge means the same amount of audio on a
   * ten-second riff as on a four-minute backing track — expressing the step as
   * a raw ratio silently rescaled it with the material.
   */
  getDurationSec(): number;

  /** Keyboard step sizes in seconds; `coarse` is the Shift-held step. */
  nudgeStepSec: { fine: number; coarse: number };

  /** Move one bound. The range is already clamped and the handle resolved. */
  onResize(range: RatioRange, handle: RangeHandle): void;

  /**
   * Sweep out a brand new range from empty canvas. Omit to opt out, in which
   * case a drag that starts away from a handle does nothing.
   */
  onCreate?(range: RatioRange): void;

  /** Fires once when a press is promoted to a create sweep. */
  onCreateStart?(): void;

  /** A press with no meaningful movement. Omit to opt out. */
  onSeek?(ratio: number): void;

  /**
   * A gesture finished and something changed — the place to flush a debounced
   * send and persist. Not called for a seek, or for a press that changed
   * nothing.
   */
  onCommit?(gesture: RangeSelectGesture): void;

  /** The keyboard-steered handle changed; hosts draw the focus ring on it. */
  onSelectedHandleChange?(handle: RangeHandle): void;

  /** Redraw the canvas. */
  render(): void;
}

export interface RangeSelectController {
  /** Which handle the keyboard is steering. */
  getSelectedHandle(): RangeHandle;
  /** True while a create sweep is in flight — hosts hide "add" affordances. */
  isCreating(): boolean;
  /** Drop listeners and any in-flight gesture. */
  destroy(): void;
}

type DragMode = "handle" | "create" | "pending" | null;

export function bindRangeSelect(spec: RangeSelectSpec): RangeSelectController {
  const { canvas } = spec;

  let dragMode: DragMode = null;
  let activeHandle: RangeHandle = "start";
  let selectedHandle: RangeHandle = "start";
  let anchorRatio = 0;
  let pointerDownX = 0;
  let pointerDownY = 0;
  let pointerDownRatio = 0;
  let pointerId: number | null = null;
  let cursorHoldTimer: ReturnType<typeof setTimeout> | null = null;
  let keyboardCommitTimer: ReturnType<typeof setTimeout> | null = null;

  function setSelectedHandle(handle: RangeHandle): void {
    if (selectedHandle === handle) {
      return;
    }
    selectedHandle = handle;
    spec.onSelectedHandleChange?.(handle);
  }

  function clearCursorHold(): void {
    if (cursorHoldTimer !== null) {
      clearTimeout(cursorHoldTimer);
      cursorHoldTimer = null;
    }
    canvas.classList.remove(RESIZING_CLASS);
  }

  /**
   * Writes one bound of the existing range, flipping the steered handle when
   * the pointer crosses the other one. Without the flip the caller keeps
   * addressing the bound it is no longer holding, so every subsequent event
   * re-crosses and the region slides along at one step wide instead of growing
   * away from its anchor.
   */
  function resizeTo(ratio: number, handle: RangeHandle): void {
    const range = spec.getRange();
    if (!range) {
      return;
    }
    const next = handle === "start"
      ? clampRatioRange(ratio, range.endRatio, spec.getMinSpanRatio(), handle)
      : clampRatioRange(range.startRatio, ratio, spec.getMinSpanRatio(), handle);
    const resolved = next.swapped ? otherHandle(handle) : handle;
    if (next.swapped) {
      activeHandle = resolved;
      setSelectedHandle(resolved);
    }
    spec.onResize({ startRatio: next.startRatio, endRatio: next.endRatio }, resolved);
    spec.render();
  }

  function nearestHandle(ratio: number, range: RatioRange): RangeHandle {
    return Math.abs(ratio - range.startRatio) <= Math.abs(ratio - range.endRatio) ? "start" : "end";
  }

  function onPointerDown(event: PointerEvent): void {
    if (!event.isPrimary || event.button !== 0 || !spec.isEnabled()) {
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const ratio = ratioFromPointer(event, canvas);
    const range = spec.getRange();

    pointerId = event.pointerId;
    pointerDownX = event.clientX;
    pointerDownY = event.clientY;
    pointerDownRatio = ratio;
    canvas.setPointerCapture?.(event.pointerId);
    canvas.focus();
    event.preventDefault();

    clearCursorHold();
    cursorHoldTimer = setTimeout(() => {
      cursorHoldTimer = null;
      canvas.classList.add(RESIZING_CLASS);
    }, CURSOR_HOLD_MS);

    if (range) {
      // Steer whichever handle is nearest even when the press was nowhere near
      // it, so the arrow keys always act on the edge the user just gestured at.
      const nearest = nearestHandle(ratio, range);
      setSelectedHandle(nearest);

      const hitRatio = rect.width > 0 ? HANDLE_HIT_PX / rect.width : 0;
      const distance = Math.min(Math.abs(ratio - range.startRatio), Math.abs(ratio - range.endRatio));
      if (distance <= hitRatio) {
        dragMode = "handle";
        activeHandle = nearest;
        spec.render();
        return;
      }
    }

    // Away from any handle, defer the decision: no meaningful movement before
    // release makes this a click (a seek, where the host wants one), movement
    // past the threshold promotes it to a create sweep.
    dragMode = "pending";
    anchorRatio = ratio;
  }

  function onPointerMove(event: PointerEvent): void {
    if (dragMode === null || event.pointerId !== pointerId || !spec.isEnabled()) {
      return;
    }

    if (dragMode === "pending") {
      const moved = Math.hypot(event.clientX - pointerDownX, event.clientY - pointerDownY);
      if (moved < CLICK_DRAG_THRESHOLD_PX) {
        return;
      }
      if (!spec.onCreate) {
        return; // host does not support sweeping out a new range
      }
      dragMode = "create";
      spec.onCreateStart?.();
    }

    const ratio = ratioFromPointer(event, canvas);

    if (dragMode === "create") {
      // In a sweep the anchor is fixed and the pointer is the far edge, so the
      // minimum-span push must move the pointer side, not the anchor.
      const sweepMoving: RangeHandle = ratio >= anchorRatio ? "end" : "start";
      const swept = clampRatioRange(Math.min(anchorRatio, ratio), Math.max(anchorRatio, ratio), spec.getMinSpanRatio(), sweepMoving);
      spec.onCreate?.({ startRatio: swept.startRatio, endRatio: swept.endRatio });
      spec.render();
      return;
    }

    resizeTo(ratio, activeHandle);
  }

  function endGesture(cancelled: boolean): void {
    const mode = dragMode;
    dragMode = null;
    clearCursorHold();
    if (pointerId !== null && canvas.hasPointerCapture?.(pointerId)) {
      canvas.releasePointerCapture(pointerId);
    }
    pointerId = null;

    if (cancelled || mode === null) {
      spec.render();
      return;
    }
    if (mode === "pending") {
      spec.onSeek?.(pointerDownRatio);
    } else {
      spec.onCommit?.(mode === "create" ? "create" : "resize");
    }
    spec.render();
  }

  function onPointerUp(event: PointerEvent): void {
    if (event.pointerId !== pointerId) {
      return;
    }
    endGesture(false);
  }

  function onPointerCancel(event: PointerEvent): void {
    if (event.pointerId !== pointerId) {
      return;
    }
    endGesture(true);
  }

  function scheduleKeyboardCommit(): void {
    if (keyboardCommitTimer !== null) {
      clearTimeout(keyboardCommitTimer);
    }
    keyboardCommitTimer = setTimeout(() => {
      keyboardCommitTimer = null;
      spec.onCommit?.("keyboard");
    }, KEYBOARD_COMMIT_IDLE_MS);
  }

  function stepRatio(coarse: boolean): number {
    const duration = spec.getDurationSec();
    const seconds = coarse ? spec.nudgeStepSec.coarse : spec.nudgeStepSec.fine;
    return duration > 0 ? seconds / duration : 0;
  }

  function onKeyDown(event: KeyboardEvent): void {
    if (!spec.isEnabled()) {
      return;
    }
    const range = spec.getRange();
    if (!range) {
      return;
    }

    switch (event.key) {
      case "ArrowLeft":
      case "ArrowRight": {
        event.preventDefault();
        const direction = event.key === "ArrowLeft" ? -1 : 1;
        const from = selectedHandle === "start" ? range.startRatio : range.endRatio;
        resizeTo(from + direction * stepRatio(event.shiftKey), selectedHandle);
        scheduleKeyboardCommit();
        return;
      }
      case "ArrowUp":
      case "ArrowDown":
        event.preventDefault();
        setSelectedHandle(otherHandle(selectedHandle));
        spec.render();
        return;
      case "Home":
        event.preventDefault();
        resizeTo(0, selectedHandle);
        scheduleKeyboardCommit();
        return;
      case "End":
        event.preventDefault();
        resizeTo(1, selectedHandle);
        scheduleKeyboardCommit();
        return;
      default:
    }
  }

  canvas.addEventListener("pointerdown", onPointerDown);
  canvas.addEventListener("pointermove", onPointerMove);
  canvas.addEventListener("pointerup", onPointerUp);
  canvas.addEventListener("pointercancel", onPointerCancel);
  canvas.addEventListener("keydown", onKeyDown);

  return {
    getSelectedHandle: () => selectedHandle,
    isCreating: () => dragMode === "create",
    destroy(): void {
      endGesture(true);
      if (keyboardCommitTimer !== null) {
        clearTimeout(keyboardCommitTimer);
        keyboardCommitTimer = null;
      }
      canvas.removeEventListener("pointerdown", onPointerDown);
      canvas.removeEventListener("pointermove", onPointerMove);
      canvas.removeEventListener("pointerup", onPointerUp);
      canvas.removeEventListener("pointercancel", onPointerCancel);
      canvas.removeEventListener("keydown", onKeyDown);
    },
  };
}
