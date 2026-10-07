/**
 * The IR Cabinet's Alignment section: where IR B sits in time against IR A.
 *
 * Two blended IRs sum like two mics on one speaker. Lined up they reinforce; a
 * fraction of a millisecond apart they comb-filter, which can be a fault to fix
 * or a tone to choose, so any offset in reach can be set, not only the aligned
 * one. The section draws the start of both IRs with B moved by the offset (drag it),
 * how alike the two are at every offset (click or drag along it), and the two
 * summed against the two added as if they could not interfere, so a cancellation
 * shows as a dip. Auto Align moves B, and flips it if it lines up inverted, to
 * where the engine finds the best fit (irAlignment.ts).
 *
 * The offset is the node's slotBOffset; polarity is slotAPolarity and
 * slotBPolarity, the same parameters as the Invert toggles in the IR A and IR B
 * groups. The section sits under the Main and Advanced tabs, so it is always in
 * view. With only one IR loaded there is nothing to align, and it shows that IR's
 * response alone.
 */

import { EffectGuids } from "../../effectGuids.js";
import { drawResponsePlot } from "../../eqPlot.js";
import {
  buildAlignmentSpectra,
  combinedResponse,
  matchAtOffset,
  requestIrAlignment,
  slotGains,
  slotPolarities,
  type AlignmentSpectra,
  type IrAlignmentAnalysis,
} from "../../irAlignment.js";
import { EffectTypeRegistry } from "../../presetV2.js";
import type { GraphNode, ResourceRef } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";
import { fitCanvasToLayout } from "./nodeSpectrum.js";

/** The parameters the section owns; the generic controls leave them out, with one
 * IR loaded too, since the offset does nothing then. */
export const IR_ALIGNMENT_PARAM_KEYS: readonly string[] = ["slotBOffset"];

/** slotBOffset's reach either way, in ms (IrSlotAlignment::kMaxOffsetMs). */
const MAX_OFFSET_MS = 10;
/** Sound travels about 34.3 cm in a millisecond: how far a mic moves for an offset. */
const CM_PER_MS = 34.3;
const RESPONSE_MIN_DB = -36;
const RESPONSE_MAX_DB = 18;
const RESPONSE_GRID_DB = 12;

/** The last analysis, for the pair of IRs it was made from. */
let shown: { key: string; analysis: IrAlignmentAnalysis; spectra: AlignmentSpectra } | null = null;
let failed: { key: string; error: string } | null = null;
let requestedKey: string | null = null;
let resizeObserver: ResizeObserver | null = null;

export function isIrCabNode(node: GraphNode): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kCabIr;
}

function slotRef(node: GraphNode, index: number): ResourceRef | null {
  const ref = node.resources?.[index];
  return ref && (ref.resourceId || ref.id || ref.filePath || ref.embeddedId) ? ref : null;
}

/** Which IRs the analysis is for: a new one in either slot needs a new analysis. */
function pairKey(irA: ResourceRef | null, irB: ResourceRef | null): string {
  const one = (ref: ResourceRef | null): string =>
    ref ? [ref.resourceId ?? ref.id ?? "", ref.filePath ?? "", ref.embeddedId ?? ""].join("|") : "-";
  return `${one(irA)}::${one(irB)}`;
}

function offsetOf(node: GraphNode): number {
  return Math.max(-MAX_OFFSET_MS, Math.min(MAX_OFFSET_MS, node.params?.slotBOffset ?? 0));
}

/** "+0.42 ms", to the microsecond when it is that fine. */
function formatOffset(ms: number): string {
  const fixed = Math.abs(ms) < 0.1 && ms !== 0 ? ms.toFixed(3) : ms.toFixed(2);
  return `${ms > 0 ? "+" : ""}${fixed} ms`;
}

/** What the offset means as a mic move: "B 14.4 cm closer". */
function describeDistance(ms: number): string {
  const cm = Math.abs(ms) * CM_PER_MS;
  if (cm < 0.05) {
    return "B where it was captured";
  }
  return `B ${cm < 10 ? cm.toFixed(1) : Math.round(cm)} cm ${ms < 0 ? "closer" : "further away"}`;
}

/** One IR's response on its own, for a cab with only that slot filled. */
function singleIrSectionHtml(node: GraphNode, slot: "A" | "B"): string {
  return `
      <section class="node-param-group-block ir-alignment ir-alignment-single" data-node-id="${escapeHtml(node.id)}" aria-label="IR response">
        <div class="node-param-group-title">IR Response</div>
        <div class="eq-visualizer ir-alignment-response-wrap">
          <div class="eq-visualizer-header">
            <span>IR ${slot}</span>
            <span class="eq-visualizer-range ir-alignment-status" role="status" aria-live="polite"></span>
          </div>
          <canvas class="eq-curve-canvas ir-alignment-response" role="img" aria-label="Response of IR ${slot}"></canvas>
        </div>
        <p class="ir-alignment-note"></p>
      </section>`;
}

/** The section's markup: "" for any other effect or a cab with no IR, one IR's
 * response with one, and the alignment controls with both. */
export function buildIrAlignmentSectionHtml(node: GraphNode): string {
  const hasA = Boolean(slotRef(node, 0));
  const hasB = Boolean(slotRef(node, 1));
  if (!isIrCabNode(node) || (!hasA && !hasB)) {
    return "";
  }
  if (!hasA || !hasB) {
    return singleIrSectionHtml(node, hasA ? "A" : "B");
  }
  const offset = offsetOf(node);
  const polarity = slotPolarities(node.params ?? {});
  const toggle = (key: string, label: string, inverted: boolean): string => `
    <label class="ir-alignment-invert">
      <span class="toggle-switch">
        <input class="ir-alignment-invert-input" type="checkbox" data-param-key="${escapeHtml(key)}"${inverted ? " checked" : ""}>
        <span class="toggle-slider"></span>
      </span>
      <span>${label}</span>
    </label>`;
  return `
      <section class="node-param-group-block ir-alignment" data-node-id="${escapeHtml(node.id)}" aria-label="IR alignment">
        <div class="node-param-group-title">Alignment</div>
        <div class="ir-alignment-header">
          <span class="ir-alignment-legend">
            <span class="ir-alignment-key ir-alignment-key-a">IR A</span>
            <span class="ir-alignment-key ir-alignment-key-b">IR B</span>
          </span>
          <span class="ir-alignment-status" role="status" aria-live="polite"></span>
        </div>
        <canvas class="eq-curve-canvas ir-alignment-waves" tabindex="0" role="slider" aria-label="IR B offset: drag to move IR B"
          aria-valuemin="${-MAX_OFFSET_MS}" aria-valuemax="${MAX_OFFSET_MS}" aria-valuenow="${offset}"></canvas>
        <canvas class="eq-curve-canvas ir-alignment-match" aria-label="How alike A and B are at each offset: click to move IR B there"></canvas>
        <div class="ir-alignment-controls">
          <label class="ir-alignment-offset">
            <span>Offset</span>
            <input class="ir-alignment-offset-input" type="number" inputmode="decimal" step="0.01"
              min="${-MAX_OFFSET_MS}" max="${MAX_OFFSET_MS}" value="${offset}" aria-label="IR B offset in ms">
            <span>ms</span>
          </label>
          <span class="ir-alignment-nudges" role="group" aria-label="Nudge IR B">
            <button type="button" data-nudge="-0.1" title="0.1 ms earlier">&minus;0.1</button>
            <button type="button" data-nudge="-0.01" title="0.01 ms earlier">&minus;.01</button>
            <button type="button" data-nudge="0.01" title="0.01 ms later">+.01</button>
            <button type="button" data-nudge="0.1" title="0.1 ms later">+0.1</button>
          </span>
          <button type="button" class="ir-alignment-auto" disabled
            title="Move IR B, and flip it if needed, to where it lines up best with IR A">Auto Align</button>
          <button type="button" class="ir-alignment-zero" title="Put IR B back where its file starts it">Zero</button>
        </div>
        <div class="ir-alignment-readouts">
          ${toggle("slotAPolarity", "Invert A", polarity.a < 0)}
          ${toggle("slotBPolarity", "Invert B", polarity.b < 0)}
          <span class="ir-alignment-distance"></span>
          <span class="ir-alignment-match-readout"></span>
        </div>
        <div class="eq-visualizer ir-alignment-response-wrap">
          <div class="eq-visualizer-header">
            <span>A + B Response</span>
            <span class="eq-visualizer-range">Dashed: without interference</span>
          </div>
          <canvas class="eq-curve-canvas ir-alignment-response" role="img" aria-label="Response of IR A and IR B summed"></canvas>
        </div>
        <p class="ir-alignment-note"></p>
      </section>`;
}

function section(): HTMLElement | null {
  return nodeParamsPanelElement?.querySelector<HTMLElement>(".ir-alignment") ?? null;
}

function themeColor(element: Element, name: string, fallback: string): string {
  return window.getComputedStyle(element).getPropertyValue(name).trim() || fallback;
}

/** The stretch of time the waveforms show, in ms of A's time. Held still while a
 * drag is in progress, so the waveform does not slide out from under the pointer. */
interface WaveView {
  startMs: number;
  spanMs: number;
}

function waveViewFor(analysis: IrAlignmentAnalysis, offsetMs: number): WaveView {
  const startB = analysis.onsetBMs + offsetMs;
  const startMs = Math.min(analysis.onsetAMs, startB) - 0.5;
  const spanMs = Math.max(4, Math.abs(startB - analysis.onsetAMs) + 3.5);
  return { startMs, spanMs };
}

function drawWaves(canvas: HTMLCanvasElement, analysis: IrAlignmentAnalysis, node: GraphNode, view: WaveView): void {
  fitCanvasToLayout(canvas);
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }
  const { width, height } = canvas;
  const offset = offsetOf(node);
  const polarity = slotPolarities(node.params ?? {});
  ctx.clearRect(0, 0, width, height);

  const grid = themeColor(canvas, "--eq-curve-grid", "rgba(255,255,255,0.08)");
  const middle = height / 2;
  const toX = (ms: number): number => ((ms - view.startMs) / view.spanMs) * width;
  ctx.strokeStyle = grid;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, middle);
  ctx.lineTo(width, middle);
  for (let ms = Math.ceil(view.startMs); ms <= view.startMs + view.spanMs; ms += 1) {
    ctx.moveTo(toX(ms), 0);
    ctx.lineTo(toX(ms), height);
  }
  ctx.stroke();

  // Each drawn to its own peak: the shapes and their timing are what line up, not their levels.
  const trace = (samples: Float32Array, shiftMs: number, sign: number, color: string): void => {
    let peak = 0;
    for (const sample of samples) {
      peak = Math.max(peak, Math.abs(sample));
    }
    if (peak <= 0) {
      return;
    }
    const scale = (sign * (height * 0.44)) / peak;
    const msPerSample = 1000 / analysis.sampleRate;
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    let penDown = false;
    for (let px = 0; px < width; px += 1) {
      const ms = view.startMs + (px / width) * view.spanMs - shiftMs;
      const position = ms / msPerSample;
      const index = Math.floor(position);
      if (index < 0 || index + 1 >= samples.length) {
        penDown = false;
        continue;
      }
      const value = samples[index] + (position - index) * (samples[index + 1] - samples[index]);
      const y = middle - value * scale;
      if (penDown) {
        ctx.lineTo(px, y);
      } else {
        ctx.moveTo(px, y);
        penDown = true;
      }
    }
    ctx.stroke();
  };
  trace(analysis.waveformA, 0, polarity.a, themeColor(canvas, "--ir-alignment-a", "rgba(72, 168, 224, 0.95)"));
  trace(analysis.waveformB, offset, polarity.b, themeColor(canvas, "--ir-alignment-b", "rgba(240, 160, 64, 0.95)"));
  canvas.setAttribute("aria-valuenow", offset.toFixed(3));
}

function matchStripX(offsetMs: number, width: number): number {
  return ((offsetMs + MAX_OFFSET_MS) / (2 * MAX_OFFSET_MS)) * width;
}

function drawMatchStrip(canvas: HTMLCanvasElement, analysis: IrAlignmentAnalysis, node: GraphNode): void {
  fitCanvasToLayout(canvas);
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }
  const { width, height } = canvas;
  const polarity = slotPolarities(node.params ?? {});
  const sign = polarity.a * polarity.b;
  const middle = height / 2;
  ctx.clearRect(0, 0, width, height);

  ctx.strokeStyle = themeColor(canvas, "--eq-curve-grid", "rgba(255,255,255,0.08)");
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, middle);
  ctx.lineTo(width, middle);
  for (let ms = -MAX_OFFSET_MS; ms <= MAX_OFFSET_MS; ms += 2) {
    ctx.moveTo(matchStripX(ms, width), middle - 3);
    ctx.lineTo(matchStripX(ms, width), middle + 3);
  }
  ctx.stroke();

  // Above the line the two reinforce, below it they cancel, as the polarities stand.
  ctx.fillStyle = themeColor(canvas, "--ir-alignment-match-fill", "rgba(72, 168, 224, 0.45)");
  ctx.beginPath();
  ctx.moveTo(0, middle);
  for (let px = 0; px <= width; px += 1) {
    const ms = (px / width) * 2 * MAX_OFFSET_MS - MAX_OFFSET_MS;
    ctx.lineTo(px, middle - sign * matchAtOffset(analysis, ms) * (middle - 2));
  }
  ctx.lineTo(width, middle);
  ctx.closePath();
  ctx.fill();

  const marker = (ms: number, color: string, dashed: boolean): void => {
    const x = Math.round(matchStripX(ms, width)) + 0.5;
    ctx.strokeStyle = color;
    ctx.lineWidth = dashed ? 1 : 2;
    ctx.setLineDash(dashed ? [3, 3] : []);
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
    ctx.setLineDash([]);
  };
  marker(analysis.alignedOffsetMs, themeColor(canvas, "--text-secondary", "#9aa0b5"), true);
  marker(offsetOf(node), themeColor(canvas, "--ir-alignment-b", "rgba(240, 160, 64, 0.95)"), false);
}

function drawResponse(canvas: HTMLCanvasElement, spectra: AlignmentSpectra, analysis: IrAlignmentAnalysis, node: GraphNode): void {
  fitCanvasToLayout(canvas);
  const response = combinedResponse(spectra, slotGains(node.params ?? {}, analysis), offsetOf(node));
  const at = (values: number[]) => (freq: number): number => {
    const { frequencies } = response;
    const last = frequencies.length - 1;
    if (freq <= frequencies[0]) {
      return values[0];
    }
    if (freq >= frequencies[last]) {
      return values[last];
    }
    // The frequencies are evenly spaced in log, so the neighbours are found directly.
    const position = (Math.log(freq / frequencies[0]) / Math.log(frequencies[last] / frequencies[0])) * last;
    const low = Math.floor(position);
    return values[low] + (position - low) * (values[low + 1] - values[low]);
  };
  drawResponsePlot(canvas, at(response.summedDb), {
    minDb: RESPONSE_MIN_DB,
    maxDb: RESPONSE_MAX_DB,
    gridStepDb: RESPONSE_GRID_DB,
    // One IR alone cannot interfere with anything, so there is nothing to compare it with.
    reference: analysis.slots === "both" ? at(response.unrelatedDb) : null,
  });
}

/** Blend at one end, or L/R Split, changes what the offset does: say so. And a cab
 * with only IR B plays nothing through it (IRCabEffect passes the input dry). */
function noteFor(node: GraphNode, single: boolean): string {
  if (single) {
    return slotRef(node, 0) ? "" : "IR B is only heard once IR A is loaded.";
  }
  const params = node.params ?? {};
  if ((params.lrSplit ?? 0) >= 0.5) {
    return "L/R Split is on: A plays left and B right, so the offset sets the stereo width rather than the tone of a blend.";
  }
  const blend = params.irBlend ?? 0;
  if (blend <= 0.01 || blend >= 0.99) {
    return `IR Blend is all ${blend <= 0.01 ? "A" : "B"}, so the offset is not heard yet. Move IR Blend toward the middle.`;
  }
  return "";
}

function redraw(node: GraphNode, view: WaveView | null = null): void {
  const root = section();
  if (!root || root.dataset.nodeId !== node.id) {
    return;
  }
  const offset = offsetOf(node);
  const input = root.querySelector<HTMLInputElement>(".ir-alignment-offset-input");
  if (input && document.activeElement !== input) {
    input.value = String(Number(offset.toFixed(3)));
  }
  const distance = root.querySelector<HTMLElement>(".ir-alignment-distance");
  if (distance) {
    distance.textContent = `${formatOffset(offset)} · ${describeDistance(offset)}`;
  }
  const polarity = slotPolarities(node.params ?? {});
  root.querySelectorAll<HTMLInputElement>(".ir-alignment-invert-input").forEach((toggle) => {
    toggle.checked = (toggle.dataset.paramKey === "slotAPolarity" ? polarity.a : polarity.b) < 0;
  });
  const single = root.classList.contains("ir-alignment-single");
  const note = root.querySelector<HTMLElement>(".ir-alignment-note");
  if (note) {
    note.textContent = noteFor(node, single);
  }

  const status = root.querySelector<HTMLElement>(".ir-alignment-status");
  const analysis = shown?.analysis ?? null;
  const ready = Boolean(shown && shown.key === root.dataset.pairKey && analysis);
  const autoButton = root.querySelector<HTMLButtonElement>(".ir-alignment-auto");
  if (autoButton) {
    autoButton.disabled = !ready;
  }
  if (!ready || !shown || !analysis) {
    if (status) {
      status.textContent = failed && failed.key === root.dataset.pairKey ? failed.error : "Analysing…";
    }
    return;
  }
  if (single) {
    if (status) {
      status.textContent = "";
    }
    const response = root.querySelector<HTMLCanvasElement>(".ir-alignment-response");
    if (response) {
      drawResponse(response, shown.spectra, analysis, node);
    }
    return;
  }

  const lined = Math.abs(offset - analysis.alignedOffsetMs) < 0.02 && (polarity.a * polarity.b < 0) === analysis.invertB;
  if (status) {
    status.textContent = lined
      ? "Lined up"
      : `Lines up at ${formatOffset(analysis.alignedOffsetMs)}${analysis.invertB ? ", B inverted" : ""}`;
  }
  const readout = root.querySelector<HTMLElement>(".ir-alignment-match-readout");
  if (readout) {
    const match = matchAtOffset(analysis, offset) * polarity.a * polarity.b;
    readout.textContent = `Match ${match >= 0 ? "+" : "−"}${Math.round(Math.abs(match) * 100)}%${match < 0 ? " (cancelling)" : ""}`;
    readout.classList.toggle("is-cancelling", match < 0);
  }

  const waves = root.querySelector<HTMLCanvasElement>(".ir-alignment-waves");
  if (waves) {
    drawWaves(waves, analysis, node, view ?? waveViewFor(analysis, offset));
  }
  const strip = root.querySelector<HTMLCanvasElement>(".ir-alignment-match");
  if (strip) {
    drawMatchStrip(strip, analysis, node);
  }
  const response = root.querySelector<HTMLCanvasElement>(".ir-alignment-response");
  if (response) {
    drawResponse(response, shown.spectra, analysis, node);
  }
}

/** Redraws the section for the node's current parameters, if it is there. Called
 * after a parameter it draws (blend, levels, polarity) changes elsewhere. */
export function updateIrAlignmentVisualization(node: GraphNode): void {
  if (isIrCabNode(node)) {
    redraw(node);
  }
}

function setParam(node: GraphNode, key: string, value: number): void {
  node.params[key] = value;
  sendSignalPathNodeParamUpdate(node.id, key, value);
  // The same parameters' toggles in the IR A and IR B groups.
  nodeParamsPanelElement?.querySelectorAll<HTMLInputElement>(`.node-param-toggle[data-param-key="${key}"]`).forEach((toggle) => {
    toggle.checked = value >= 0.5;
    const label = toggle.closest(".node-param-group")?.querySelector(".node-param-value");
    if (label) {
      label.textContent = value >= 0.5 ? "On" : "Off";
    }
  });
}

/** Rounded to the microsecond, which keeps a saved preset readable. */
function setOffset(node: GraphNode, offsetMs: number, view: WaveView | null = null): void {
  const value = Math.round(Math.max(-MAX_OFFSET_MS, Math.min(MAX_OFFSET_MS, offsetMs)) * 1000) / 1000;
  if (value === offsetOf(node) && node.params.slotBOffset !== undefined) {
    redraw(node, view);
    return;
  }
  setParam(node, "slotBOffset", value);
  redraw(node, view);
}

function bindWaveDrag(canvas: HTMLCanvasElement, node: GraphNode): void {
  let drag: { pointerId: number; startX: number; startOffset: number; view: WaveView } | null = null;
  canvas.addEventListener("pointerdown", (event) => {
    const analysis = shown?.analysis;
    if (!analysis || event.button !== 0) {
      return;
    }
    drag = { pointerId: event.pointerId, startX: event.clientX, startOffset: offsetOf(node), view: waveViewFor(analysis, offsetOf(node)) };
    canvas.setPointerCapture(event.pointerId);
    canvas.classList.add("is-dragging");
    event.preventDefault();
  });
  canvas.addEventListener("pointermove", (event) => {
    if (!drag || event.pointerId !== drag.pointerId) {
      return;
    }
    // Shift for fine moves, a tenth of the pointer's travel.
    const msPerPixel = (drag.view.spanMs / Math.max(1, canvas.clientWidth)) * (event.shiftKey ? 0.1 : 1);
    setOffset(node, drag.startOffset + (event.clientX - drag.startX) * msPerPixel, drag.view);
  });
  const end = (event: PointerEvent): void => {
    if (!drag || event.pointerId !== drag.pointerId) {
      return;
    }
    drag = null;
    canvas.classList.remove("is-dragging");
    redraw(node);
  };
  canvas.addEventListener("pointerup", end);
  canvas.addEventListener("pointercancel", end);
  canvas.addEventListener("keydown", (event) => {
    const step = event.shiftKey ? 0.1 : 0.01;
    if (event.key === "ArrowLeft" || event.key === "ArrowDown") {
      setOffset(node, offsetOf(node) - step);
    } else if (event.key === "ArrowRight" || event.key === "ArrowUp") {
      setOffset(node, offsetOf(node) + step);
    } else {
      return;
    }
    event.preventDefault();
  });
}

function bindMatchStrip(canvas: HTMLCanvasElement, node: GraphNode): void {
  const offsetAt = (event: PointerEvent): number =>
    ((event.clientX - canvas.getBoundingClientRect().left) / Math.max(1, canvas.clientWidth)) * 2 * MAX_OFFSET_MS - MAX_OFFSET_MS;
  let pointer: number | null = null;
  canvas.addEventListener("pointerdown", (event) => {
    if (!shown || event.button !== 0) {
      return;
    }
    pointer = event.pointerId;
    canvas.setPointerCapture(event.pointerId);
    setOffset(node, offsetAt(event));
    event.preventDefault();
  });
  canvas.addEventListener("pointermove", (event) => {
    if (pointer === event.pointerId) {
      setOffset(node, offsetAt(event));
    }
  });
  const end = (event: PointerEvent): void => {
    if (pointer === event.pointerId) {
      pointer = null;
    }
  };
  canvas.addEventListener("pointerup", end);
  canvas.addEventListener("pointercancel", end);
}

function analyse(node: GraphNode, key: string, irA: ResourceRef | null, irB: ResourceRef | null): void {
  if (requestedKey === key) {
    return;
  }
  requestedKey = key;
  requestIrAlignment(irA, irB, (result) => {
    requestedKey = null;
    if (result.ok) {
      shown = { key, analysis: result.analysis, spectra: buildAlignmentSpectra(result.analysis) };
      failed = null;
    } else {
      failed = { key, error: result.error };
    }
    // The panel may have been rebuilt, or moved to another node, while the request was out.
    redraw(node);
  });
}

export function bindIrAlignmentControls(node: GraphNode): void {
  resizeObserver?.disconnect();
  resizeObserver = null;
  const root = section();
  const irA = slotRef(node, 0);
  const irB = slotRef(node, 1);
  if (!root || (!irA && !irB)) {
    return;
  }
  const key = pairKey(irA, irB);
  root.dataset.pairKey = key;

  const waves = root.querySelector<HTMLCanvasElement>(".ir-alignment-waves");
  const strip = root.querySelector<HTMLCanvasElement>(".ir-alignment-match");
  if (waves) {
    bindWaveDrag(waves, node);
  }
  if (strip) {
    bindMatchStrip(strip, node);
  }

  const input = root.querySelector<HTMLInputElement>(".ir-alignment-offset-input");
  input?.addEventListener("change", () => {
    const value = Number.parseFloat(input.value);
    setOffset(node, Number.isFinite(value) ? value : 0);
  });
  root.querySelectorAll<HTMLButtonElement>("[data-nudge]").forEach((button) => {
    button.addEventListener("click", () => setOffset(node, offsetOf(node) + Number.parseFloat(button.dataset.nudge ?? "0")));
  });
  root.querySelector<HTMLButtonElement>(".ir-alignment-zero")?.addEventListener("click", () => setOffset(node, 0));
  root.querySelector<HTMLButtonElement>(".ir-alignment-auto")?.addEventListener("click", () => {
    const analysis = shown?.key === key ? shown.analysis : null;
    if (!analysis) {
      return;
    }
    // B's polarity is set against A's as it stands, so A is never flipped behind the user's back.
    const invertA = slotPolarities(node.params ?? {}).a < 0;
    const invertB = invertA !== analysis.invertB ? 1 : 0;
    if ((node.params.slotBPolarity ?? 0) >= 0.5 !== (invertB === 1)) {
      setParam(node, "slotBPolarity", invertB);
    }
    setOffset(node, analysis.alignedOffsetMs);
  });
  root.querySelectorAll<HTMLInputElement>(".ir-alignment-invert-input").forEach((toggle) => {
    toggle.addEventListener("change", () => {
      const key = toggle.dataset.paramKey;
      if (key) {
        setParam(node, key, toggle.checked ? 1 : 0);
        redraw(node);
      }
    });
  });

  // Redrawn whenever its width changes, and when a hidden panel comes back into view.
  if (typeof ResizeObserver !== "undefined") {
    resizeObserver = new ResizeObserver(() => redraw(node));
    resizeObserver.observe(root);
  }

  if (shown?.key !== key) {
    analyse(node, key, irA, irB);
  }
  redraw(node);
}
