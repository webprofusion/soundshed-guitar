/**
 * The Channels choice in an effect panel's header: how the node takes a stereo input.
 *
 * "Follow" takes whatever layout arrives (docs/signal-chain.md, "Channel layout"). The mono
 * choices fold a stereo input to one channel (summed, or one side alone), run the effect mono
 * and put out mono: one amp after a stereo chorus, the way a single real amp would hear it, and
 * half the CPU of running a model on both sides. The engine rebuilds the chain for a change.
 *
 * An advanced control, shown only when Settings > Audio turns it on. Hiding it changes nothing
 * in the engine: a node's channel mode still applies.
 */

import { getAppSetting } from "../../appSettingsStore.js";
import { CHANNEL_MODE_CONTROL_SETTING } from "../../settings/keys.js";
import { escapeHtml } from "../../utils.js";
import type { GraphNode } from "../../types.js";
import { sendSignalPathNodeChannelMode } from "../commands.js";
import { requestNodeParamsRefresh } from "../render.js";
import { nodeParamsPanelElement } from "../state.js";

const CHANNEL_MODES: ReadonlyArray<{ value: string; label: string }> = [
  { value: "", label: "Follow" },
  { value: "mono", label: "Mono (sum)" },
  { value: "monoLeft", label: "Mono (left)" },
  { value: "monoRight", label: "Mono (right)" },
];

/** Input, output and splitter nodes pass what they are given; there is nothing to choose. */
const BOUNDARY_TYPES = new Set(["input", "output", "splitter"]);

function nodeChannelMode(node: GraphNode): string {
  return typeof node.channelMode === "string" ? node.channelMode : "";
}

export function renderChannelModeControl(node: GraphNode): string {
  if (!getAppSetting(CHANNEL_MODE_CONTROL_SETTING) || BOUNDARY_TYPES.has(node.type)) {
    return "";
  }

  const current = nodeChannelMode(node);
  const options = CHANNEL_MODES.map(({ value, label }) =>
    `<option value="${escapeHtml(value)}"${value === current ? " selected" : ""}>${escapeHtml(label)}</option>`
  ).join("");

  return `<select class="node-channel-mode-select" aria-label="Channels" title="How this effect takes a stereo input: follow it, or fold it to mono">${options}</select>`;
}

export function bindChannelModeControl(node: GraphNode): void {
  const select = nodeParamsPanelElement?.querySelector<HTMLSelectElement>(".node-channel-mode-select");
  if (!select) {
    return;
  }

  select.addEventListener("change", () => {
    const value = select.value;
    if (value === nodeChannelMode(node)) {
      return;
    }
    node.channelMode = value || undefined;
    sendSignalPathNodeChannelMode(node.id, value);
    requestNodeParamsRefresh();
  });
}
