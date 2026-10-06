/**
 * The buttons that act on the node as a whole rather than on one parameter.
 */

import { openCustomEffectDesigner } from "../../customEffectDesigner.js";
import { getBlendState, updateBlendMatchSummary } from "../../signalPathBlend.js";
import type { GraphNode, Preset } from "../../types.js";
import { toggleSignalPathNodeBypass } from "../bypass.js";
import { sendSignalPathNodeConfigUpdate, sendSignalPathNodeRename } from "../commands.js";
import { promptSaveCurrentCustomEffect } from "../customEffectActions.js";
import { getNodeAutomaticName, getNodeUserTitle } from "../nodeLabels.js";
import { requestNodeParamsRefresh, requestSignalPathRender } from "../render.js";
import { nodeParamsPanelElement } from "../state.js";

export function bindCustomEffectActionControls(node: GraphNode): void {
  const designButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-design-btn");
  designButton?.addEventListener("click", () => {
    void openCustomEffectDesigner(node);
  });

  const saveButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-save-btn");
  saveButton?.addEventListener("click", () => {
    promptSaveCurrentCustomEffect(node, false);
  });

  const useButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-use-btn");
  useButton?.addEventListener("click", () => {
    promptSaveCurrentCustomEffect(node, true);
  });
}

/** The longest name the title field takes; the engine caps a title in bytes as well. */
const NODE_TITLE_MAX_LENGTH = 64;

/**
 * The pencil beside the panel title swaps the title for a text field. Enter or leaving
 * the field keeps the name, Escape abandons it, and an empty field, or the automatic
 * name typed back, gives the node its automatic name again.
 */
export function bindNodeTitleEdit(node: GraphNode): void {
  const editButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".default-effect-shell-title-edit");
  const titleText = nodeParamsPanelElement?.querySelector<HTMLElement>(".default-effect-shell-title-text");
  if (!editButton || !titleText) {
    return;
  }

  editButton.addEventListener("click", () => {
    const automaticName = getNodeAutomaticName(node);
    const input = document.createElement("input");
    input.type = "text";
    input.className = "default-effect-shell-title-input";
    input.maxLength = NODE_TITLE_MAX_LENGTH;
    input.value = getNodeUserTitle(node) || automaticName;
    input.placeholder = automaticName;
    input.setAttribute("aria-label", "Effect name");
    input.spellcheck = false;

    let settled = false;
    const finish = (keep: boolean): void => {
      if (settled) {
        return;
      }
      settled = true;
      const typed = input.value.trim();
      const nextTitle = typed === automaticName ? "" : typed;
      if (keep && nextTitle !== getNodeUserTitle(node)) {
        node.title = nextTitle;
        sendSignalPathNodeRename(node.id, nextTitle);
        requestSignalPathRender();
      }
      // A redraw puts the title and its pencil back, renamed or not.
      requestNodeParamsRefresh();
    };

    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        finish(true);
      } else if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        finish(false);
      }
    });
    input.addEventListener("blur", () => finish(true));

    titleText.replaceWith(input);
    editButton.hidden = true;
    input.focus();
    input.select();
  });
}

export function bindBypassButton(node: GraphNode, preset: Preset): void {
  const bypassButtons = document.querySelectorAll<HTMLButtonElement>("#node-params-panel .node-bypass-btn");
  bypassButtons.forEach((bypassBtn) => {
    bypassBtn.addEventListener("click", () => {
      toggleSignalPathNodeBypass(node, preset);
    });
  });
}

export function bindBlendModeOverride(node: GraphNode): void {
  const select = nodeParamsPanelElement?.querySelector<HTMLSelectElement>(".blend-mode-select");
  if (!select) {
    return;
  }
  select.addEventListener("change", () => {
    // The node's own choice, kept apart from the definition's blendMode so it survives the
    // blend being applied again. "" follows the definition.
    const value = select.value;
    node.config.blendModeOverride = value;
    sendSignalPathNodeConfigUpdate(node.id, "blendModeOverride", value);
    // The knobs snap in the UI too, so they are redrawn for the new mode.
    const blendState = getBlendState(node);
    if (blendState) {
      updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
    }
    requestNodeParamsRefresh();
  });
}
