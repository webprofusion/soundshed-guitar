import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  AUTO_SCROLL_EDGE_PX,
  POINTER_DRAG_THRESHOLD_PX,
  TOUCH_DRAG_HOLD_MS,
  TOUCH_HELD_CLASS,
  beginPointerDrag,
  cancelPointerDrag,
  isPointerDragActive,
  resetPointerDragForTests,
  type PointerDragGesture,
  type PointerDragTarget,
} from "../ts/pointerDrag";

/** jsdom does not implement PointerEvent, and only the geometry matters here. */
function pointerEvent(type: string, x: number, y: number): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, clientX: x, clientY: y, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  Object.defineProperty(event, "isPrimary", { value: true });
  return event as PointerEvent;
}

type TestTarget = PointerDragTarget;

let source: HTMLElement;
let dropZone: HTMLElement;
let drops: { target: TestTarget | null; gesture: PointerDragGesture }[];

function startDrag(overrides: { resolveTarget?: (event: PointerEvent) => TestTarget | null } = {}): void {
  const event = pointerEvent("pointerdown", 100, 100);
  source.dispatchEvent(event);
  beginPointerDrag<TestTarget>(event, {
    source,
    sourceSelector: ".draggable",
    previewClass: "drag-preview",
    bodyClass: "dragging-active",
    resolveTarget: overrides.resolveTarget ?? (() => ({ element: dropZone })),
    setTargetHighlight: (target, highlighted) => target.element.classList.toggle("drag-over", highlighted),
    onDrop: (target, gesture) => drops.push({ target, gesture }),
  });
}

beforeEach(() => {
  document.body.innerHTML = `
    <div id="source" class="draggable">node</div>
    <div id="drop-zone">zone</div>
  `;
  source = document.getElementById("source") as HTMLElement;
  dropZone = document.getElementById("drop-zone") as HTMLElement;
  drops = [];
});

afterEach(() => {
  resetPointerDragForTests();
  document.body.innerHTML = "";
});

describe("beginPointerDrag", () => {
  it("treats movement under the threshold as a click, not a drag", () => {
    startDrag();
    document.dispatchEvent(pointerEvent("pointermove", 100 + POINTER_DRAG_THRESHOLD_PX - 1, 100));
    document.dispatchEvent(pointerEvent("pointerup", 100 + POINTER_DRAG_THRESHOLD_PX - 1, 100));

    expect(drops).toEqual([]);
    expect(document.querySelector(".drag-preview")).toBeNull();
    expect(isPointerDragActive()).toBe(false);
  });

  it("reports the drop target and total travel once past the threshold", () => {
    startDrag();
    document.dispatchEvent(pointerEvent("pointermove", 160, 130));
    expect(isPointerDragActive()).toBe(true);
    expect(dropZone.classList.contains("drag-over")).toBe(true);
    expect(document.body.classList.contains("dragging-active")).toBe(true);

    document.dispatchEvent(pointerEvent("pointerup", 160, 130));

    expect(drops).toHaveLength(1);
    expect(drops[0].target?.element).toBe(dropZone);
    expect(drops[0].gesture).toEqual({ deltaX: 60, deltaY: 30 });
    expect(dropZone.classList.contains("drag-over")).toBe(false);
    expect(document.body.classList.contains("dragging-active")).toBe(false);
  });

  it("reports a null target when released away from any drop zone", () => {
    startDrag({ resolveTarget: () => null });
    document.dispatchEvent(pointerEvent("pointermove", 100, 180));
    document.dispatchEvent(pointerEvent("pointerup", 100, 180));

    expect(drops).toHaveLength(1);
    expect(drops[0].target).toBeNull();
    expect(drops[0].gesture).toEqual({ deltaX: 0, deltaY: 80 });
  });

  it("shows a preview that follows the pointer and cannot be hit-tested", () => {
    startDrag();
    document.dispatchEvent(pointerEvent("pointermove", 160, 130));

    const preview = document.querySelector(".drag-preview") as HTMLElement;
    expect(preview).not.toBeNull();
    expect(preview.style.left).toBe("160px");
    expect(preview.style.top).toBe("130px");

    document.dispatchEvent(pointerEvent("pointermove", 200, 140));
    expect(preview.style.left).toBe("200px");

    document.dispatchEvent(pointerEvent("pointerup", 200, 140));
    expect(document.querySelector(".drag-preview")).toBeNull();
  });

  it("survives the source being replaced by a re-render mid-drag", () => {
    startDrag();
    document.dispatchEvent(pointerEvent("pointermove", 160, 130));

    // What a backend state broadcast does to the signal path.
    document.body.innerHTML = `<div id="source" class="draggable">node</div><div id="drop-zone">zone</div>`;
    dropZone = document.getElementById("drop-zone") as HTMLElement;

    document.dispatchEvent(pointerEvent("pointerup", 160, 130));

    expect(drops).toHaveLength(1);
    expect(document.querySelector(".drag-preview")).toBeNull();
    expect(document.querySelector(".draggable.dragging")).toBeNull();
    expect(document.body.classList.contains("dragging-active")).toBe(false);
  });

  it("drops nothing when the gesture is cancelled", () => {
    startDrag();
    document.dispatchEvent(pointerEvent("pointermove", 160, 130));
    document.dispatchEvent(pointerEvent("pointercancel", 160, 130));

    expect(drops).toEqual([]);
    expect(document.querySelector(".drag-preview")).toBeNull();
    expect(dropZone.classList.contains("drag-over")).toBe(false);
  });

  it("swallows the click the browser synthesises after a drag", () => {
    const onClick = vi.fn();
    dropZone.addEventListener("click", onClick);

    startDrag();
    document.dispatchEvent(pointerEvent("pointermove", 160, 130));
    document.dispatchEvent(pointerEvent("pointerup", 160, 130));
    dropZone.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    expect(onClick).not.toHaveBeenCalled();

    // Only the one click; the next is a genuine one.
    dropZone.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    expect(onClick).toHaveBeenCalledTimes(1);
  });

  it("leaves clicks alone when the press never became a drag", () => {
    const onClick = vi.fn();
    dropZone.addEventListener("click", onClick);

    startDrag();
    document.dispatchEvent(pointerEvent("pointerup", 100, 100));
    dropZone.dispatchEvent(new MouseEvent("click", { bubbles: true }));

    expect(onClick).toHaveBeenCalledTimes(1);
  });
});

describe("beginPointerDrag with a touch hold", () => {
  function touchEvent(type: string, x: number, y: number): PointerEvent {
    const event = pointerEvent(type, x, y);
    Object.defineProperty(event, "pointerType", { value: "touch" });
    return event;
  }

  function startTouchDrag(press = touchEvent("pointerdown", 100, 100)): void {
    const event = press;
    source.dispatchEvent(event);
    beginPointerDrag<TestTarget>(event, {
      source,
      sourceSelector: ".draggable",
      previewClass: "drag-preview",
      touchHoldMs: TOUCH_DRAG_HOLD_MS,
      resolveTarget: () => ({ element: dropZone }),
      setTargetHighlight: () => undefined,
      onDrop: (target, gesture) => drops.push({ target, gesture }),
    });
  }

  function touchMove(): TouchEvent {
    const event = new Event("touchmove", { bubbles: true, cancelable: true }) as TouchEvent;
    document.dispatchEvent(event);
    return event;
  }

  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it("leaves a swipe that moves before the hold is up to the browser's scrolling", () => {
    startTouchDrag();
    expect(touchMove().defaultPrevented).toBe(false);
    document.dispatchEvent(touchEvent("pointermove", 100, 140));
    vi.advanceTimersByTime(TOUCH_DRAG_HOLD_MS);
    document.dispatchEvent(touchEvent("pointermove", 100, 180));
    document.dispatchEvent(touchEvent("pointerup", 100, 180));

    expect(isPointerDragActive()).toBe(false);
    expect(drops).toEqual([]);
    expect(touchMove().defaultPrevented).toBe(false);
  });

  it("picks the item up once the finger has rested, and keeps the page from scrolling", () => {
    startTouchDrag();
    document.dispatchEvent(touchEvent("pointermove", 102, 101));
    vi.advanceTimersByTime(TOUCH_DRAG_HOLD_MS);
    expect(touchMove().defaultPrevented).toBe(true);

    document.dispatchEvent(touchEvent("pointermove", 100, 160));
    expect(isPointerDragActive()).toBe(true);
    document.dispatchEvent(touchEvent("pointerup", 100, 160));

    expect(drops).toHaveLength(1);
    expect(drops[0].target?.element).toBe(dropZone);
    expect(touchMove().defaultPrevented).toBe(false);
  });

  it("lifts the source once the hold takes, buzzes where it can, and lets it down at the end", () => {
    const vibrate = vi.fn();
    Object.defineProperty(navigator, "vibrate", { value: vibrate, configurable: true });
    try {
      startTouchDrag();
      expect(source.classList.contains(TOUCH_HELD_CLASS)).toBe(false);
      vi.advanceTimersByTime(TOUCH_DRAG_HOLD_MS);
      expect(source.classList.contains(TOUCH_HELD_CLASS)).toBe(true);
      expect(vibrate).toHaveBeenCalledTimes(1);
      document.dispatchEvent(touchEvent("pointerup", 100, 100));
      expect(source.classList.contains(TOUCH_HELD_CLASS)).toBe(false);
    } finally {
      Reflect.deleteProperty(navigator, "vibrate");
    }
  });

  it("does not make a mouse wait", () => {
    startTouchDrag(pointerEvent("pointerdown", 100, 100));
    document.dispatchEvent(pointerEvent("pointermove", 160, 100));
    expect(isPointerDragActive()).toBe(true);
  });

  it("drops nothing when cancelled, as a menu opened by a long press does", () => {
    startTouchDrag();
    vi.advanceTimersByTime(TOUCH_DRAG_HOLD_MS);
    cancelPointerDrag();
    document.dispatchEvent(touchEvent("pointermove", 100, 160));
    document.dispatchEvent(touchEvent("pointerup", 100, 160));

    expect(drops).toEqual([]);
    expect(touchMove().defaultPrevented).toBe(false);
  });
});

describe("beginPointerDrag with autoScroll", () => {
  let list: HTMLElement;

  beforeEach(() => {
    vi.useFakeTimers();
    list = document.createElement("div");
    list.style.overflowY = "auto";
    document.body.appendChild(list);
    // jsdom lays nothing out: a 100 px tall list at the top of the page, with 400 px of rows.
    list.getBoundingClientRect = () => ({ left: 0, right: 200, top: 0, bottom: 100, width: 200, height: 100, x: 0, y: 0, toJSON: () => ({}) });
    Object.defineProperty(list, "clientHeight", { value: 100 });
    Object.defineProperty(list, "scrollHeight", { value: 400 });
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  function startListDrag(resolveTarget: (event: PointerEvent) => TestTarget | null = () => null, autoScroll: HTMLElement = list): void {
    const event = pointerEvent("pointerdown", 100, 50);
    source.dispatchEvent(event);
    beginPointerDrag<TestTarget>(event, {
      source,
      sourceSelector: ".draggable",
      previewClass: "drag-preview",
      autoScroll,
      resolveTarget,
      setTargetHighlight: () => undefined,
      onDrop: (target, gesture) => drops.push({ target, gesture }),
    });
  }

  it("scrolls the list while the pointer rests near its bottom edge, and stops when it leaves", () => {
    startListDrag();
    document.dispatchEvent(pointerEvent("pointermove", 100, 70));
    vi.advanceTimersByTime(100);
    expect(list.scrollTop).toBe(0);

    document.dispatchEvent(pointerEvent("pointermove", 100, 100 - AUTO_SCROLL_EDGE_PX / 2));
    vi.advanceTimersByTime(100);
    const scrolled = list.scrollTop;
    expect(scrolled).toBeGreaterThan(0);

    document.dispatchEvent(pointerEvent("pointermove", 100, 50));
    vi.advanceTimersByTime(100);
    expect(list.scrollTop).toBe(scrolled);
  });

  it("scrolls back up from the top edge, and finds the target again as rows pass under the pointer", () => {
    list.scrollTop = 200;
    const seen: number[] = [];
    startListDrag(() => {
      seen.push(list.scrollTop);
      return { element: dropZone };
    });
    document.dispatchEvent(pointerEvent("pointermove", 100, 2));
    vi.advanceTimersByTime(200);

    expect(list.scrollTop).toBeLessThan(200);
    expect(new Set(seen).size).toBeGreaterThan(2);
  });

  it("scrolls the nearest scrolling ancestor of a list that does not scroll itself", () => {
    const rows = document.createElement("div");
    list.appendChild(rows);
    startListDrag(undefined, rows);
    document.dispatchEvent(pointerEvent("pointermove", 100, 98));
    vi.advanceTimersByTime(100);
    expect(list.scrollTop).toBeGreaterThan(0);
  });

  it("leaves the list alone when the pointer is beside it, and once the drag ends", () => {
    startListDrag();
    document.dispatchEvent(pointerEvent("pointermove", 300, 98));
    vi.advanceTimersByTime(100);
    expect(list.scrollTop).toBe(0);

    document.dispatchEvent(pointerEvent("pointermove", 100, 98));
    document.dispatchEvent(pointerEvent("pointerup", 100, 98));
    vi.advanceTimersByTime(100);
    expect(list.scrollTop).toBe(0);
  });
});
