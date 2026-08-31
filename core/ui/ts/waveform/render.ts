/**
 * Shared waveform canvas renderer.
 *
 * The Practice Tool's loop editor and the Riff Capture crop editor draw the
 * same picture — a peak trace, a selected range with two handles, a playhead —
 * and had two copies of the code to do it. The copies agreed on almost every
 * colour and measurement, which is the tell: they were the same drawing, forked.
 *
 * What is genuinely different between the two stays as options rather than
 * being flattened away:
 *
 *   - **Lanes.** A backing track is really stereo and gets two stacked lanes; a
 *     captured riff is drawn as one. `lanes.length` decides.
 *   - **Emphasis.** A loop region *tints what it includes*; a crop range
 *     *darkens what it will discard*. Those say different things, so both are
 *     kept — and they sit on opposite sides of the trace accordingly: a shade
 *     goes down before the trace (the peaks it excludes still read clearly), a
 *     tint goes over it (it is colouring what it covers).
 *
 * Colours live here as one palette so the two editors cannot drift apart on
 * them again. They are still literals rather than theme variables, matching
 * what both editors did before; `eqCurve.ts` shows the pattern to follow when
 * these are eventually themed.
 */

import type { RangeHandle } from "./range.js";

export const WAVEFORM_COLORS = {
  background: "rgba(255,255,255,0.06)",
  laneDivider: "rgba(255,255,255,0.22)",
  laneCenter: "rgba(255,255,255,0.10)",
  emptyText: "rgba(255,255,255,0.55)",
  trace: "rgba(101, 186, 255, 0.95)",
  recordingTrace: "rgba(255, 100, 80, 0.95)",
  recordingHead: "rgba(255, 80, 80, 0.9)",
  /** A committed range: the active loop, or the crop the user will apply. */
  rangeActive: "rgba(255, 204, 102, 0.95)",
  /** A range swept but not yet committed to anything. */
  rangeCandidate: "rgba(101, 186, 255, 0.95)",
  selectedHandleRing: "rgba(255,255,255,0.95)",
  playhead: "rgba(255, 255, 255, 0.85)",
  shade: "rgba(0,0,0,0.20)",
  unrecordedShade: "rgba(0,0,0,0.35)",
} as const;

/** Opacity applied to a range colour when tinting the area it covers. */
const TINT_ALPHA = 0.16;

const HANDLE_RADIUS = 4;
const SELECTED_RING_RADIUS = 6;
const LANE_PADDING = 4;

export interface WaveformRangeOverlay {
  startRatio: number;
  endRatio: number;
  /** Which handle wears the focus ring. */
  selectedHandle: RangeHandle;
  color: string;
  /** `tint` colours the range; `shade` darkens everything outside it. */
  emphasis: "tint" | "shade";
  /** Dashes the boundary lines — for a range not yet committed to anything. */
  dashed?: boolean;
}

export interface WaveformSpec {
  /** One entry draws a single centred lane; two stack L over R. */
  lanes: readonly (readonly number[])[];
  traceColor?: string;
  /**
   * Draw peaks only up to this ratio of the width. For a recording in
   * progress, where the rest of the canvas is buffer that has not happened yet.
   */
  traceLimitRatio?: number;
  /** Darken everything right of this ratio — the not-yet-recorded tail. */
  shadeAfterRatio?: number;
  range?: WaveformRangeOverlay | null;
  playhead?: { ratio: number; color?: string } | null;
  /** Shown instead of a trace when there is nothing to draw. */
  empty?: { text: string; align?: "left" | "center" };
}

/** Applies `alpha` to an `rgba(r, g, b, a)` string, leaving other formats be. */
function withAlpha(color: string, alpha: number): string {
  const match = /^rgba?\(([^)]+)\)$/.exec(color.trim());

  if (!match) {
    return color;
  }

  const parts = match[1].split(",").map((part) => part.trim());

  if (parts.length < 3) {
    return color;
  }

  return `rgba(${parts[0]}, ${parts[1]}, ${parts[2]}, ${alpha})`;
}

export function drawWaveform(canvas: HTMLCanvasElement, spec: WaveformSpec): void {
  // Size the backing store to the device pixel ratio each draw: the canvas can
  // be laid out at any width, and a stale backing store renders blurry.
  const dpr = window.devicePixelRatio || 1;
  const width = Math.max(1, Math.floor(canvas.clientWidth));
  const height = Math.max(1, Math.floor(canvas.clientHeight));
  canvas.width = Math.floor(width * dpr);
  canvas.height = Math.floor(height * dpr);

  const ctx = canvas.getContext("2d");

  if (!ctx) {
    return;
  }

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);

  ctx.fillStyle = WAVEFORM_COLORS.background;
  ctx.fillRect(0, 0, width, height);

  const midY = Math.floor(height / 2);
  ctx.strokeStyle = WAVEFORM_COLORS.laneDivider;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, midY);
  ctx.lineTo(width, midY);
  ctx.stroke();

  const lanes = spec.lanes.filter((lane) => lane.length > 0);

  if (lanes.length === 0) {
    if (spec.empty) {
      const centered = spec.empty.align === "center";
      ctx.fillStyle = WAVEFORM_COLORS.emptyText;
      ctx.font = "12px sans-serif";
      ctx.textAlign = centered ? "center" : "left";
      ctx.fillText(spec.empty.text, centered ? width / 2 : 10, centered ? midY : midY - 8);
      ctx.textAlign = "left";
    }

    return;
  }

  const laneHeight = height / lanes.length;
  const maxAmp = laneHeight / 2 - LANE_PADDING;
  const laneCenterY = (index: number) => laneHeight * index + laneHeight / 2;

  // Per-lane reference lines only make sense once a lane is not the whole
  // canvas — with one lane the divider above already sits on its centre.
  if (lanes.length > 1) {
    ctx.strokeStyle = WAVEFORM_COLORS.laneCenter;
    ctx.lineWidth = 1;
    ctx.beginPath();
    lanes.forEach((_, index) => {
      const y = Math.floor(laneCenterY(index));
      ctx.moveTo(0, y);
      ctx.lineTo(width, y);
    });
    ctx.stroke();
  }

  const toX = (ratio: number) => Math.max(0, Math.min(width - 1, ratio * width));

  // A shade says "this is not part of the selection", so it goes down before
  // the trace — the excluded peaks still read clearly on top of it.
  if (spec.range && spec.range.emphasis === "shade") {
    const startX = toX(spec.range.startRatio);
    const endX = toX(spec.range.endRatio);
    ctx.fillStyle = WAVEFORM_COLORS.shade;

    if (startX > 0) {
      ctx.fillRect(0, 0, startX, height);
    }

    if (endX < width) {
      ctx.fillRect(endX, 0, width - endX, height);
    }
  }

  if (spec.shadeAfterRatio !== undefined && spec.shadeAfterRatio < 1) {
    const fromX = Math.max(0, Math.min(width, spec.shadeAfterRatio * width));
    ctx.fillStyle = WAVEFORM_COLORS.unrecordedShade;
    ctx.fillRect(fromX, 0, width - fromX, height);
  }

  const traceLimitX = spec.traceLimitRatio === undefined ? width : Math.max(0, Math.min(width, spec.traceLimitRatio * width));

  lanes.forEach((peaks, index) => {
    const step = width / peaks.length;
    const centerY = laneCenterY(index);
    ctx.strokeStyle = spec.traceColor ?? WAVEFORM_COLORS.trace;
    ctx.lineWidth = Math.max(1, step * 0.7);
    ctx.beginPath();

    peaks.forEach((peak, peakIndex) => {
      const x = peakIndex * step + step / 2;

      if (x > traceLimitX) {
        return;
      }

      const amp = Math.max(1, Math.min(maxAmp, peak * maxAmp));
      ctx.moveTo(x, centerY - amp);
      ctx.lineTo(x, centerY + amp);
    });

    ctx.stroke();
  });

  if (spec.range) {
    const { startRatio, endRatio, color, emphasis, dashed, selectedHandle } = spec.range;
    const startX = toX(startRatio);
    const endX = toX(endRatio);

    // A tint colours what it covers, so it goes over the trace.
    if (emphasis === "tint") {
      ctx.fillStyle = withAlpha(color, TINT_ALPHA);
      ctx.fillRect(startX, 0, Math.max(1, endX - startX), height);
    }

    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.setLineDash(dashed ? [4, 3] : []);
    ctx.beginPath();
    ctx.moveTo(startX, 0);
    ctx.lineTo(startX, height);
    ctx.moveTo(endX, 0);
    ctx.lineTo(endX, height);
    ctx.stroke();
    ctx.setLineDash([]);

    // Handles sit on the divider, so with two lanes they span both.
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(startX, midY, HANDLE_RADIUS, 0, Math.PI * 2);
    ctx.arc(endX, midY, HANDLE_RADIUS, 0, Math.PI * 2);
    ctx.fill();

    ctx.strokeStyle = WAVEFORM_COLORS.selectedHandleRing;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(selectedHandle === "start" ? startX : endX, midY, SELECTED_RING_RADIUS, 0, Math.PI * 2);
    ctx.stroke();
  }

  if (spec.playhead) {
    const x = Math.max(0, Math.min(width, spec.playhead.ratio * width));
    ctx.strokeStyle = spec.playhead.color ?? WAVEFORM_COLORS.playhead;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
  }
}
