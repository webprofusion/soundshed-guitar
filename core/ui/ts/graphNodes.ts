import type { GraphNode } from "./types.js";

/**
 * Predicates over a signal-graph node.
 *
 * Kept in a leaf module so consumers do not have to import signalPath.ts (and
 * drag its whole dependency tree, plus the app's central import cycle, along
 * with them) just to ask a one-line question about a node.
 */

/**
 * Nodes carry either an explicit `bypassed` flag or a legacy `enabled` flag,
 * depending on how old the preset is. Neither is on the public GraphNode type.
 */
export function isNodeBypassed(node: GraphNode): boolean {
  const flags = node as unknown as { bypassed?: unknown; enabled?: unknown };
  if (typeof flags.bypassed === "boolean") return flags.bypassed;
  if (typeof flags.enabled === "boolean") return !flags.enabled;
  return false;
}
