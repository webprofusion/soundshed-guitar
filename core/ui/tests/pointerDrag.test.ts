import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  POINTER_DRAG_THRESHOLD_PX,
  beginPointerDrag,
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
