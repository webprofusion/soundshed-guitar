/**
 * Signal chain checks a preset must pass before it can be published to Tone Sharing.
 *
 * One rule so far: nothing that adds gain-stage distortion may follow a cabinet. A drive
 * pedal, an amp head, or a NAM capture of either, placed after an IR cab, a Simple Cab or a
 * full-rig NAM capture (amp and cab together), distorts an already cab-filtered signal. That
 * sounds harsh and fizzy and is never how the gear is wired, so a preset built that way is
 * not published.
 *
 * Classification only uses what is known. A NAM model whose gear type the library cannot
 * tell is let through rather than guessed at, and studio or outboard captures (compressors,
 * EQs) are fine after a cab.
 */

import { EffectGuids } from "../effectGuids.js";
import { EffectTypeRegistry } from "../presetV2.js";
import { getLibraryResource } from "../resourceLibrary.js";
import type { GraphNode, Preset, SignalGraph } from "../types.js";
import { normalizeNamGearCategory } from "./chainRules.js";
import { buildGraphMaps } from "./graph.js";
import { getNodeResourceAtIndex } from "./nodeResources.js";
import { getNodeDisplayName } from "./nodeLabels.js";

export interface SignalChainIssue {
  rule: "gain-stage-after-cab";
  /** The drive, amp or amp/pedal NAM node that is in the wrong place. */
  nodeId: string;
  nodeName: string;
  /** The cab or full-rig node it follows. */
  cabNodeId: string;
  cabNodeName: string;
  /** Set when the problem is only in one of the preset's scenes. */
  sceneTitle?: string;
  message: string;
}

const NAM_TYPES = new Set<string>([
  EffectGuids.kAmpNam,
  EffectGuids.kAmpNamOptimized,
  EffectGuids.kAmpNamBlend,
  EffectGuids.kFxNam,
]);

const DRIVE_AND_AMP_TYPES = new Set<string>([
  EffectGuids.kOverdrive,
  EffectGuids.kDistortion,
  EffectGuids.kMultibandDistortion,
  EffectGuids.kFuzz,
  EffectGuids.kAmpBuiltin,
]);

type NamGearClass = "pedal" | "preamp" | "amp" | "full-rig" | "studio" | "";

/** What one of a NAM node's models captures, from the library row or what the preset saved about it. */
function classifyNamResource(node: GraphNode, index: number): NamGearClass {
  const resource = getNodeResourceAtIndex(node, index);
  if (!resource.id && !resource.filePath) {
    return "";
  }
  const library = resource.id ? getLibraryResource("nam", resource.id) : undefined;
  const metadata = library?.metadata ?? {};
  const source = resource.source ?? {};

  const gearType = String(metadata.gear_type ?? source.gear_type ?? "").trim().toLowerCase().replace(/[\s-]+/g, "_");
  const providerCategory = String(metadata.tone3000Category ?? metadata.gear ?? "").trim().toLowerCase();
  // Studio and outboard captures are compressors, EQs and the like: fine after a cab.
  if (gearType === "studio" || providerCategory === "outboard") {
    return "studio";
  }

  const category = normalizeNamGearCategory(String(library?.category ?? metadata.gear ?? source.category ?? ""));
  if (category === "cab") {
    return "";
  }
  if (category) {
    return category;
  }

  if (gearType.includes("cab")) return "full-rig";
  if (gearType === "pedal") return "pedal";
  if (gearType === "preamp") return "preamp";
  if (gearType === "amp" || gearType === "pedal_amp") return "amp";
  return "";
}

function namClasses(node: GraphNode): NamGearClass[] {
  const count = Math.max(1, node.resources?.length ?? 0);
  const classes: NamGearClass[] = [];
  for (let index = 0; index < count; index += 1) {
    classes.push(classifyNamResource(node, index));
  }
  return classes;
}

function isNamNode(node: GraphNode): boolean {
  return NAM_TYPES.has(EffectTypeRegistry.resolve(node.type));
}

/** True when the node shapes the signal as a speaker cabinet does: an IR cab, a Simple Cab or a full-rig capture. */
export function isCabStageNode(node: GraphNode): boolean {
  const type = EffectTypeRegistry.resolve(node.type);
  if (type === EffectGuids.kCabSimple) {
    return true;
  }
  if (type === EffectGuids.kCabIr) {
    const resource = getNodeResourceAtIndex(node, 0);
    if (!resource.id && !resource.filePath) {
      return false; // No IR loaded: nothing is filtering the signal.
    }
    const library = resource.id ? getLibraryResource("ir", resource.id) : undefined;
    const category = String(library?.category ?? resource.source?.category ?? "").trim().toLowerCase();
    return category !== "reverb";
  }
  if (isNamNode(node)) {
    return namClasses(node).includes("full-rig");
  }
  return false;
}

/** True when the node is a drive, an amp, or a NAM capture of an amp, preamp, pedal or full rig. */
export function isGainStageNode(node: GraphNode): boolean {
  const type = EffectTypeRegistry.resolve(node.type);
  if (DRIVE_AND_AMP_TYPES.has(type)) {
    return true;
  }
  if (isNamNode(node)) {
    return namClasses(node).some((gear) => gear === "pedal" || gear === "preamp" || gear === "amp" || gear === "full-rig");
  }
  return false;
}

function describeGainStage(node: GraphNode): string {
  const type = EffectTypeRegistry.resolve(node.type);
  if (type === EffectGuids.kAmpBuiltin) return "an amp";
  if (isNamNode(node)) {
    const classes = namClasses(node);
    if (classes.includes("pedal")) return "a pedal model";
    if (classes.includes("full-rig")) return "a full-rig model";
    return "an amp model";
  }
  return "a drive";
}

function describeCabStage(node: GraphNode): string {
  return isNamNode(node) ? "the full-rig model" : "the cab";
}

function findIssuesInGraph(graph: SignalGraph, sceneTitle?: string): SignalChainIssue[] {
  const { nodeById, outgoing } = buildGraphMaps(graph);
  const issues: SignalChainIssue[] = [];
  const reported = new Set<string>();

  for (const cab of graph.nodes) {
    if (!isCabStageNode(cab)) {
      continue;
    }
    const visited = new Set<string>([cab.id]);
    const queue: string[] = [cab.id];
    while (queue.length > 0) {
      const current = queue.shift() as string;
      for (const edge of outgoing.get(current) ?? []) {
        if (!edge.to || visited.has(edge.to)) {
          continue;
        }
        visited.add(edge.to);
        queue.push(edge.to);
        const node = nodeById.get(edge.to);
        if (!node || reported.has(node.id) || !isGainStageNode(node)) {
          continue;
        }
        reported.add(node.id);
        const nodeName = getNodeDisplayName(node);
        const cabNodeName = getNodeDisplayName(cab);
        const where = sceneTitle ? ` in scene "${sceneTitle}"` : "";
        issues.push({
          rule: "gain-stage-after-cab",
          nodeId: node.id,
          nodeName,
          cabNodeId: cab.id,
          cabNodeName,
          ...(sceneTitle ? { sceneTitle } : {}),
          message: `"${nodeName}" is ${describeGainStage(node)} after ${describeCabStage(cab)} "${cabNodeName}"${where}. `
            + "Drives and amps belong before the cab or full-rig model.",
        });
      }
    }
  }
  return issues;
}

/**
 * Every signal chain problem in the preset: its main chain first, then any scene chain that
 * has a problem the main chain does not.
 */
export function validatePresetSignalChain(preset: Preset): SignalChainIssue[] {
  const issues: SignalChainIssue[] = [];
  const seen = new Set<string>();
  const add = (found: SignalChainIssue[]): void => {
    for (const issue of found) {
      const key = `${issue.cabNodeId}>${issue.nodeId}`;
      if (!seen.has(key)) {
        seen.add(key);
        issues.push(issue);
      }
    }
  };

  if (preset.graph) {
    add(findIssuesInGraph(preset.graph));
  }
  for (const scene of preset.scenes ?? []) {
    if (scene?.graph) {
      add(findIssuesInGraph(scene.graph, scene.title || scene.id));
    }
  }
  return issues;
}

/** One line for a status bar or an error: the first problem, and how many more there are. */
export function summarizeSignalChainIssues(issues: SignalChainIssue[]): string {
  if (issues.length === 0) {
    return "";
  }
  const more = issues.length > 1 ? ` (${issues.length - 1} more chain problem${issues.length > 2 ? "s" : ""})` : "";
  return `${issues[0].message}${more}`;
}
