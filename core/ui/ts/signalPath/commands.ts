import { uiState, setPresetDirty } from "../state.js";
import { postMessage } from "../bridge.js";
import {
  type SignalPathEdgeRef,
  type SignalPathNodeOptions,
} from "../fxSelector.js";
export function sendSignalPathNodeParamUpdate(nodeId: string, paramKey: string, value: number): void {
  const presetId = uiState.activePresetId ?? undefined;
  postMessage({
    type: "updateSignalPathNodeParam",
    nodeId,
    paramKey,
    value,
    ...(presetId ? { presetId } : {}),
  });
  setPresetDirty(true);
}

export function sendSignalPathNodeBypassUpdate(nodeId: string, presetId: string, bypassed: boolean): void {
  postMessage({
    type: "updateSignalPathNodeBypass",
    nodeId,
    presetId,
    bypassed,
  });
  setPresetDirty(true);
}

export function sendSignalPathNodeConfigUpdate(nodeId: string, key: string, value: string, persist = true, capture = false): void {
  postMessage({
    type: "updateSignalPathNodeConfig",
    nodeId,
    key,
    value,
    persist,
    capture,
  });
  if (persist && !capture) {
    setPresetDirty(true);
  }
}

export function sendNodeResourceUpdate(
  nodeId: string,
  resourceType: string,
  resourceId: string,
  filePath: string,
  resourceIndex?: number,
  parameterValue?: number,
  exposedResourceId?: string,
  openPluginEditorAfterLoad?: boolean,
): void {
  postMessage({
    type: "updateNodeResource",
    nodeId,
    resourceType,
    resourceId,
    filePath,
    resourceIndex,
    parameterValue,
    exposedResourceId,
    openPluginEditorAfterLoad,
  });
  setPresetDirty(true);
}

export function sendBrowseNodeResource(
  nodeId: string,
  resourceType: string,
  resourceIndex?: number,
  exposedResourceId?: string,
  openPluginEditorAfterLoad?: boolean,
): void {
  postMessage({
    type: "browseNodeResource",
    nodeId,
    resourceType,
    resourceIndex,
    exposedResourceId,
    openPluginEditorAfterLoad,
  });
}

export function sendSignalPathNodeReorder(nodeId: string, targetNodeId: string): void {
  postMessage({
    type: "reorderSignalPathNode",
    nodeId,
    targetNodeId,
  });
  setPresetDirty(true);
}

export function sendSignalPathNodeDelete(nodeId: string): void {
  postMessage({
    type: "deleteSignalPathNode",
    nodeId,
  });
  setPresetDirty(true);
}

export function sendReplaceSignalPathNode(
  nodeId: string,
  newEffectType: string,
  options?: SignalPathNodeOptions,
): void {
  postMessage({
    type: "replaceSignalPathNode",
    nodeId,
    newEffectType,
    config: options?.config,
    label: options?.label,
    category: options?.category,
    params: options?.params,
    resources: options?.resources,
  });
  setPresetDirty(true);
}

export function sendMoveSignalPathNodeToEdge(nodeId: string, edge: SignalPathEdgeRef): void {
  postMessage({
    type: "reorderSignalPathNode",
    nodeId,
    edge,
  });
  setPresetDirty(true);
}

export function sendCollapseParallelSplit(splitterId: string, mixerId: string): void {
  postMessage({
    type: "collapseSignalPathSplit",
    splitterId,
    mixerId,
  });
  setPresetDirty(true);
}
