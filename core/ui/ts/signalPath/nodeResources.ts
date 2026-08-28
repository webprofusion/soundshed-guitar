import type {
  GraphNode,
  Preset,
} from "../types.js";
import {
  getSelectedNodeId,
} from "./state.js";
export function getResourceBaseName(filePath: string): string {
  const normalized = filePath.replace(/\\/g, "/");
  return normalized.split("/").pop() || filePath;
}

export function getSelectedSignalPathNode(preset: Preset | null | undefined): GraphNode | null {
  if (!getSelectedNodeId() || !preset?.graph) {
    return null;
  }

  return preset.graph.nodes.find((node) => node.id === getSelectedNodeId()) ?? null;
}

export function getNodeResourceAtIndex(node: GraphNode, index = 0): { id: string; filePath: string; parameterValue?: number } {
  const anyNode = node as unknown as {
    resources?: unknown;
  };

  if (Array.isArray(anyNode.resources)) {
    const res = anyNode.resources[index] as { id?: unknown; resourceId?: unknown; embeddedId?: unknown; filePath?: unknown; parameterValue?: unknown } | undefined;
    const id = typeof res?.id === "string"
      ? res.id
      : (typeof res?.resourceId === "string"
        ? res.resourceId
        : (typeof res?.embeddedId === "string" ? res.embeddedId : ""));
    const filePath = typeof res?.filePath === "string" ? res.filePath : "";
    const parameterValue = typeof res?.parameterValue === "number" ? res.parameterValue : undefined;
    return { id, filePath, parameterValue };
  }

  return { id: "", filePath: "" };
}
