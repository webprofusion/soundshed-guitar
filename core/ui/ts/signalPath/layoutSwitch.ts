import { uiState } from "../state.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import type {
  GraphNode,
  Preset,
} from "../types.js";
import { escapeHtml } from "../utils.js";
import { showNotification } from "../notifications.js";
import { getNodeEffectInfo } from "../presetV2.js";
import { renderIcon } from "../iconAssets.js";
import { findLayoutById, selectStandardControls } from "../layoutPreferences.js";
import type { EffectLayout } from "../layoutTypes.js";
import { closeLayoutPicker, hasSelectableLayouts, openLayoutPicker } from "../layoutPicker.js";
import { layoutDesigner } from "../layoutDesigner.js";
import {
  BLEND_PARAM_SPECS,
  getBlendState,
} from "../signalPathBlend.js";
import {
  nodeParamsPanelElement,
} from "./state.js";
import { buildNodeLayoutMatchText, getNodeDisplayName } from "./nodeLabels.js";
import { requestNodeParamsRefresh, requestSignalPathRender } from "./render.js";
/**
 * Header control for choosing how this effect is presented, as a split toggle: the
 * left half selects the standard auto-generated controls in one click, the right half
 * opens the layout picker for the custom layouts available for it. Whichever is
 * showing is lit. The picker is also the only route into the layout designer, so the
 * control stays available while the layout feature is on even before the effect has
 * any layouts of its own.
 */
export function renderLayoutSwitchButtonHtml(node: GraphNode, blendId: string, usingCustomLayout: boolean): string {
  const canDesign = isFeatureEnabled(Features.EffectLayout);
  if (!canDesign && !hasSelectableLayouts(node.type, blendId || undefined)) {
    return "";
  }
  const label = `Effect layout: ${usingCustomLayout ? "custom layout" : "standard controls"}`;
  const standardTitle = usingCustomLayout ? "Show the standard controls" : "Showing the standard controls";
  return `
    <div class="node-layout-split" role="group" aria-label="${label}">
      <button
        class="effect-visualization-toolbar-btn node-layout-split-btn node-layout-standard-btn${usingCustomLayout ? "" : " is-active"}"
        type="button"
        aria-pressed="${usingCustomLayout ? "false" : "true"}"
        title="${standardTitle}"
        aria-label="Standard controls"
      >
        ${renderIcon("sliders", "effect-visualization-toolbar-icon")}
      </button>
      <button
        class="effect-visualization-toolbar-btn node-layout-split-btn node-layout-switch-btn${usingCustomLayout ? " is-active" : ""}"
        data-node-id="${escapeHtml(node.id)}"
        data-effect-type="${escapeHtml(node.type)}"
        data-blend-id="${escapeHtml(blendId)}"
        type="button"
        aria-haspopup="dialog"
        aria-expanded="false"
        title="${label} — choose layout"
        aria-label="${label}. Choose effect layout"
      >
        ${renderIcon("layout", "effect-visualization-toolbar-icon layout-switch-icon")}
        ${renderIcon("chevron-down", "node-layout-split-caret")}
      </button>
    </div>
  `;
}

export function bindLayoutSwitchButton(node: GraphNode, preset: Preset): void {
  const standardButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".node-layout-standard-btn");
  standardButton?.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    closeLayoutPicker();
    const blendId = getBlendState(node)?.blend?.id || "";
    const changed = selectStandardControls({
      effectType: node.type,
      blendId: blendId || undefined,
      matchText: buildNodeLayoutMatchText(node),
      presetId: uiState.activePresetId,
    });
    if (changed) {
      requestNodeParamsRefresh();
      requestSignalPathRender();
    }
  });

  const button = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".node-layout-switch-btn");
  if (!button) {
    return;
  }
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const effectType = button.dataset.effectType || node.type;
    const blendId = button.dataset.blendId || "";
    openLayoutPicker(button, {
      effectType,
      blendId: blendId || undefined,
      nodeLabel: getNodeDisplayName(node),
      matchText: buildNodeLayoutMatchText(node),
      presetId: uiState.activePresetId,
      presetName: preset.name || "",
      onApplied: () => {
        requestNodeParamsRefresh();
        requestSignalPathRender();
      },
      onDesignLayout: isFeatureEnabled(Features.EffectLayout)
        ? (layoutId) => openLayoutDesignerForNode(node, effectType, blendId, layoutId)
        : undefined,
    });
  });
}

/**
 * Opens the layout designer for a node, from the layout picker.
 * `layoutId` is null to start a fresh auto-generated layout (so an effect type can
 * hold several), or an existing layout id to edit that one.
 */
export function openLayoutDesignerForNode(
  node: GraphNode,
  effectType: string,
  blendId: string,
  layoutId: string | null,
): void {
  let existingLayout: EffectLayout | null = null;
  if (layoutId) {
    existingLayout = findLayoutById(layoutId, effectType, blendId || undefined);
    if (!existingLayout) {
      showNotification("That layout is no longer available");
      return;
    }
  }

  // Resolve blend params so the designer shows all available controls
  let blendName = "";
  let blendParamDefs: Array<{ key: string; name: string; default: number; min: number; max: number; unit: string; step?: number }> | undefined;
  if (blendId) {
    const blendState = getBlendState(node);
    if (blendState) {
      blendName = blendState.blend?.name || blendId;
      // Include ALL blend param specs so every possible knob is available in the designer
      const allBlendParams = BLEND_PARAM_SPECS.map((spec) => ({
        key: spec.id,
        name: spec.label,
        default: 0,
        min: spec.min,
        max: spec.max,
        unit: "amount",
        step: 0.1,
      }));
      const typeInfo = getNodeEffectInfo(node);
      const baseParams = (typeInfo?.parameters || []).filter((p) => p.key !== "blend");
      blendParamDefs = [...allBlendParams, ...baseParams];
    }
  }

  layoutDesigner.open(effectType, existingLayout ?? undefined, {
    blendId: blendId || undefined,
    blendName: blendName || undefined,
    blendParamDefs,
  });
}
