/**
 * Lights the signal path with the signal: a connector, node or route between
 * wrapped lines that is passing signal gets `signal-live`, which turns its cable
 * green with pulses running down it and brightens the node's underglow
 * (css/signal-path/connections.css, nodes.css, wrap.css).
 *
 * It is on/off rather than a level on purpose. A level written twenty times a
 * second restyled the chain on nearly every frame (measured: style recalculation
 * up from about 1% to 9% of a core while playing); a class that flips when you
 * start and stop playing costs nothing in between.
 *
 * It is always on. It used to be a setting ("Animate signal flow", off by
 * default) because the pulses cost a few percent of a core while playing: the
 * meters keep the page drawing every frame, and Blink restyles running
 * animations on each one.
 *
 * A connector carries the signal of the node it leaves. Not every node reports a
 * level (a splitter may not), so a node without its own reading is live when any
 * connector leading into it is.
 */

import { uiState } from "../state.js";
import { signalPathNodesElement } from "./state.js";

/** RMS above this counts as signal. */
const LIVE_THRESHOLD_DBFS = -54;

/** How long a live element stays live through quiet, so a gap between notes or a decaying note does not flicker it. */
const RELEASE_MS = 400;

/** When each live element last saw signal. */
const lastHeard = new WeakMap<Element, number>();

function setLive(element: Element, hearing: boolean, now: number): void {
  if (hearing) {
    lastHeard.set(element, now);
  }
  const live = hearing || now - (lastHeard.get(element) ?? -Infinity) < RELEASE_MS;
  // classList.toggle with a force that matches leaves the attribute alone.
  element.classList.toggle("signal-live", live);
}

/** Applies the latest diagnostics frame to the connectors and nodes in the signal path bar. */
export function updateSignalPathFlow(): void {
  const root = signalPathNodesElement;
  if (!root) {
    return;
  }

  const diagnostics = uiState.signalDiagnostics;
  const reported = new Map<string, boolean>();
  const hears = (rmsDbfs: number | undefined): boolean => typeof rmsDbfs === "number" && rmsDbfs > LIVE_THRESHOLD_DBFS;
  if (diagnostics) {
    diagnostics.nodes.forEach((node) => {
      if (typeof node.nodeId === "string") {
        reported.set(node.nodeId, hears(node.levels?.rmsDbfs));
      }
    });
    reported.set("__input__", hears(diagnostics.input?.rmsDbfs));
    reported.set("__output__", hears(diagnostics.output?.rmsDbfs));
  }

  const connectors = Array.from(root.querySelectorAll<HTMLElement>(".signal-connector-wrapper[data-edge-from]"));
  const sources = new Map<string, string[]>();
  connectors.forEach((connector) => {
    const from = connector.dataset.edgeFrom ?? "";
    const to = connector.dataset.edgeTo ?? "";
    const list = sources.get(to);
    if (list) {
      list.push(from);
    } else {
      sources.set(to, [from]);
    }
  });

  const resolved = new Map<string, boolean>();
  const resolving = new Set<string>();
  const hearsAt = (nodeId: string): boolean => {
    const known = reported.get(nodeId) ?? resolved.get(nodeId);
    if (known !== undefined) {
      return known;
    }
    if (resolving.has(nodeId)) {
      return false;
    }
    resolving.add(nodeId);
    const hearing = (sources.get(nodeId) ?? []).some(hearsAt);
    resolving.delete(nodeId);
    resolved.set(nodeId, hearing);
    return hearing;
  };

  const now = performance.now();
  const update = (element: HTMLElement | SVGElement, nodeId: string | undefined): void => {
    setLive(element, Boolean(diagnostics) && hearsAt(nodeId ?? ""), now);
  };
  connectors.forEach((connector) => update(connector, connector.dataset.edgeFrom));
  root.querySelectorAll<HTMLElement>(".signal-node[data-node-id]").forEach((node) => update(node, node.dataset.nodeId));
  root.querySelectorAll<SVGElement>(".signal-chain-wrap-routes path[data-edge-from]")
    .forEach((route) => update(route, route.dataset.edgeFrom));
}
