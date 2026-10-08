/**
 * The Simple Cabinet's section of the params panel: the response the engine
 * will apply, drawn over the live spectrum of the cab's input, and two IR
 * tools — save this voicing to the IR library, and match the voicing to an IR
 * picked in the resource browser.
 *
 * The curve is the engine's own (effectResponse.ts), including the dry Mix, the
 * second mic and the floor bounce of a distant mic, so what is drawn is what is
 * heard. Speaker Drive depends on level and is left out, as it is from an export.
 */

import { EffectGuids } from "../../effectGuids.js";
import {
  exportEffectAsIr,
  requestEffectResponse,
  requestSimpleCabIrMatch,
  responseDbAt,
  type ResponseCurve,
} from "../../effectResponse.js";
import { drawResponsePlot } from "../../eqPlot.js";
import { enumLabel } from "../../paramLabels.js";
import { showNotification } from "../../notifications.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../../presetV2.js";
import { resourceBrowserModal } from "../../resourceBrowser.js";
import type { GraphNode, Preset } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { applyEffectPresetParams } from "../effectPresets.js";
import { getLibraryResourceName } from "../nodeTypes.js";
import { nodeParamsPanelElement } from "../state.js";
import { ensureNodeSpectrumWatcher, fitCanvasToLayout } from "./nodeSpectrum.js";
import { paramsPanelInteractions } from "./state.js";

/** The plot's range: a cabinet lifts its low resonance by up to ~15 dB and rolls
 * the top off by far more, so the axis leans low. */
const CURVE_MIN_DB = -36;
const CURVE_MAX_DB = 18;
const CURVE_GRID_DB = 12;

/** The curve last received, and which node it belongs to. */
let shownCurve: { nodeId: string; curve: ResponseCurve } | null = null;

/** The IR last matched, so the browser reopens on it. */
let lastMatchedIrId = "";

export function isSimpleCabNode(node: GraphNode): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kCabSimple;
}

/** The section's markup, or "" for any other effect. */
export function buildCabResponseSectionHtml(node: GraphNode): string {
  if (!isSimpleCabNode(node)) {
    return "";
  }
  return `
    <section class="cab-response" data-node-id="${escapeHtml(node.id)}">
      <div class="eq-visualizer cab-response-visualizer">
        <div class="eq-visualizer-header">
          <span>Cabinet Response</span>
          <span class="eq-visualizer-range">+${CURVE_MAX_DB} / ${CURVE_MIN_DB} dB</span>
        </div>
        <canvas class="eq-curve-canvas cab-response-canvas" role="img" aria-label="Cabinet frequency response"></canvas>
      </div>
      <div class="cab-response-tools">
        <button type="button" class="cab-response-match-btn"
          title="Pick an IR, and set the cabinet, mic and tone to the closest shape this cab can make">Match an IR…</button>
        <button type="button" class="cab-response-export-btn"
          title="Save this voicing to the IR library, for an IR cabinet or another app">Export as IR</button>
        <span class="cab-response-status" role="status" aria-live="polite"></span>
      </div>
    </section>
  `;
}

/** This node's section as the panel shows it now, if it shows it at all. */
function liveSection(nodeId: string): HTMLElement | null {
  const section = nodeParamsPanelElement?.querySelector<HTMLElement>(".cab-response");
  return section?.dataset.nodeId === nodeId ? section : null;
}

function curveCanvas(): HTMLCanvasElement | null {
  return nodeParamsPanelElement?.querySelector<HTMLCanvasElement>(".cab-response-canvas") ?? null;
}

function drawCurve(canvas: HTMLCanvasElement, nodeId: string): void {
  const curve = shownCurve?.nodeId === nodeId ? shownCurve.curve : null;
  drawResponsePlot(canvas, curve ? (freq) => responseDbAt(curve, freq) : () => Number.NaN, {
    spectrum: paramsPanelInteractions.eqSpectrum?.spectrum ?? null,
    minDb: CURVE_MIN_DB,
    maxDb: CURVE_MAX_DB,
    gridStepDb: CURVE_GRID_DB,
  });
}

/** Redraws the curve for the node's current parameters: straight away with the
 * last curve, then again when the engine answers for the new ones. */
export function updateCabResponseVisualization(node: GraphNode): void {
  const canvas = isSimpleCabNode(node) ? curveCanvas() : null;
  if (!canvas) {
    return;
  }
  fitCanvasToLayout(canvas);
  ensureNodeSpectrumWatcher(canvas, node.id, () => drawCurve(canvas, node.id));
  drawCurve(canvas, node.id);

  requestEffectResponse(node.type, node.params ?? {}, (curve) => {
    if (curve) {
      shownCurve = { nodeId: node.id, curve };
    }
    // The panel may have been rebuilt, or moved to another node, while the request was out.
    const current = curveCanvas();
    if (current && current.closest<HTMLElement>(".cab-response")?.dataset.nodeId === node.id) {
      drawCurve(current, node.id);
    }
  });
}

/** "Cybercab 2x12 Open Ribbon": what the export is called until the user renames it. */
function exportName(node: GraphNode): string {
  const parameters = getNodeEffectInfo(node)?.parameters ?? [];
  const label = (key: string): string => {
    const def = parameters.find((parameter) => parameter.key === key);
    const value = node.params?.[key] ?? def?.default ?? 0;
    return def?.labels ? (enumLabel(value, def.labels, def.min, def.step) ?? "") : "";
  };
  return ["Cybercab", label("cabinet"), label("micType")].filter(Boolean).join(" ");
}

export function bindCabResponseControls(node: GraphNode, preset: Preset): void {
  const section = isSimpleCabNode(node)
    ? nodeParamsPanelElement?.querySelector<HTMLElement>(".cab-response")
    : null;
  if (!section) {
    return;
  }
  const matchButton = section.querySelector<HTMLButtonElement>(".cab-response-match-btn");
  const exportButton = section.querySelector<HTMLButtonElement>(".cab-response-export-btn");
  const status = section.querySelector<HTMLElement>(".cab-response-status");
  const setStatus = (text: string): void => {
    if (status) {
      status.textContent = text;
    }
  };

  const matchIr = (resourceId: string): void => {
    lastMatchedIrId = resourceId;
    const irName = getLibraryResourceName("ir", resourceId) || resourceId;
    // Opening the browser rebuilds the panel, so the controls bound above are
    // gone by now: show progress on the section on screen at each step.
    const showMatching = (matching: boolean): void => {
      const live = liveSection(node.id);
      const button = live?.querySelector<HTMLButtonElement>(".cab-response-match-btn");
      const text = live?.querySelector<HTMLElement>(".cab-response-status");
      if (button) {
        button.disabled = matching;
      }
      if (text) {
        text.textContent = matching ? "Matching…" : "";
      }
    };
    showMatching(true);
    requestSimpleCabIrMatch(resourceId, (result) => {
      if (!result.ok) {
        showMatching(false);
        showNotification("Match failed", result.error);
        return;
      }
      // Rebuilds the panel, so the knobs and the curve both show the match.
      applyEffectPresetParams(node, preset, result.params);
      const error = Number.isFinite(result.rmsErrorDb) ? ` (shape within ${result.rmsErrorDb.toFixed(1)} dB)` : "";
      showNotification("Cabinet matched", `${irName}${error}`);
    });
  };

  matchButton?.addEventListener("click", () => {
    // No node: the pick is only an IR to match against, not one to load and
    // audition. Its own role key keeps this browsing from moving where the IR
    // cabs' folder and next/prev point.
    resourceBrowserModal.open({
      resourceType: "ir",
      currentId: lastMatchedIrId,
      libraryCategoryHint: "ir",
      contextKey: "ir-match",
      title: "Match an IR",
      hint: "Double click an IR, or select it and click OK, to match the cabinet to it.",
      onSelect: matchIr,
    });
  });

  exportButton?.addEventListener("click", () => {
    exportButton.disabled = true;
    setStatus("Exporting…");
    exportEffectAsIr(node.type, node.params ?? {}, exportName(node), (result) => {
      exportButton.disabled = false;
      setStatus("");
      if (result.ok) {
        showNotification("IR saved to your library", result.name);
      } else {
        showNotification("IR export failed", result.error);
      }
    });
  });
}
