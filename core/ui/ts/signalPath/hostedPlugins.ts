import { uiState, getSignalPathPreset } from "../state.js";
import type {
  GraphNode,
  LibraryResource,
  ResourceRef,
} from "../types.js";
import { setAppSetting } from "../bridge.js";
import { escapeHtml } from "../utils.js";
import { getNodeEffectInfo } from "../presetV2.js";
import { renderIcon } from "../iconAssets.js";
import { getUnsupportedPluginSelection, inferPluginFormat, type PluginResourceSupportInfo } from "../pluginSupport.js";
import { getLibraryResource } from "../resourceLibrary.js";
import {
  nodeParamsPanelElement,
} from "./state.js";
import { getNodeResourceAtIndex, getResourceBaseName, getSelectedSignalPathNode } from "./nodeResources.js";
import { updateEffectVisualization } from "./visualization.js";
export type HostedPluginLoadFailure = {
  selectionKey: string;
  resourceIndex?: number;
  resource: PluginResourceSupportInfo;
  message: string;
  errorCode?: string;
};

export const hostedPluginLoadFailures = new Map<string, HostedPluginLoadFailure>();

export type HostedPluginPendingLoad = {
  resourceIndex: number;
  startedAt: number;
};

export /** Hosted-plugin loads in flight, keyed by node id. Drives the inline loading indicator. */
const hostedPluginPendingLoads = new Map<string, HostedPluginPendingLoad>();

export /** Safety valve: never keep a loading indicator around longer than this. */
const HOSTED_PLUGIN_PENDING_LOAD_MAX_AGE_MS = 120000;

export const HOSTED_PLUGIN_FAVORITES_SETTING = "plugins.hostFavorites";

export const HOSTED_PLUGIN_NAME_COLLATOR = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });

export function handleHostedPluginResourceLoadFailed(payload: {
  nodeId?: string;
  resourceType?: string;
  resourceId?: string;
  filePath?: string;
  resourceIndex?: number;
  message?: string;
  errorCode?: string;
}): void {
  if (payload.resourceType && payload.resourceType !== "plugin") {
    return;
  }

  const nodeId = payload.nodeId ?? "";
  if (!nodeId) {
    return;
  }

  clearHostedPluginLoadPending(nodeId);

  const resource = (payload.resourceId ? getLibraryResource("plugin", payload.resourceId) : undefined)
    ?? ({ filePath: payload.filePath ?? "" } satisfies PluginResourceSupportInfo);
  const message = payload.message?.trim() || "The selected plugin cannot be hosted by this build.";
  const failure: HostedPluginLoadFailure = {
    selectionKey: getPluginResourceSelectionKey(payload.resourceId, payload.filePath),
    resourceIndex: typeof payload.resourceIndex === "number" ? payload.resourceIndex : undefined,
    resource,
    message,
    errorCode: payload.errorCode?.trim() || undefined,
  };
  hostedPluginLoadFailures.set(nodeId, failure);

  const selectedNode = getSelectedSignalPathNode(getSignalPathPreset());
  if (selectedNode?.id === nodeId && getPluginResourceIndex(selectedNode) !== null) {
    renderHostedPluginWarningIntoOpenPanel(nodeId, failure.resourceIndex, buildHostedPluginLoadErrorMarkup(failure));
    updateEffectVisualization(selectedNode);
  }
}

export function getPluginResourceIndex(node: GraphNode): number | null {
  const typeInfo = getNodeEffectInfo(node);
  if (typeInfo?.resourceType === "plugin") {
    return 0;
  }

  const exposedPluginResource = typeInfo?.exposedResources?.find((resource) => resource.resourceType === "plugin");
  if (exposedPluginResource) {
    return exposedPluginResource.resourceIndex ?? 0;
  }

  const resources = (node as unknown as { resources?: ResourceRef[] }).resources;
  if (Array.isArray(resources)) {
    const index = resources.findIndex((resource) => (resource.resourceType ?? resource.type) === "plugin");
    if (index >= 0) {
      return index;
    }
  }

  return null;
}

export function getPluginResourceSelectionKey(resourceId?: string, filePath?: string): string {
  if (resourceId) {
    return `id:${resourceId}`;
  }
  if (filePath) {
    return `file:${filePath.replace(/\\/g, "/").toLowerCase()}`;
  }
  return "";
}

export function getPluginResourceSupportInfoAtIndex(node: GraphNode, resourceIndex: number): PluginResourceSupportInfo | null {
  const current = getNodeResourceAtIndex(node, resourceIndex);
  if (current.id) {
    const resource = getLibraryResource("plugin", current.id);
    if (resource) {
      return resource;
    }
  }

  if (current.filePath) {
    return { filePath: current.filePath };
  }

  return null;
}

export function getHostedPluginLoadFailureForResource(node: GraphNode, resourceIndex: number): HostedPluginLoadFailure | null {
  const failure = hostedPluginLoadFailures.get(node.id);
  if (!failure) {
    return null;
  }

  if (failure.resourceIndex !== undefined && failure.resourceIndex !== resourceIndex) {
    return null;
  }

  const current = getNodeResourceAtIndex(node, resourceIndex);
  const currentSelectionKey = getPluginResourceSelectionKey(current.id, current.filePath);
  if (failure.selectionKey && currentSelectionKey && failure.selectionKey !== currentSelectionKey) {
    return null;
  }

  return failure;
}

export function buildHostedPluginLoadErrorMarkup(failure: HostedPluginLoadFailure): string {
  const unsupportedPlugin = getUnsupportedPluginSelection(failure.resource);
  const title = unsupportedPlugin ? "Selected Plugin Type Not Supported" : "Plugin Load Error";
  const baseDetail = failure.message.trim() || "The selected plugin cannot be hosted by this build.";
  const detail = failure.errorCode ? `${baseDetail} (code: ${failure.errorCode})` : baseDetail;
  return buildHostedPluginWarningMarkup(title, detail);
}

export function buildUnsupportedPluginWarningMarkup(resource: PluginResourceSupportInfo | null | undefined): string {
  const unsupportedPlugin = getUnsupportedPluginSelection(resource);
  if (!unsupportedPlugin) {
    return "";
  }

  return buildHostedPluginWarningMarkup(
    "Selected Plugin Type Not Supported",
    `${unsupportedPlugin.label} plugins cannot be hosted by this build.`,
  );
}

export function buildHostedPluginWarningMarkup(title: string, detail: string): string {
  return `
    <div class="plugin-host-load-error" role="status" aria-live="polite">
      <div class="plugin-host-load-error-title">${escapeHtml(title)}</div>
      <div class="plugin-host-load-error-detail">${escapeHtml(detail)}</div>
    </div>
  `;
}

export function buildHostedPluginLoadErrorHtml(node: GraphNode, resourceIndex: number): string {
  const failure = getHostedPluginLoadFailureForResource(node, resourceIndex);
  if (failure) {
    return buildHostedPluginLoadErrorMarkup(failure);
  }

  return buildUnsupportedPluginWarningMarkup(getPluginResourceSupportInfoAtIndex(node, resourceIndex));
}

export function renderHostedPluginWarningIntoOpenPanel(nodeId: string, resourceIndex: number | undefined, warningHtml: string): void {
  if (!nodeParamsPanelElement?.classList.contains("visible")) {
    return;
  }

  const pluginControls = Array.from(
    nodeParamsPanelElement.querySelectorAll<HTMLElement>(`.resource-dropdown[data-resource-type="plugin"], .resource-picker-btn[data-resource-type="plugin"], .plugin-host-list[data-resource-type="plugin"]`),
  );
  const targetControl = pluginControls.find((control) => {
    if (control.dataset.nodeId !== nodeId) {
      return false;
    }
    if (resourceIndex === undefined) {
      return true;
    }
    const controlResourceIndex = control.dataset.resourceIndex ? parseInt(control.dataset.resourceIndex, 10) : 0;
    return controlResourceIndex === resourceIndex;
  });
  const container = targetControl?.closest(".node-resource-selector");
  if (!container) {
    return;
  }

  container.querySelector(".plugin-host-load-error")?.remove();
  const anchorRow = container.querySelector(".plugin-host-list") ?? container.querySelector(".resource-controls");
  if (warningHtml) {
    anchorRow?.insertAdjacentHTML("afterend", warningHtml);
  }
}

export function clearInlineHostedPluginLoadError(source: Element): void {
  source.closest(".node-resource-selector")?.querySelector(".plugin-host-load-error")?.remove();
}

export function containsCaseInsensitive(text: string | undefined, token: string): boolean {
  return Boolean(text && token && text.toLowerCase().includes(token.toLowerCase()));
}

export function isBlockedHostedPluginLibraryEntry(resourceId: string): boolean {
  const resource = getLibraryResource("plugin", resourceId);
  if (!resource) {
    return false;
  }

  return containsCaseInsensitive(resource.name, "soundshed")
    || containsCaseInsensitive(resource.filePath, "soundshed")
    || containsCaseInsensitive(resource.id, "soundshed")
    || containsCaseInsensitive(resource.metadata?.pluginName, "soundshed")
    || containsCaseInsensitive(resource.metadata?.pluginIdentifier, "soundshed")
    || containsCaseInsensitive(resource.metadata?.pluginStableId, "soundshed");
}

export function getHostedPluginFavoriteIds(resources?: LibraryResource[]): Set<string> {
  const raw = uiState.appSettings?.[HOSTED_PLUGIN_FAVORITES_SETTING];
  if (!Array.isArray(raw)) {
    return new Set<string>();
  }

  const values = raw
    .filter((value): value is string => typeof value === "string")
    .map((value) => value.trim())
    .filter((value) => value.length > 0);

  if (!resources) {
    return new Set(values);
  }

  const validIds = new Set(resources.map((resource) => resource.id));
  return new Set(values.filter((value) => validIds.has(value)));
}

export function persistHostedPluginFavoriteIds(favoriteIds: Set<string>): void {
  const payload = Array.from(favoriteIds).sort((a, b) => HOSTED_PLUGIN_NAME_COLLATOR.compare(a, b));
  uiState.appSettings[HOSTED_PLUGIN_FAVORITES_SETTING] = payload;
  setAppSetting(HOSTED_PLUGIN_FAVORITES_SETTING, payload);
}

export function toggleHostedPluginFavorite(resourceId: string): void {
  const favorites = getHostedPluginFavoriteIds();
  if (favorites.has(resourceId)) {
    favorites.delete(resourceId);
  } else {
    favorites.add(resourceId);
  }
  persistHostedPluginFavoriteIds(favorites);
}

export function sortHostedPluginResources(resources: LibraryResource[]): LibraryResource[] {
  const favorites = getHostedPluginFavoriteIds(resources);
  return [...resources].sort((a, b) => {
    const aFavorite = favorites.has(a.id);
    const bFavorite = favorites.has(b.id);
    if (aFavorite !== bFavorite) {
      return aFavorite ? -1 : 1;
    }
    return HOSTED_PLUGIN_NAME_COLLATOR.compare(a.name, b.name);
  });
}

export function getHostedPluginPendingLoad(nodeId: string, resourceIndex: number): HostedPluginPendingLoad | null {
  const pending = hostedPluginPendingLoads.get(nodeId);
  if (!pending || pending.resourceIndex !== resourceIndex) {
    return null;
  }
  if (Date.now() - pending.startedAt > HOSTED_PLUGIN_PENDING_LOAD_MAX_AGE_MS) {
    hostedPluginPendingLoads.delete(nodeId);
    return null;
  }
  return pending;
}

export function setHostedPluginLoadingIndicatorVisible(nodeId: string, resourceIndex: number, visible: boolean): void {
  const indicators = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
    `.plugin-host-loading[data-node-id="${nodeId}"]`,
  );
  indicators?.forEach((indicator) => {
    const indicatorIndex = indicator.dataset.resourceIndex ? parseInt(indicator.dataset.resourceIndex, 10) : 0;
    if (indicatorIndex !== resourceIndex) {
      return;
    }
    indicator.hidden = !visible;
    indicator.closest(".node-resource-selector")
      ?.querySelector(".plugin-host-list")
      ?.classList.toggle("is-loading", visible);
  });
}

export function markHostedPluginLoadPending(nodeId: string, resourceIndex: number): void {
  hostedPluginPendingLoads.set(nodeId, { resourceIndex, startedAt: Date.now() });
  setHostedPluginLoadingIndicatorVisible(nodeId, resourceIndex, true);
}

export function clearHostedPluginLoadPending(nodeId: string): void {
  const pending = hostedPluginPendingLoads.get(nodeId);
  if (!pending) {
    return;
  }
  hostedPluginPendingLoads.delete(nodeId);
  setHostedPluginLoadingIndicatorVisible(nodeId, pending.resourceIndex, false);
}

export function handleHostedPluginResourceLoadCompleted(payload: {
  nodeId?: string;
  resourceType?: string;
}): void {
  if (payload.resourceType && payload.resourceType !== "plugin") {
    return;
  }
  if (payload.nodeId) {
    clearHostedPluginLoadPending(payload.nodeId);
  }
}

export function handleNodeResourceBrowseCancelled(payload: {
  nodeId?: string;
  resourceType?: string;
}): void {
  if (payload.resourceType !== "plugin") {
    return;
  }
  if (payload.nodeId) {
    clearHostedPluginLoadPending(payload.nodeId);
  }
}

export function buildHostedPluginLoadingIndicatorHtml(node: GraphNode, resourceIndex: number): string {
  const pending = getHostedPluginPendingLoad(node.id, resourceIndex);
  return `
    <div
      class="plugin-host-loading"
      data-node-id="${node.id}"
      data-resource-index="${resourceIndex}"
      role="status"
      aria-live="polite"
      ${pending ? "" : "hidden"}
    >
      <span class="plugin-host-loading-spinner" aria-hidden="true"></span>
      <span class="plugin-host-loading-label">Loading plugin…</span>
    </div>
  `;
}

export function buildHostedPluginListHtml(node: GraphNode, resourceIndex: number, exposedResourceId?: string): string {
  const resources = uiState.resourceLibrary["plugin"] || [];
  const sortedResources = sortHostedPluginResources(resources);
  const favoriteIds = getHostedPluginFavoriteIds(resources);
  const current = getNodeResourceAtIndex(node, resourceIndex);
  const isLoading = Boolean(getHostedPluginPendingLoad(node.id, resourceIndex));
  const exposedAttr = exposedResourceId ? ` data-exposed-resource-id="${escapeHtml(exposedResourceId)}"` : "";

  const rows = sortedResources.map((res: LibraryResource) => {
    const isSelected = current.id === res.id && !current.filePath;
    const isFavorite = favoriteIds.has(res.id);
    const format = inferPluginFormat(res);
    const formatLabel = format ? format.toUpperCase() : "";
    const path = res.filePath || "";
    const favoriteTitle = isFavorite ? "Remove from favorites" : "Add to favorites";
    return `
      <div
        class="plugin-host-item${isSelected ? " is-selected" : ""}${isFavorite ? " is-favorite" : ""}"
        data-node-id="${node.id}"
        data-resource-id="${escapeHtml(res.id)}"
        data-resource-index="${resourceIndex}"
        ${exposedAttr}
        role="button"
        aria-selected="${isSelected ? "true" : "false"}"
        tabindex="0"
        title="${escapeHtml(path || res.name)}"
      >
        <div class="plugin-host-item-info">
          <div class="plugin-host-item-name">
            <span class="plugin-host-item-title">${escapeHtml(res.name)}</span>
            ${formatLabel ? `<span class="plugin-host-item-format">${escapeHtml(formatLabel)}</span>` : ""}
          </div>
          ${path ? `<div class="plugin-host-item-path">${escapeHtml(path)}</div>` : ""}
        </div>
        <div class="plugin-host-item-actions">
          <button
            type="button"
            class="plugin-host-favorite-btn${isFavorite ? " is-favorite" : ""}"
            data-node-id="${node.id}"
            data-resource-id="${escapeHtml(res.id)}"
            data-resource-name="${escapeHtml(res.name)}"
            title="${favoriteTitle}"
            aria-label="${favoriteTitle}: ${escapeHtml(res.name)}"
            aria-pressed="${isFavorite ? "true" : "false"}"
          >${renderIcon("star", "plugin-host-favorite-icon")}</button>
          <button
            type="button"
            class="plugin-host-remove-btn"
            data-node-id="${node.id}"
            data-resource-id="${escapeHtml(res.id)}"
            data-resource-name="${escapeHtml(res.name)}"
            title="Remove plugin from library"
            aria-label="Remove ${escapeHtml(res.name)} from library"
          >${renderIcon("close", "plugin-host-remove-icon")}</button>
        </div>
      </div>
    `;
  }).join("");

  const customRow = current.filePath ? `
    <div class="plugin-host-item is-selected is-custom" title="${escapeHtml(current.filePath)}">
      <div class="plugin-host-item-info">
        <div class="plugin-host-item-name">
          <span class="plugin-host-item-title">${escapeHtml(getResourceBaseName(current.filePath))}</span>
          ${(() => {
            const format = inferPluginFormat({ filePath: current.filePath });
            return format ? `<span class="plugin-host-item-format">${escapeHtml(format.toUpperCase())}</span>` : "";
          })()}
        </div>
        <div class="plugin-host-item-path">${escapeHtml(current.filePath)}</div>
      </div>
    </div>
  ` : "";

  const emptyRow = !rows && !customRow
    ? `<div class="plugin-host-list-empty">No plugins added yet. Use the folder button to browse for a plugin.</div>`
    : "";

  return `
    <div
      class="plugin-host-list${isLoading ? " is-loading" : ""}"
      data-node-id="${node.id}"
      data-resource-type="plugin"
      data-resource-index="${resourceIndex}"
      role="listbox"
      aria-label="Hosted plugins"
      tabindex="0"
      ${exposedAttr}
    >
      ${rows}${customRow}${emptyRow}
    </div>
  `;
}
