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
 *
 * A source in a scrolling list sets `touchHoldMs`: on a touch screen the finger
 * then has to rest that long before it picks the item up, and a swipe sooner is
 * left to the browser, so the list still scrolls. That is the only way an iPhone
 * drags these: its WKWebView has HTML5 drag-and-drop switched off. Picked up, the
 * source wears {@link TOUCH_HELD_CLASS} until the gesture ends (the signal chain's
 * node lifts) and the device gives a short buzz where it can (Android), so the
 * finger knows the hold took before it moves.
 */

/** Movement required before a press turns into a drag rather than a click. */
export const POINTER_DRAG_THRESHOLD_PX = 6;

/**
 * The `touchHoldMs` of a source in a scrolling list: long enough that a swipe
 * scrolls, short of the long press that opens a menu (touchLongPress.ts).
 */
export const TOUCH_DRAG_HOLD_MS = 250;

/** On the source from the moment a touch hold picks it up until the gesture ends. */
export const TOUCH_HELD_CLASS = "touch-held";

/** The buzz that says a touch hold took, in milliseconds (where `navigator.vibrate` exists). */
const TOUCH_HELD_VIBRATE_MS = 12;

/** How close to an `autoScroll` list's top or bottom edge the pointer scrolls it. */
export const AUTO_SCROLL_EDGE_PX = 28;

/** The fastest an `autoScroll` list scrolls, in pixels a frame, with the pointer at its edge. */
const AUTO_SCROLL_MAX_STEP_PX = 12;

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
  /**
   * For a touch press, how long it has to stay within the movement threshold
   * before it can turn into a drag. Moving sooner gives the gesture up, to the
   * browser's scrolling. Unset, a touch drags at once, which suits a source that
   * opts out of panning with `touch-action`. Mouse and pen never wait.
   */
  touchHoldMs?: number;
  /**
   * A list scrolled while the pointer is held near its top or bottom edge, as a
   * native drag scrolls one, so a target out of view can still be reached. The
   * element itself or, when it does not scroll, its nearest ancestor that does.
   */
  autoScroll?: HTMLElement | null;
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
  /** False while a touch press is still waiting out `touchHoldMs`. */
  held: boolean;
  /** A touch hold has picked the item up: the page must not scroll under the drag. */
  blockTouchScroll: boolean;
  holdTimer: number | null;
  /** The latest move, replayed to find the target again after an auto-scroll. */
  lastEvent: PointerEvent | null;
  /** What `autoScroll` resolved to, found when the drag starts. */
  scroller: HTMLElement | null;
  scrollFrame: number | null;
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
export function swallowNextClick(): void {
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
  if (active.holdTimer !== null) {
    window.clearTimeout(active.holdTimer);
  }
  if (active.scrollFrame !== null) {
    window.cancelAnimationFrame(active.scrollFrame);
  }

  const { spec } = active;
  const target = active.target;
  clearTarget(active);
  active.preview?.remove();
  spec.source.classList.remove("dragging", TOUCH_HELD_CLASS);
  // A re-render during the drag leaves a fresh element wearing the classes.
  document.querySelectorAll(`${spec.sourceSelector}.dragging, ${spec.sourceSelector}.${TOUCH_HELD_CLASS}`)
    .forEach((element) => element.classList.remove("dragging", TOUCH_HELD_CLASS));
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
    if (!active.held) {
      // Moved before the hold was up: a scroll, not a drag.
      endSession(true);
      return;
    }
    active.active = true;
    active.spec.source.classList.add("dragging");
    if (active.spec.bodyClass) {
      document.body.classList.add(active.spec.bodyClass);
    }
    active.preview = createPreview(active, event);
    active.scroller = findScroller(active.spec.autoScroll ?? null);
  }

  event.preventDefault();
  if (active.preview) {
    positionPreview(active.preview, event);
  }

  active.lastEvent = event;
  retarget(active, event);
  if (active.scrollFrame === null && autoScrollStep(active) !== 0) {
    active.scrollFrame = window.requestAnimationFrame(() => runAutoScroll(active));
  }
}

function retarget(active: PointerDragSession<PointerDragTarget>, event: PointerEvent): void {
  const next = active.spec.resolveTarget(event);
  if (active.target?.element === next?.element) return;
  clearTarget(active);
  active.target = next;
  if (next) {
    active.spec.setTargetHighlight(next, true);
  }
}

function findScroller(element: HTMLElement | null): HTMLElement | null {
  for (let current = element; current; current = current.parentElement) {
    const { overflowY } = window.getComputedStyle(current);
    if ((overflowY === "auto" || overflowY === "scroll") && current.scrollHeight > current.clientHeight) {
      return current;
    }
  }
  return null;
}

/** How far the `autoScroll` list should move this frame: negative is up, 0 is not at all. */
function autoScrollStep(active: PointerDragSession<PointerDragTarget>): number {
  const list = active.scroller;
  if (!list?.isConnected) return 0;
  const rect = list.getBoundingClientRect();
  if (active.lastX < rect.left || active.lastX > rect.right) return 0;
  // A list taller than the window has its edge zones where the pointer can reach them.
  const top = Math.max(rect.top, 0);
  const bottom = Math.min(rect.bottom, window.innerHeight);
  // Eased, so the zone's inner part, often over the first or last row in view, barely moves
  // the list, and only the edge itself runs it at full speed.
  const depth = (edgeDistance: number): number =>
    Math.round(AUTO_SCROLL_MAX_STEP_PX * Math.max(0, 1 - edgeDistance / AUTO_SCROLL_EDGE_PX) ** 2);
  if (active.lastY < top + AUTO_SCROLL_EDGE_PX && list.scrollTop > 0) {
    return -depth(Math.max(0, active.lastY - top));
  }
  if (active.lastY > bottom - AUTO_SCROLL_EDGE_PX && list.scrollTop + list.clientHeight < list.scrollHeight) {
    return depth(Math.max(0, bottom - active.lastY));
  }
  return 0;
}

/** Scrolls a frame at a time while the pointer rests in an edge zone, re-finding the target under it. */
function runAutoScroll(active: PointerDragSession<PointerDragTarget>): void {
  active.scrollFrame = null;
  const step = autoScrollStep(active);
  if (session !== active || step === 0 || !active.scroller) return;
  active.scroller.scrollTop += step;
  if (active.lastEvent) {
    retarget(active, active.lastEvent);
  }
  active.scrollFrame = window.requestAnimationFrame(() => runAutoScroll(active));
}

function onPointerUp(event: PointerEvent): void {
  if (session?.pointerId !== event.pointerId) return;
  endSession(false);
}

function onPointerCancel(event: PointerEvent): void {
  if (session?.pointerId !== event.pointerId) return;
  endSession(true);
}

/**
 * Once a touch hold has picked an item up, the finger's movement is the drag's.
 * Left alone, the browser would scroll the list and cancel the pointer stream.
 */
function onTouchMove(event: TouchEvent): void {
  if (session?.blockTouchScroll && event.cancelable) {
    event.preventDefault();
  }
}

function bindDocumentListeners(): void {
  if (documentListenersBound) return;
  document.addEventListener("pointermove", onPointerMove, true);
  document.addEventListener("pointerup", onPointerUp, true);
  document.addEventListener("pointercancel", onPointerCancel, true);
  // Not passive, or preventDefault() could not stop the scroll.
  document.addEventListener("touchmove", onTouchMove, { capture: true, passive: false });
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

  const waitForHold = event.pointerType === "touch" && (spec.touchHoldMs ?? 0) > 0;
  const started: PointerDragSession<PointerDragTarget> = {
    spec: spec as unknown as PointerDragSpec<PointerDragTarget>,
    pointerId: event.pointerId,
    startX: event.clientX,
    startY: event.clientY,
    lastX: event.clientX,
    lastY: event.clientY,
    active: false,
    held: !waitForHold,
    blockTouchScroll: false,
    holdTimer: null,
    lastEvent: null,
    scroller: null,
    scrollFrame: null,
    preview: null,
    target: null,
  };
  if (waitForHold) {
    started.holdTimer = window.setTimeout(() => {
      started.holdTimer = null;
      started.held = true;
      started.blockTouchScroll = true;
      spec.source.classList.add(TOUCH_HELD_CLASS);
      try {
        navigator.vibrate?.(TOUCH_HELD_VIBRATE_MS);
      } catch {
        // A WebView may refuse it; the lift is the cue that matters.
      }
    }, spec.touchHoldMs);
  }
  session = started;
  // Capture keeps events flowing to us if the pointer leaves the source; when a
  // re-render detaches the source the capture is released implicitly and the
  // document listeners above keep the drag alive regardless.
  spec.source.setPointerCapture?.(event.pointerId);
}

/**
 * Give up the press being tracked, if any, without a drop: a long press that
 * opened a menu (touchLongPress.ts) has taken the gesture over.
 */
export function cancelPointerDrag(): void {
  endSession(true);
}

/** Test hook: drop any in-flight drag and its listeners. */
export function resetPointerDragForTests(): void {
  if (session) endSession(true);
  document.removeEventListener("pointermove", onPointerMove, true);
  document.removeEventListener("pointerup", onPointerUp, true);
  document.removeEventListener("pointercancel", onPointerCancel, true);
  document.removeEventListener("touchmove", onTouchMove, true);
  documentListenersBound = false;
}
