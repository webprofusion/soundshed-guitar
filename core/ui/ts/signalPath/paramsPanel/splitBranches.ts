/**
 * The splitter's Branches control: how many parallel branches its split has.
 *
 * The count is the splitter's outgoing edges; no param stores it. The engine
 * (`setSplitBranchCount`) adds empty branches and removes only empty ones, so a
 * count below the branches that hold effects is offered disabled rather than
 * refused after the click.
 */

import { EffectGuids } from "../../effectGuids.js";
import type { GraphNode, Preset } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { sendSetSplitBranchCount } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";

export const MIN_SPLIT_BRANCHES = 2;
/** One branch per mixer input (MixerEffect::kMaxInputs). */
export const MAX_SPLIT_BRANCHES = 4;

export interface SplitBranchState {
  count: number;
  /** The fewest branches the split can have without deleting an effect. */
  minCount: number;
}

/** Null for anything but a splitter. */
export function getSplitBranchState(node: GraphNode, preset: Preset): SplitBranchState | null {
  if (node.type !== EffectGuids.kSplitter) {
    return null;
  }
  const nodes = preset.graph?.nodes ?? [];
  const branches = (preset.graph?.edges ?? []).filter((edge) => edge.from === node.id);
  // An empty branch runs from the splitter straight into the mixer.
  const occupied = branches.filter((edge) => nodes.find((n) => n.id === edge.to)?.type !== EffectGuids.kMixer).length;
  return { count: branches.length, minCount: Math.max(MIN_SPLIT_BRANCHES, occupied) };
}

export function buildSplitBranchControlsHtml(node: GraphNode, preset: Preset): string {
  const state = getSplitBranchState(node, preset);
  if (!state) {
    return "";
  }
  const options = [];
  for (let count = MIN_SPLIT_BRANCHES; count <= MAX_SPLIT_BRANCHES; count++) {
    const selected = count === state.count;
    const blocked = count < state.minCount;
    const title = blocked
      ? "Take the effects off a branch first: only empty branches are removed"
      : `${count} parallel branches`;
    options.push(
      `<button type="button" class="split-branch-option${selected ? " is-selected" : ""}" data-branch-count="${count}" aria-pressed="${selected}"${blocked ? " disabled" : ""} title="${escapeHtml(title)}">${count}</button>`,
    );
  }
  return `
    <div class="mixer-inputs-section split-branches-section" data-node-id="${escapeHtml(node.id)}">
      <div class="mixer-inputs-header">Branches</div>
      <div class="split-branch-options" role="group" aria-label="Parallel branches">${options.join("")}</div>
      <div class="split-branch-hint">New branches start empty. Only an empty branch can be removed.</div>
    </div>
  `;
}

export function bindSplitBranchControls(node: GraphNode, preset: Preset): void {
  const state = getSplitBranchState(node, preset);
  if (!state || !nodeParamsPanelElement) {
    return;
  }
  nodeParamsPanelElement.querySelectorAll<HTMLButtonElement>(".split-branch-option").forEach((button) => {
    button.addEventListener("click", () => {
      const count = Number(button.dataset.branchCount);
      if (button.disabled || count === state.count) {
        return;
      }
      sendSetSplitBranchCount(node.id, count);
    });
  });
}
