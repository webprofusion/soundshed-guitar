/**
 * The main chain's row, and wrapping it onto more lines instead of scrolling it
 * sideways.
 *
 * The row is built from segments: the input on its own, then each connector
 * with the node it leads into. A splitter's segment also carries its parallel
 * block and the mixer that joins it, because the branch routes reach under both
 * nodes. With wrap off a segment is `display: contents`, so the row lays out as
 * the flat list it always was. With it on the row wraps between segments, so a
 * line never ends on a dangling connector and every later line starts with the
 * connector (and its "+") that leads into its first node. The route from the end
 * of one line round to the start of the next is drawn here, over the row, once
 * the browser has laid the lines out.
 */

import { renderIcon } from "../iconAssets.js";
import { getCurrentUiSettings } from "../windowSettings.js";
import type { EdgeRef } from "./graph.js";

const SVG_NS = "http://www.w3.org/2000/svg";

/** How far a return route runs past a line's end, or before its start, before turning.
 *  The row's inline padding (--signal-wrap-gutter) leaves room for it. */
const ROUTE_REACH = 10;

const ROUTE_RADIUS = 8;

export function isSignalChainWrapEnabled(): boolean {
  return getCurrentUiSettings().signalPathWrap === true;
}

export function renderBoundaryNode(kind: "input" | "output"): string {
  const label = kind === "input" ? "Input" : "Output";
  const icon = kind === "input" ? renderIcon("guitar", "fx-effect-icon") : "🔈";
  return `
    <div class="signal-node ${kind}-node" data-node-id="__${kind}__" title="${label}" aria-label="${label}">
      <div class="node-icon">${icon}</div>
      <div class="node-info">
        <div class="node-name">${label}</div>
      </div>
      <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
    </div>`;
}

export function renderConnectorWrapper(edge: EdgeRef): string {
  const data = `data-edge-from="${edge.from}" data-edge-to="${edge.to}" data-edge-from-port="${edge.fromPort}"`
    + ` data-edge-to-port="${edge.toPort}" data-edge-gain="${edge.gain}"`;
  return `
    <div class="signal-connector-wrapper" ${data}>
      <div class="signal-connector"></div>
      <button class="signal-add-btn" ${data} title="Add Effect">
        <span class="add-icon">+</span>
      </button>
    </div>
  `;
}

/** The whole chain: the input, then each segment. The caller ends the last one with the output. */
export function renderSignalChainRow(segments: readonly string[]): string {
  const markup = [renderBoundaryNode("input"), ...segments]
    .map((segment) => `<div class="signal-chain-segment">${segment}</div>`)
    .join("");
  return `
    <div class="signal-graph-container">
      <div class="signal-graph-row">${markup}</div>
    </div>`;
}

/** Applies the wrap setting to the bar and its toggle. Returns whether the chain wraps. */
export function applySignalChainWrapState(bar: HTMLElement | null): boolean {
  const wrap = isSignalChainWrapEnabled();
  bar?.classList.toggle("chain-wrap", wrap);
  const button = document.getElementById("signal-path-wrap-btn");
  if (button) {
    button.setAttribute("aria-pressed", String(wrap));
    button.title = wrap ? "Show the signal chain on one line" : "Wrap the signal chain onto more lines";
  }
  return wrap;
}

type Box = { left: number; right: number; top: number; bottom: number };

type Line = { boxes: Box[]; top: number; bottom: number };

let routeObserver: ResizeObserver | null = null;

let observedContainer: HTMLElement | null = null;

/**
 * Keeps the routes following the layout. The container's width follows the bar,
 * which moves with panels as well as the window, and segments change size as
 * fonts load and as a density switch animates. The observer is only re-armed for
 * a newly rendered chain, because observing always reports once, and re-arming
 * on every redraw would redraw forever.
 */
function watchChain(container: HTMLElement | null, onLayoutChange: () => void): void {
  if (container === observedContainer) {
    return;
  }
  routeObserver?.disconnect();
  observedContainer = container;
  if (!container || typeof ResizeObserver === "undefined") {
    return;
  }
  routeObserver ??= new ResizeObserver(() => onLayoutChange());
  routeObserver.observe(container);
  container.querySelectorAll(":scope > .signal-graph-row > .signal-chain-segment")
    .forEach((segment) => routeObserver?.observe(segment));
}

function routePath(from: Line, to: Line, reach: number): string {
  const last = from.boxes[from.boxes.length - 1];
  const first = to.boxes[0];
  const y1 = (from.top + from.bottom) / 2;
  const y2 = (to.top + to.bottom) / 2;
  const yGap = (from.bottom + to.top) / 2;
  const xRight = last.right + reach;
  const xLeft = first.left - reach;
  const r = Math.max(0, Math.min(ROUTE_RADIUS, reach, (yGap - y1) / 2, (y2 - yGap) / 2));
  return [
    `M ${last.right} ${y1}`,
    `H ${xRight - r} Q ${xRight} ${y1} ${xRight} ${y1 + r}`,
    `V ${yGap - r} Q ${xRight} ${yGap} ${xRight - r} ${yGap}`,
    `H ${xLeft + r} Q ${xLeft} ${yGap} ${xLeft} ${yGap + r}`,
    `V ${y2 - r} Q ${xLeft} ${y2} ${xLeft + r} ${y2}`,
    `H ${first.left}`,
  ].join(" ");
}

/**
 * Draws the route from the end of each wrapped line round to the start of the
 * next. Each line's main signal runs through its middle: the row centres its
 * segments, and a segment centres its connector, nodes and parallel block.
 */
export function updateSignalChainWrapRoutes(nodesElement: HTMLElement | null, onLayoutChange: () => void): void {
  const container = nodesElement?.querySelector<HTMLElement>(":scope > .signal-graph-container") ?? null;
  container?.querySelector(":scope > .signal-chain-wrap-routes")?.remove();
  const row = container?.querySelector<HTMLElement>(":scope > .signal-graph-row") ?? null;
  const wrap = Boolean(row) && isSignalChainWrapEnabled();
  watchChain(wrap ? container : null, onLayoutChange);
  // A wrapped row takes the full width, which leaves its lines off to the left.
  // Narrowing it to its widest line keeps every line break (each line already fit
  // in that width, and the next segment did not fit in a wider one), and the
  // container then centres it. Measure unpinned, since the width may have changed.
  row?.style.removeProperty("width");
  if (!container || !row || !wrap) {
    return;
  }

  let lines = measureLines(container, row);
  if (lines.length > 1) {
    const widest = Math.max(...lines.map((line) => line.boxes[line.boxes.length - 1].right - line.boxes[0].left));
    const style = getComputedStyle(row);
    const inline = Number.parseFloat(style.paddingLeft) + Number.parseFloat(style.paddingRight);
    row.style.width = `${Math.ceil(widest + inline)}px`;
    lines = measureLines(container, row);
  }
  if (lines.length < 2) {
    return;
  }

  const gutter = Number.parseFloat(getComputedStyle(row).paddingLeft) || ROUTE_REACH;
  const reach = Math.min(ROUTE_REACH, Math.max(4, gutter - 4));
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.classList.add("signal-chain-wrap-routes");
  svg.setAttribute("aria-hidden", "true");
  for (let i = 0; i + 1 < lines.length; i++) {
    const path = document.createElementNS(SVG_NS, "path");
    path.setAttribute("d", routePath(lines[i], lines[i + 1], reach));
    svg.appendChild(path);
  }
  // First, so the nodes paint over the route where a card's artwork overhangs it.
  container.prepend(svg);
}

/**
 * Which line of a wrapped chain a viewport y falls on, or null when the chain is
 * on one line or the point is not on any of them. The node drop rules use it to
 * tell a move to another line from a bypass flick.
 */
export function signalChainLineAt(nodesElement: HTMLElement | null, clientY: number): number | null {
  const container = nodesElement?.querySelector<HTMLElement>(":scope > .signal-graph-container") ?? null;
  const row = container?.querySelector<HTMLElement>(":scope > .signal-graph-row") ?? null;
  if (!container || !row || !isSignalChainWrapEnabled()) {
    return null;
  }
  const lines = measureLines(container, row);
  const { origin, scale } = containerFrame(container);
  const y = (clientY - origin.top) / scale;
  const index = lines.findIndex((line) => y >= line.top && y <= line.bottom);
  return lines.length > 1 && index >= 0 ? index : null;
}

/** Client rects include the page zoom; the SVG's units are the container's own pixels. */
function containerFrame(container: HTMLElement): { origin: DOMRect; scale: number } {
  const origin = container.getBoundingClientRect();
  return { origin, scale: container.offsetWidth > 0 ? origin.width / container.offsetWidth : 1 };
}

/** The row's segments grouped into the lines the browser broke them into, in the container's pixels. */
function measureLines(container: HTMLElement, row: HTMLElement): Line[] {
  const { origin, scale } = containerFrame(container);
  const lines: Line[] = [];
  row.querySelectorAll<HTMLElement>(":scope > .signal-chain-segment").forEach((segment) => {
    const rect = segment.getBoundingClientRect();
    const box: Box = {
      left: (rect.left - origin.left) / scale,
      right: (rect.right - origin.left) / scale,
      top: (rect.top - origin.top) / scale,
      bottom: (rect.bottom - origin.top) / scale,
    };
    const line = lines[lines.length - 1];
    // Along a line each segment starts where the one before it ends; one that starts
    // before that began a new line (possibly under a line of one segment).
    if (!line || box.left + 1 < line.boxes[line.boxes.length - 1].right) {
      lines.push({ boxes: [box], top: box.top, bottom: box.bottom });
      return;
    }
    line.boxes.push(box);
    line.top = Math.min(line.top, box.top);
    line.bottom = Math.max(line.bottom, box.bottom);
  });
  return lines;
}
