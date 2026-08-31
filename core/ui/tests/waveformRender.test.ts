import { beforeEach, describe, expect, it } from "vitest";
import { WAVEFORM_COLORS, drawWaveform, type WaveformSpec } from "../ts/waveform/render.js";

/**
 * jsdom has no 2D context, so the canvas records what was asked of it. That is
 * the right level for this anyway: the interesting properties are *what* gets
 * drawn and *in what order* (a shade has to land before the trace, a tint
 * after), which a pixel comparison would express far less directly.
 */
type Op =
  | { op: "fillRect"; x: number; w: number; style: string }
  | { op: "fillText"; text: string; x: number; align: string }
  | { op: "stroke"; style: string; dash: number[]; lines: number[]; arcs: number[] }
  | { op: "fill"; style: string; arcs: number[] };

const WIDTH = 1000;
const HEIGHT = 200;

let ops: Op[];
let canvas: HTMLCanvasElement;

function makeCanvas(): HTMLCanvasElement {
  const el = document.createElement("canvas");
  Object.defineProperty(el, "clientWidth", { value: WIDTH });
  Object.defineProperty(el, "clientHeight", { value: HEIGHT });

  let lines: number[] = [];
  let arcs: number[] = [];

  const ctx = {
    fillStyle: "",
    strokeStyle: "",
    lineWidth: 0,
    font: "",
    textAlign: "left",
    dash: [] as number[],
    setTransform: () => {},
    clearRect: () => {},
    fillRect: (x: number, _y: number, w: number) => ops.push({ op: "fillRect", x, w, style: ctx.fillStyle }),
    fillText: (text: string, x: number) => ops.push({ op: "fillText", text, x, align: ctx.textAlign }),
    beginPath: () => {
      lines = [];
      arcs = [];
    },
    moveTo: (x: number) => lines.push(x),
    lineTo: (x: number) => lines.push(x),
    arc: (x: number) => arcs.push(x),
    setLineDash: (d: number[]) => {
      ctx.dash = d;
    },
    stroke: () => ops.push({ op: "stroke", style: ctx.strokeStyle, dash: [...ctx.dash], lines: [...lines], arcs: [...arcs] }),
    fill: () => ops.push({ op: "fill", style: ctx.fillStyle, arcs: [...arcs] }),
  };

  (el as unknown as { getContext: () => unknown }).getContext = () => ctx;
  return el;
}

/** Index of the first stroke that looks like a peak trace (many segments). */
function traceIndex(): number {
  return ops.findIndex((o) => o.op === "stroke" && o.lines.length > 20);
}

function draw(spec: WaveformSpec): void {
  ops = [];
  drawWaveform(canvas, spec);
}

const peaks = Array.from({ length: 64 }, (_, i) => (i % 8) / 8);

beforeEach(() => {
  ops = [];
  canvas = makeCanvas();
});

describe("empty state", () => {
  it("draws the message and no trace when there are no lanes", () => {
    draw({ lanes: [], empty: { text: "No capture yet" } });

    const text = ops.find((o) => o.op === "fillText");
    expect(text).toMatchObject({ text: "No capture yet", align: "left" });
    expect(traceIndex()).toBe(-1);
  });

  it("centres the message when asked", () => {
    draw({ lanes: [], empty: { text: "Drop a file", align: "center" } });
    expect(ops.find((o) => o.op === "fillText")).toMatchObject({ align: "center", x: WIDTH / 2 });
  });

  it("treats an empty lane array the same as no lanes", () => {
    draw({ lanes: [[]], empty: { text: "nothing" } });
    expect(traceIndex()).toBe(-1);
  });
});

describe("lanes", () => {
  it("draws one trace for mono and no per-lane centre lines", () => {
    draw({ lanes: [peaks] });

    const traces = ops.filter((o) => o.op === "stroke" && o.lines.length > 20);
    expect(traces).toHaveLength(1);
    // Only the L/R divider is stroked as a plain 2-point horizontal line.
    const rules = ops.filter((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.laneCenter);
    expect(rules).toHaveLength(0);
  });

  it("draws two traces plus per-lane centre lines for stereo", () => {
    draw({ lanes: [peaks, peaks] });

    const traces = ops.filter((o) => o.op === "stroke" && o.lines.length > 20);
    expect(traces).toHaveLength(2);
    expect(ops.filter((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.laneCenter)).toHaveLength(1);
  });

  it("stops the trace at traceLimitRatio", () => {
    draw({ lanes: [peaks], traceLimitRatio: 0.5 });

    const trace = ops[traceIndex()];
    expect(trace.op === "stroke" && Math.max(...trace.lines)).toBeLessThanOrEqual(WIDTH / 2);
  });
});

describe("range emphasis", () => {
  const range = {
    startRatio: 0.25,
    endRatio: 0.75,
    selectedHandle: "start" as const,
    color: WAVEFORM_COLORS.rangeActive,
  };

  it("shades outside the range BEFORE the trace, so excluded peaks stay legible", () => {
    draw({ lanes: [peaks], range: { ...range, emphasis: "shade" } });

    const shades = ops
      .map((o, i) => ({ o, i }))
      .filter(({ o }) => o.op === "fillRect" && o.style === WAVEFORM_COLORS.shade);
    expect(shades).toHaveLength(2); // left of start, right of end
    expect(Math.max(...shades.map((s) => s.i))).toBeLessThan(traceIndex());
  });

  it("tints the range AFTER the trace, so it colours what it covers", () => {
    draw({ lanes: [peaks], range: { ...range, emphasis: "tint" } });

    const tint = ops.findIndex((o) => o.op === "fillRect" && o.style.startsWith("rgba(255, 204, 102"));
    expect(tint).toBeGreaterThan(traceIndex());
  });

  it("spans exactly the selected range when tinting", () => {
    draw({ lanes: [peaks], range: { ...range, emphasis: "tint" } });

    const tint = ops.find((o) => o.op === "fillRect" && o.style.startsWith("rgba(255, 204, 102"));
    expect(tint).toMatchObject({ x: 250, w: 500 });
  });

  it("draws no range furniture when there is no range", () => {
    draw({ lanes: [peaks] });
    expect(ops.filter((o) => o.op === "fill" && o.arcs.length > 0)).toHaveLength(0);
  });
});

describe("handles", () => {
  const base = { lanes: [peaks], range: { startRatio: 0.25, endRatio: 0.75, color: WAVEFORM_COLORS.rangeActive, emphasis: "tint" as const } };

  it("draws both handles and rings the selected one", () => {
    draw({ ...base, range: { ...base.range, selectedHandle: "start" } });

    const handles = ops.find((o) => o.op === "fill" && o.arcs.length === 2);
    expect(handles?.op === "fill" && handles.arcs).toEqual([250, 750]);

    const ring = ops.find((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.selectedHandleRing);
    expect(ring?.op === "stroke" && ring.arcs).toEqual([250]);
  });

  it("moves the ring to the end handle when that is the selected one", () => {
    draw({ ...base, range: { ...base.range, selectedHandle: "end" } });

    const ring = ops.find((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.selectedHandleRing);
    expect(ring?.op === "stroke" && ring.arcs).toEqual([750]);
  });

  it("dashes the boundary for an uncommitted range and not otherwise", () => {
    draw({ ...base, range: { ...base.range, selectedHandle: "start", dashed: true } });
    const dashed = ops.find((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.rangeActive && o.lines.length === 4);
    expect(dashed?.op === "stroke" && dashed.dash).toEqual([4, 3]);

    draw({ ...base, range: { ...base.range, selectedHandle: "start" } });
    const solid = ops.find((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.rangeActive && o.lines.length === 4);
    expect(solid?.op === "stroke" && solid.dash).toEqual([]);
  });
});

describe("playhead and recording overlay", () => {
  it("draws the playhead at its ratio", () => {
    draw({ lanes: [peaks], playhead: { ratio: 0.4 } });

    const head = ops.find((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.playhead);
    expect(head?.op === "stroke" && head.lines).toEqual([400, 400]);
  });

  it("shades the un-recorded tail and heads it in the recording colour", () => {
    draw({
      lanes: [peaks],
      traceColor: WAVEFORM_COLORS.recordingTrace,
      traceLimitRatio: 0.3,
      shadeAfterRatio: 0.3,
      playhead: { ratio: 0.3, color: WAVEFORM_COLORS.recordingHead },
    });

    const shade = ops.find((o) => o.op === "fillRect" && o.style === WAVEFORM_COLORS.unrecordedShade);
    expect(shade).toMatchObject({ x: 300, w: 700 });

    expect(ops[traceIndex()]).toMatchObject({ style: WAVEFORM_COLORS.recordingTrace });
    expect(ops.some((o) => o.op === "stroke" && o.style === WAVEFORM_COLORS.recordingHead)).toBe(true);
  });
});

describe("backing store", () => {
  it("sizes the canvas to the device pixel ratio", () => {
    const original = window.devicePixelRatio;
    Object.defineProperty(window, "devicePixelRatio", { value: 2, configurable: true });
    draw({ lanes: [peaks] });
    expect(canvas.width).toBe(WIDTH * 2);
    expect(canvas.height).toBe(HEIGHT * 2);
    Object.defineProperty(window, "devicePixelRatio", { value: original, configurable: true });
  });
});
