import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  CLICK_DRAG_THRESHOLD_PX,
  CURSOR_HOLD_MS,
  HANDLE_HIT_PX,
  RESIZING_CLASS,
  bindRangeSelect,
  type RangeSelectController,
  type RangeSelectGesture,
  type RangeSelectSpec,
} from "../ts/waveform/rangeSelect.js";
import type { RangeHandle, RatioRange } from "../ts/waveform/range.js";

/** jsdom implements neither PointerEvent nor layout; only geometry matters. */
function pointerEvent(type: string, x: number): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, clientX: x, clientY: 0, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  Object.defineProperty(event, "isPrimary", { value: true });
  return event as PointerEvent;
}

const CANVAS_WIDTH = 1000;
const DURATION_SEC = 100;

/** x pixel for a 0..1 ratio, given the stubbed canvas width. */
const px = (ratio: number) => ratio * CANVAS_WIDTH;

let canvas: HTMLCanvasElement;
let controller: RangeSelectController;
let range: RatioRange | null;
let enabled: boolean;
let seeks: number[];
let creates: RatioRange[];
let commits: RangeSelectGesture[];
let renders: number;
let createStarts: number;

function setup(overrides: Partial<RangeSelectSpec> = {}): void {
  controller = bindRangeSelect({
    canvas,
    isEnabled: () => enabled,
    getRange: () => range,
    getMinSpanRatio: () => 0.01,
    getDurationSec: () => DURATION_SEC,
    nudgeStepSec: { fine: 1, coarse: 10 },
    onResize: (next) => {
      range = next;
    },
    onCreate: (next) => {
      range = next;
      creates.push(next);
    },
    onCreateStart: () => {
      createStarts += 1;
    },
    onSeek: (ratio) => seeks.push(ratio),
    onCommit: (gesture) => commits.push(gesture),
    onSelectedHandleChange: () => {},
    render: () => {
      renders += 1;
    },
    ...overrides,
  });
}

function press(x: number): void {
  canvas.dispatchEvent(pointerEvent("pointerdown", x));
}
function move(x: number): void {
  canvas.dispatchEvent(pointerEvent("pointermove", x));
}
function release(x: number): void {
  canvas.dispatchEvent(pointerEvent("pointerup", x));
}
function key(k: string, shiftKey = false): void {
  canvas.dispatchEvent(new KeyboardEvent("keydown", { key: k, shiftKey, bubbles: true }));
}

beforeEach(() => {
  vi.useFakeTimers();
  document.body.innerHTML = `<canvas id="wave" tabindex="0"></canvas>`;
  canvas = document.getElementById("wave") as HTMLCanvasElement;
  canvas.getBoundingClientRect = () =>
    ({ left: 0, top: 0, width: CANVAS_WIDTH, height: 100, right: CANVAS_WIDTH, bottom: 100, x: 0, y: 0, toJSON: () => ({}) });
  range = { startRatio: 0.3, endRatio: 0.6 };
  enabled = true;
  seeks = [];
  creates = [];
  commits = [];
  renders = 0;
  createStarts = 0;
});

afterEach(() => {
  controller?.destroy();
  vi.useRealTimers();
  document.body.innerHTML = "";
});

describe("handle grabbing", () => {
  it("grabs a handle when the press lands within the hit radius", () => {
    setup();
    press(px(0.3) + HANDLE_HIT_PX - 1);
    move(px(0.4));
    release(px(0.4));

    expect(range).toEqual({ startRatio: 0.4, endRatio: 0.6 });
    expect(commits).toEqual(["resize"]);
    expect(seeks).toEqual([]);
  });

  it("does not grab a handle when the press lands outside the hit radius", () => {
    setup();
    press(px(0.3) + HANDLE_HIT_PX + 5);
    move(px(0.4));
    release(px(0.4));

    // Became a create sweep from the press point, not a yank of the nearest edge.
    expect(range?.startRatio).toBeCloseTo(0.315);
    expect(commits).toEqual(["create"]);
  });

  it("retargets the steered handle when the pointer crosses the other one", () => {
    setup();
    press(px(0.6)); // grab the end handle
    move(px(0.2));
    move(px(0.1));
    release(px(0.1));

    // Anchored on 0.3 and grown leftwards, not collapsed into a sliding sliver.
    expect(range?.startRatio).toBeCloseTo(0.1);
    expect(range?.endRatio).toBeCloseTo(0.3);
    expect(controller.getSelectedHandle()).toBe("start");
  });
});

describe("click versus drag", () => {
  it("treats a press with no meaningful movement as a seek", () => {
    setup();
    press(px(0.8));
    move(px(0.8) + CLICK_DRAG_THRESHOLD_PX - 1);
    release(px(0.8));

    expect(seeks).toEqual([0.8]);
    expect(creates).toEqual([]);
    expect(commits).toEqual([]);
  });

  it("promotes a press past the threshold into a create sweep", () => {
    setup();
    press(px(0.8));
    move(px(0.8) + CLICK_DRAG_THRESHOLD_PX + 1);
    release(px(0.9));

    expect(createStarts).toBe(1);
    expect(seeks).toEqual([]);
    expect(commits).toEqual(["create"]);
  });

  it("clamps a create sweep to the minimum span", () => {
    setup();
    press(px(0.5));
    move(px(0.5) + CLICK_DRAG_THRESHOLD_PX + 1);
    release(px(0.5) + CLICK_DRAG_THRESHOLD_PX + 1);

    const swept = creates[creates.length - 1];
    expect(swept.endRatio - swept.startRatio).toBeCloseTo(0.01);
  });

  it("does nothing on an empty-space drag when the host opts out of create", () => {
    setup({ onCreate: undefined });
    const before = { ...range! };
    press(px(0.8));
    move(px(0.95));
    release(px(0.95));

    expect(range).toEqual(before);
    expect(commits).toEqual([]);
  });
});

describe("keyboard", () => {
  it("steps by seconds, not by a fraction of the material", () => {
    setup();
    press(px(0.3)); // select the start handle
    release(px(0.3));
    key("ArrowRight");

    // 1s of a 100s track is 0.01 of its width.
    expect(range?.startRatio).toBeCloseTo(0.31);
  });

  it("uses the coarse step with Shift held", () => {
    setup();
    press(px(0.3));
    release(px(0.3));
    key("ArrowRight", true);

    expect(range?.startRatio).toBeCloseTo(0.4);
  });

  it("swaps the steered handle on ArrowUp/ArrowDown", () => {
    setup();
    expect(controller.getSelectedHandle()).toBe("start");
    key("ArrowUp");
    expect(controller.getSelectedHandle()).toBe("end");
    key("ArrowDown");
    expect(controller.getSelectedHandle()).toBe("start");
  });

  it("sends the steered handle to the extremes with Home and End", () => {
    setup();
    key("Home");
    expect(range?.startRatio).toBe(0);

    key("ArrowUp"); // steer the end handle
    key("End");
    expect(range?.endRatio).toBe(1);
  });

  it("collapses a burst of key repeats into one commit", () => {
    setup();
    key("ArrowRight");
    key("ArrowRight");
    key("ArrowRight");
    expect(commits).toEqual([]); // nothing written yet

    vi.runAllTimers();
    expect(commits).toEqual(["keyboard"]);
  });
});

describe("cursor affordance", () => {
  it("shows the resize cursor only once a press is held", () => {
    setup();
    press(px(0.3));
    expect(canvas.classList.contains(RESIZING_CLASS)).toBe(false);

    vi.advanceTimersByTime(CURSOR_HOLD_MS);
    expect(canvas.classList.contains(RESIZING_CLASS)).toBe(true);

    release(px(0.3));
    expect(canvas.classList.contains(RESIZING_CLASS)).toBe(false);
  });

  it("never flashes it for a quick click", () => {
    setup();
    press(px(0.8));
    vi.advanceTimersByTime(CURSOR_HOLD_MS - 1);
    release(px(0.8));
    vi.runAllTimers();

    expect(canvas.classList.contains(RESIZING_CLASS)).toBe(false);
  });
});

describe("guards", () => {
  it("ignores every gesture while disabled", () => {
    setup();
    enabled = false;
    const before = { ...range! };
    press(px(0.3));
    move(px(0.5));
    release(px(0.5));
    key("ArrowRight");

    expect(range).toEqual(before);
    expect(seeks).toEqual([]);
    expect(commits).toEqual([]);
  });

  it("abandons a drag on pointercancel without committing", () => {
    setup();
    press(px(0.3));
    move(px(0.4));
    canvas.dispatchEvent(pointerEvent("pointercancel", px(0.4)));

    expect(commits).toEqual([]);
    expect(seeks).toEqual([]);
  });

  it("stops responding after destroy", () => {
    setup();
    controller.destroy();
    const before = { ...range! };
    press(px(0.3));
    move(px(0.5));
    release(px(0.5));

    expect(range).toEqual(before);
  });
});
