/**
 * The generic parameter controls: the tab strip, the default knob/toggle markup,
 * and the bindings that push a change back to the engine.
 */

import { GenericKnob, enhanceRangeInput } from "../../controls.js";
import { enumLabel, wholeStepLabel } from "../../paramLabels.js";
import { formatTaperedValue, parseParamTaper, taperPositionToValue } from "../../paramTaper.js";
import { getNodeEffectInfo } from "../../presetV2.js";
import { BLEND_MAPPING_EPS, buildParameterMapFromLegacy, getBlendState, normalizeBlendValue, updateBlendMatchSummary, updateBlendParamIndicators } from "../../signalPathBlend.js";
import type { BlendParamSpec } from "../../signalPathBlend.js";
import type { BlendMode, BlendModelMapping, GraphNode, Preset } from "../../types.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { requestNodeParamsPanel } from "../render.js";
import { nodeParamsPanelElement } from "../state.js";
import { isPitchShiftRangeSetting, isPitchShiftType, reconcilePitchShiftParams, semitoneKnobRange } from "./pitchShiftRange.js";
import { nodeParamKnobs } from "./state.js";
import { refreshNodeVisualizations } from "./visualizations.js";

export { buildDefaultParamControlsHtml, formatParamLabel, isToggleParam } from "../../parameterControlMarkup.js";

export function bindParamTabs(): void {
  const tabButtons = nodeParamsPanelElement?.querySelectorAll(".node-param-tab");
  const tabPanels = nodeParamsPanelElement?.querySelectorAll(".node-param-tab-panel");
  if (!tabButtons || !tabPanels || tabButtons.length === 0 || tabPanels.length === 0) {
    return;
  }

  tabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tab = (button as HTMLElement).dataset.tab;
      if (!tab) return;

      tabButtons.forEach((btn) => {
        const active = (btn as HTMLElement).dataset.tab === tab;
        btn.classList.toggle("is-active", active);
        btn.setAttribute("aria-selected", active ? "true" : "false");
      });
      tabPanels.forEach((panel) => {
        const active = (panel as HTMLElement).dataset.tab === tab;
        panel.classList.toggle("is-active", active);
      });
    });
  });
}

/**
 * After a pitch shift range setting changes, moves the settings it drags along and
 * points the Semitones knob at the new range. Works in place rather than rebuilding
 * the panel, so a Range knob being dragged keeps its drag.
 */
function syncPitchShiftRange(node: GraphNode, nodeId: string, changedKey: string): void {
  if (!isPitchShiftType(node.type) || !isPitchShiftRangeSetting(changedKey)) {
    return;
  }

  for (const [key, value] of Object.entries(reconcilePitchShiftParams(node.params, changedKey))) {
    node.params[key] = value;
    sendSignalPathNodeParamUpdate(nodeId, key, value);
    nodeParamKnobs.get(key)?.setValue(value);
  }

  const range = semitoneKnobRange(node.params);
  nodeParamKnobs.get("semitones")?.setRange(range.min, range.max, range.step);
}

export function bindNodeParamControls(node: GraphNode, preset: Preset): void {
  // Bind slider inputs from custom layouts
  const sliders = nodeParamsPanelElement?.querySelectorAll(".node-param-slider");
  sliders?.forEach((sliderEl) => {
    const input = sliderEl as HTMLInputElement;
    enhanceRangeInput(input);
    input.addEventListener("input", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        // A log-taper slider runs over its travel, 0..1, with the range in data-min/data-max.
        const sliderTaper = parseParamTaper(input.dataset.taper);
        const value = sliderTaper === "log"
          ? taperPositionToValue(
            parseFloat(input.value),
            parseFloat(input.dataset.min ?? "0"),
            parseFloat(input.dataset.max ?? "1"),
            sliderTaper,
          )
          : parseFloat(input.value);
        // A blend knob laid out as a slider shows its parameter's scale; the engine takes 0..1.
        const blendSpec = input.dataset.blendParam === "true"
          ? { min: parseFloat(input.dataset.blendSpecMin ?? "0"), max: parseFloat(input.dataset.blendSpecMax ?? "10") }
          : null;
        const sentValue = blendSpec ? normalizeBlendValue(value, blendSpec) : value;
        node.params[paramKey] = sentValue;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, sentValue);

        const sliderBlendState = blendSpec ? getBlendState(node) : null;
        if (sliderBlendState) {
          updateBlendMatchSummary(nodeParamsPanelElement, node, sliderBlendState);
        }

        // Update associated value display
        const parentControl = input.closest(".custom-layout-control");
        const valueEl = parentControl?.querySelector(".node-param-value") as HTMLElement | null;
        if (blendSpec && valueEl) {
          valueEl.textContent = value.toFixed(1);
        } else if (valueEl) {
          const paramDef = getNodeEffectInfo(node)?.parameters.find((p) => p.key === paramKey);
          if (paramDef) {
            const whole = wholeStepLabel(value, paramDef.unit, paramDef.step);
            if (sliderTaper === "log") {
              valueEl.textContent = formatTaperedValue(value, paramDef.unit);
            } else if (whole !== undefined) {
              valueEl.textContent = whole;
            } else if (paramDef.unit === "dB" || paramDef.unit === "ms" || paramDef.unit === "Hz") {
              valueEl.textContent = `${value.toFixed(1)}${paramDef.unit}`;
            } else if (paramDef.unit === "enum" && Array.isArray(paramDef.labels)) {
              valueEl.textContent = enumLabel(value, paramDef.labels, paramDef.min, paramDef.step) ?? `${Math.round(value)}`;
            } else {
              valueEl.textContent = value.toFixed(2);
            }
          }
        }
        refreshNodeVisualizations(node);
      }
    });
  });

  // Bind blend slider inputs (irBlend-style A/B range controls)
  const blendSliders = nodeParamsPanelElement?.querySelectorAll(".node-param-blend-slider");
  blendSliders?.forEach((sliderEl) => {
    const input = sliderEl as HTMLInputElement;
    enhanceRangeInput(input);
    input.addEventListener("input", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = parseFloat(input.value);
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);

        const valueEl = input.closest(".node-param-blend-group")?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueEl) {
          valueEl.textContent = value <= 0.01 ? "A" : value >= 0.99 ? "B" : `${Math.round(value * 100)}%`;
        }
        refreshNodeVisualizations(node);
      }
    });
  });

  const toggles = nodeParamsPanelElement?.querySelectorAll(".node-param-toggle");
  toggles?.forEach((toggleEl) => {
    const input = toggleEl as HTMLInputElement;
    input.addEventListener("change", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = input.checked ? 1 : 0;
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);
        syncPitchShiftRange(node, nodeId, paramKey);

        // Handle standard toggle labels
        const valueLabel = input.closest(".node-param-group")?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueLabel) {
          valueLabel.textContent = input.checked ? "On" : "Off";
        }
        
        // Handle mixer mute labels
        const muteLabel = input.closest(".mixer-input-header")?.querySelector(".mixer-mute-label") as HTMLElement | null;
        if (muteLabel) {
          muteLabel.textContent = input.checked ? "Muted" : "Active";
        }

        refreshNodeVisualizations(node);
      }
    });
  });

  const knobs = nodeParamsPanelElement?.querySelectorAll(".node-param-knob");
  if (!knobs) {
    return;
  }

  const blendState = getBlendState(node);

  const findClosestBlendMappingForParam = (
    activeParamId: string,
    targetValue: number,
    target: Record<string, number>,
  ): BlendModelMapping | null => {
    if (!blendState) {
      return null;
    }

    let best: BlendModelMapping | null = null;
    let bestDelta = Number.POSITIVE_INFINITY;
    let bestSecondary = Number.POSITIVE_INFINITY;

    blendState.mappings.forEach((mapping) => {
      const params = buildParameterMapFromLegacy(mapping);
      const mappedValue = params[activeParamId];
      if (typeof mappedValue !== "number") {
        return;
      }

      const delta = Math.abs(mappedValue - targetValue);
      let secondary = 0;

      blendState.paramIds.forEach((paramId) => {
        if (paramId === activeParamId) {
          return;
        }
        const targetOther = target[paramId];
        const mappedOther = params[paramId];
        if (typeof targetOther !== "number" || typeof mappedOther !== "number") {
          secondary += 4;
          return;
        }
        const diff = mappedOther - targetOther;
        secondary += diff * diff;
      });

      const isBetter = delta < bestDelta - BLEND_MAPPING_EPS
        || (Math.abs(delta - bestDelta) <= BLEND_MAPPING_EPS && secondary < bestSecondary);
      if (isBetter) {
        best = mapping;
        bestDelta = delta;
        bestSecondary = secondary;
      }
    });

    return best;
  };

  knobs.forEach((knobElement) => {
    const knob = knobElement as HTMLElement;
    const valueDisplay = knob.parentElement?.querySelector(".node-param-value") as HTMLElement | null;
    
    const nodeId = knob.dataset.nodeId;
    const paramKey = knob.dataset.paramKey;
    const min = parseFloat(knob.dataset.min || "0");
    const max = parseFloat(knob.dataset.max || "1");
    const unit = knob.dataset.unit || "amount";
    const defaultValue = parseFloat(knob.dataset.default || knob.dataset.value || "0");
    const sensitivity = (max - min) / 200;
    const step = knob.dataset.step ? parseFloat(knob.dataset.step) : undefined;
    const labels = (knob.dataset.labels || "").split("|").filter(Boolean);
    const isEnum = unit === "enum" && labels.length > 0;
    const isBlendParam = knob.dataset.blendParam === "true";
    const taper = parseParamTaper(knob.dataset.taper);
    const blendSpecMin = knob.dataset.blendSpecMin ? parseFloat(knob.dataset.blendSpecMin) : 0;
    const blendSpecMax = knob.dataset.blendSpecMax ? parseFloat(knob.dataset.blendSpecMax) : 10;
    const blendMode = (knob.dataset.blendMode ?? "interpolate") as BlendMode;
    const blendSpec: BlendParamSpec | null = isBlendParam
      ? { id: paramKey ?? "", label: paramKey ?? "", min: blendSpecMin, max: blendSpecMax }
      : null;

    const snapValue = (rawValue: number): number => {
      // Read the range each time: it can follow another setting (see syncPitchShiftRange).
      const lo = parseFloat(knob.dataset.min || "0");
      const hi = parseFloat(knob.dataset.max || "1");
      const currentStep = knob.dataset.step ? parseFloat(knob.dataset.step) : undefined;
      const clamped = Math.max(lo, Math.min(hi, rawValue));
      if (!currentStep || currentStep <= 0) return clamped;
      const snapped = Math.round((clamped - lo) / currentStep) * currentStep + lo;
      return Math.max(lo, Math.min(hi, snapped));
    };

    const formatValue = (rawValue: number): string => {
      if (isEnum) {
        return enumLabel(rawValue, labels, min, step) ?? `${Math.round(rawValue)}`;
      }
      if (isBlendParam) {
        return rawValue.toFixed(1);
      }
      // Special formatting for pan values
      if (unit === "pan") {
        if (Math.abs(rawValue) < 0.01) return "C";
        return rawValue < 0 
          ? `L${Math.abs(rawValue * 100).toFixed(0)}`
          : `R${(rawValue * 100).toFixed(0)}`;
      }
      if (taper === "log") {
        return formatTaperedValue(rawValue, unit);
      }
      return wholeStepLabel(rawValue, unit, step) ?? `${rawValue.toFixed(2)}${unit === "amount" ? "" : unit}`;
    };

    const knobInstance = new GenericKnob({
      knobElement: knob,
      paramId: `${nodeId ?? "node"}_${paramKey ?? "param"}`,
      minValue: min,
      maxValue: max,
      defaultValue,
      displayFormat: (value) => formatValue(value),
      valueDisplay,
      labelElement: knob.parentElement?.querySelector(".node-param-label, .custom-control-label") as HTMLElement | null,
      sensitivity,
      stepValue: step,
      taper,
      sendParameter: false,
      onValueChange: (value) => {
        if (!nodeId || !paramKey) return;
        const finalValue = snapValue(value);
        if (finalValue !== value) {
          knobInstance.setValue(finalValue);
        }

        const normalizedValue = isBlendParam ? normalizeBlendValue(finalValue, blendSpec) : finalValue;

        if (isBlendParam && blendMode === "snap" && blendState) {
          const target: Record<string, number> = { ...node.params, [paramKey]: normalizedValue };
          const closest = findClosestBlendMappingForParam(paramKey, normalizedValue, target);
          if (closest) {
            const params = buildParameterMapFromLegacy(closest);
            let updated = false;
            blendState.paramIds.forEach((paramId) => {
              const mappedValue = params[paramId];
              if (typeof mappedValue === "number" && node.params[paramId] !== mappedValue) {
                node.params[paramId] = mappedValue;
                sendSignalPathNodeParamUpdate(nodeId, paramId, mappedValue);
                updated = true;
              }
            });
            if (updated) {
              requestNodeParamsPanel(node, preset);
            }
            return;
          }
        }

        node.params[paramKey] = normalizedValue;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, normalizedValue);
        syncPitchShiftRange(node, nodeId, paramKey);

        refreshNodeVisualizations(node);

        if (isBlendParam && blendState) {
          updateBlendParamIndicators(nodeParamsPanelElement, node, blendState);
          updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
        }
      },
    });

    // Store knob instance for live EQ curve sync
    if (paramKey) {
      nodeParamKnobs.set(paramKey, knobInstance);
    }
  });

  if (blendState) {
    updateBlendParamIndicators(nodeParamsPanelElement, node, blendState);
    updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
  }
}
