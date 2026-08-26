/**
 * Pointer-driven drag helper.
 *
 * The app deliberately does *not* use HTML5 drag-and-drop for in-app dragging.
 * WebKitGTK (the Linux WebView) starts an HTML5 drag but never emits the `drop`
 * event on our targets, so signal-chain reordering was impossible on Linux
 * (issue #27). Native DnD is also the weaker API everywhere else: no touch
 * support, no control over the drag image, and `dataTransfer` payloads are
 * unreadable during `dragover`.
 *
 * Native drag-and-drop is still used — and must be — for drags that originate
 * *outside* the WebView (files dropped from the OS file manager), because that
 * is the only API the platform offers for them.
 *
 * A single drag is active at a time. Listeners live on `document` in the
 * capture phase rather than on the dragged element, so a drag survives the
 * source element being replaced mid-gesture (any backend state broadcast
 * re-renders the signal path by replacing `innerHTML`).
 */

/** Movement required before a press turns into a drag rather than a click. */
export const POINTER_DRAG_THRESHOLD_PX = 6;

export interface PointerDragTarget {
  /** Element highlighted while hovered, and used to detect target changes. */
  element: HTMLElement;
}

/** Movement of the pointer over the whole gesture, in CSS pixels. */
export interface PointerDragGesture {
  deltaX: number;
  deltaY: number;
}

export interface PointerDragSpec<TTarget extends PointerDragTarget> {
  /** Element the gesture started on; cloned to make the drag preview. */
  source: HTMLElement;
  /**
   * Selector matching `source`. Used to clear leftover `dragging` classes when
   * the source element was replaced by a re-render during the drag.
   */
  sourceSelector: string;
  /** Class applied to the cloned preview element. */
  previewClass: string;
  /** Give the preview the source's height as well as its width. */
  matchPreviewHeight?: boolean;
  /** Class applied to `document.body` while the drag is active. */
  bodyClass?: string;
  /** Resolve the drop target under the pointer, or null when there is none. */
  resolveTarget: (event: PointerEvent) => TTarget | null;
  /** Add or remove the drop-target highlight. */
  setTargetHighlight: (target: TTarget, highlighted: boolean) => void;
  /**
   * Called once the drag ends past the movement threshold. `target` is null
   * when the pointer was released away from any drop target, which callers use
   * to implement gestures (a vertical flick to bypass a node, say).
   * Not called for a click, or for a cancelled drag.
   */
  onDrop: (target: TTarget | null, gesture: PointerDragGesture) => void;
}

interface PointerDragSession<TTarget extends PointerDragTarget> {
  spec: PointerDragSpec<TTarget>;
  pointerId: number;
  startX: number;
  startY: number;
  lastX: number;
  lastY: number;
  active: boolean;
  preview: HTMLElement | null;
  target: TTarget | null;
}

let session: PointerDragSession<PointerDragTarget> | null = null;
let documentListenersBound = false;

/**
 * Body-level CSS zoom scales `position: fixed` coordinates, so pointer client
 * coordinates have to be divided by it before being used as `left`/`top`.
 */
export function getUiZoom(): number {
  const zoom = Number.parseFloat(window.getComputedStyle(document.body).zoom);
  return Number.isFinite(zoom) && zoom > 0 ? zoom : 1;
}

export function isPointerDragActive(): boolean {
  return Boolean(session?.active);
}

function positionPreview(preview: HTMLElement, event: PointerEvent): void {
  const zoom = getUiZoom();
  preview.style.left = `${Math.round(event.clientX / zoom)}px`;
  preview.style.top = `${Math.round(event.clientY / zoom)}px`;
}

function createPreview(active: PointerDragSession<PointerDragTarget>, event: PointerEvent): HTMLElement {
  const { spec } = active;
  const rect = spec.source.getBoundingClientRect();
  const zoom = getUiZoom();
  const preview = spec.source.cloneNode(true) as HTMLElement;
  preview.classList.remove("dragging", "drag-over", "selected");
  preview.classList.add(spec.previewClass);
  preview.removeAttribute("id");
  preview.style.width = `${rect.width / zoom}px`;
  if (spec.matchPreviewHeight) {
    preview.style.height = `${rect.height / zoom}px`;
  }
  document.body.appendChild(preview);
  positionPreview(preview, event);
  return preview;
}

function clearTarget(active: PointerDragSession<PointerDragTarget>): void {
  if (!active.target) return;
  active.spec.setTargetHighlight(active.target, false);
  active.target = null;
}

/**
 * Swallow the click the browser synthesises when the pointer is released, so a
 * drag never doubles as a click on whatever it happened to finish over. The
 * listener is removed by that click or by the next press, whichever lands
 * first — no timer, so there is no window in which a genuine click is eaten.
 */
function swallowNextClick(): void {
  const remove = (): void => {
    document.removeEventListener("click", onClick, true);
    document.removeEventListener("pointerdown", remove, true);
  };
  const onClick = (event: MouseEvent): void => {
    event.stopPropagation();
    event.preventDefault();
    remove();
  };
  document.addEventListener("click", onClick, true);
  document.addEventListener("pointerdown", remove, true);
}

function endSession(cancelled: boolean): void {
  const active = session;
  if (!active) return;
  session = null;

  const { spec } = active;
  const target = active.target;
  clearTarget(active);
  active.preview?.remove();
  spec.source.classList.remove("dragging");
  // A re-render during the drag leaves a fresh element wearing the class.
  document.querySelectorAll(`${spec.sourceSelector}.dragging`)
    .forEach((element) => element.classList.remove("dragging"));
  if (spec.bodyClass) {
    document.body.classList.remove(spec.bodyClass);
  }
  if (spec.source.hasPointerCapture?.(active.pointerId)) {
    spec.source.releasePointerCapture(active.pointerId);
  }

  if (!active.active || cancelled) return;

  swallowNextClick();
  spec.onDrop(target, {
    deltaX: active.lastX - active.startX,
    deltaY: active.lastY - active.startY,
  });
}

function onPointerMove(event: PointerEvent): void {
  const active = session;
  if (!active || active.pointerId !== event.pointerId) return;

  active.lastX = event.clientX;
  active.lastY = event.clientY;

  if (!active.active) {
    const distance = Math.hypot(event.clientX - active.startX, event.clientY - active.startY);
    if (distance < POINTER_DRAG_THRESHOLD_PX) return;
    active.active = true;
    active.spec.source.classList.add("dragging");
    if (active.spec.bodyClass) {
      document.body.classList.add(active.spec.bodyClass);
    }
    active.preview = createPreview(active, event);
  }

  event.preventDefault();
  if (active.preview) {
    positionPreview(active.preview, event);
  }

  const next = active.spec.resolveTarget(event);
  if (active.target?.element === next?.element) return;
  clearTarget(active);
  active.target = next;
  if (next) {
    active.spec.setTargetHighlight(next, true);
  }
}

function onPointerUp(event: PointerEvent): void {
  if (session?.pointerId !== event.pointerId) return;
  endSession(false);
}

function onPointerCancel(event: PointerEvent): void {
  if (session?.pointerId !== event.pointerId) return;
  endSession(true);
}

function bindDocumentListeners(): void {
  if (documentListenersBound) return;
  document.addEventListener("pointermove", onPointerMove, true);
  document.addEventListener("pointerup", onPointerUp, true);
  document.addEventListener("pointercancel", onPointerCancel, true);
  documentListenersBound = true;
}

/**
 * Start tracking a drag from a primary-button press. The drag only becomes
 * visible — and only reports a drop — once the pointer moves past
 * {@link POINTER_DRAG_THRESHOLD_PX}, so plain clicks are unaffected.
 */
export function beginPointerDrag<TTarget extends PointerDragTarget>(
  event: PointerEvent,
  spec: PointerDragSpec<TTarget>,
): void {
  if (!event.isPrimary || event.button !== 0) return;

  bindDocumentListeners();
  if (session) {
    endSession(true);
  }

  session = {
    spec: spec as unknown as PointerDragSpec<PointerDragTarget>,
    pointerId: event.pointerId,
    startX: event.clientX,
    startY: event.clientY,
    lastX: event.clientX,
    lastY: event.clientY,
    active: false,
    preview: null,
    target: null,
  };
  // Capture keeps events flowing to us if the pointer leaves the source; when a
  // re-render detaches the source the capture is released implicitly and the
  // document listeners above keep the drag alive regardless.
  spec.source.setPointerCapture?.(event.pointerId);
}

/** Test hook: drop any in-flight drag and its listeners. */
export function resetPointerDragForTests(): void {
  if (session) endSession(true);
  document.removeEventListener("pointermove", onPointerMove, true);
  document.removeEventListener("pointerup", onPointerUp, true);
  document.removeEventListener("pointercancel", onPointerCancel, true);
  documentListenersBound = false;
}
