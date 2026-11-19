/**
 * The resource picker on a node: which model or IR is loaded, what else is
 * available, and the prev/next navigation across the library.
 */

import { EffectGuids } from "../../effectGuids.js";
import { Features, isFeatureEnabled } from "../../featureFlags.js";
import { renderIcon } from "../../iconAssets.js";
import type { LayoutResourceControlDef } from "../../layoutRenderer.js";
import { EffectTypeRegistry } from "../../presetV2.js";
import type { EffectTypeInfo } from "../../presetV2.js";
import { resourceBrowserModal } from "../../resourceBrowser.js";
import { resolveResourceIdAlias } from "../../resourceDedup.js";
import { getLibraryResource } from "../../resourceLibrary.js";
import { isLibraryPickerType, libraryPickerControlsHtml, libraryPickerNavigation, librarySlotHasBrowseButton } from "../../resourceSlotMarkup.js";
import type { getBlendState} from "../../signalPathBlend.js";
import { renderBlendInfoHtml } from "../../signalPathBlend.js";
import type { GraphNode, LibraryResource, Preset } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { buildHostedPluginListHtml, buildHostedPluginLoadErrorHtml, buildHostedPluginLoadingIndicatorHtml } from "../hostedPlugins.js";
import { getNodeResourceDisplayName } from "../nodeLabels.js";
import { getNodeResourceAtIndex } from "../nodeResources.js";
import { collectPreferredNodeResourceIds, getDeduplicatedLibraryResources } from "../nodeTypes.js";
import { resolveResourceContextKey, resolveResourceNavigationCategoryHint } from "../resourceContext.js";
import type { IrAbSlots } from "./irAbPanel.js";
import { buildIrSlotListHtml } from "./irSlotList.js";

/**
 * Warms the resource browser's navigation cache so a node's prev/next buttons
 * work without opening the browser first. Fire-and-forget: never blocks render.
 */
export function preloadResourceNavigationCaches(node: GraphNode, preset: Preset, typeInfo: EffectTypeInfo | undefined): void {
  const navCacheRequests = new Map<string, { resourceType: "nam" | "ir"; categoryHint?: string; contextKey: string }>();
  if (typeInfo?.requiresResource) {
    const rt = typeInfo.resourceType;
    if (rt === "nam" || rt === "ir") {
      const categoryHint = resolveResourceNavigationCategoryHint(node, preset, rt);
      // The cabinet's IR B steps through a list of its own.
      const slotCount = EffectTypeRegistry.resolve(node.type) === EffectGuids.kCabIr ? 2 : 1;
      for (let index = 0; index < slotCount; index++) {
        const contextKey = resolveResourceContextKey(node, rt, index);
        navCacheRequests.set(`${rt}:${categoryHint ?? ""}:${contextKey}`, { resourceType: rt, categoryHint, contextKey });
      }
    }
  }
  (typeInfo?.exposedResources ?? []).forEach((er) => {
    if (er.resourceType === "nam" || er.resourceType === "ir") {
      const resourceType = er.resourceType as "nam" | "ir";
      const categoryHint = resolveResourceNavigationCategoryHint(node, preset, resourceType);
      const contextKey = resolveResourceContextKey(node, resourceType);
      navCacheRequests.set(`${resourceType}:${categoryHint ?? ""}`, { resourceType, categoryHint, contextKey });
    }
  });
  navCacheRequests.forEach(({ resourceType, categoryHint, contextKey }) => {
    resourceBrowserModal.preloadLibraryNavigationCache(resourceType, { categoryHint, contextKey });
  });
}

/** The cabinet's second IR only blends in, so an empty one is normal: it is asked for quietly. */
function isOptionalResourceSlot(node: GraphNode, resourceIndex: number): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kCabIr && resourceIndex > 0;
}

/** The resource slots for a node, plus any controls a custom layout can position. */
export interface NodeResourceSelector {
  html: string;
  layoutControls: LayoutResourceControlDef[];
  /** The IR Cabinet's two pickers and its IR list apart, for the side-by-side section (irAbPanel.ts). */
  cabIrPickers?: IrAbSlots;
}

/**
 * Builds the resource selector for a node: one slot per exposed resource for a
 * composite, otherwise a single slot (or an A/B pair when the node is blended).
 */
export function buildNodeResourceSelector(
  node: GraphNode,
  preset: Preset,
  typeInfo: EffectTypeInfo | undefined,
  blendState: ReturnType<typeof getBlendState>,
): NodeResourceSelector {
  // Build resource selector if this node type requires a resource,
  // or if a composite node surfaces inner resources.
  let resourceSelector = "";
  let cabIrPickers: IrAbSlots | undefined;
  const hideRedundantLibraryBrowseButton = !librarySlotHasBrowseButton(node.type);
  const customLayoutResourceControls: LayoutResourceControlDef[] = [];
  const exposedResources = typeInfo?.exposedResources ?? [];
  // The cabinet's two IRs are slots of the node itself, not of an inner node, so they step with
  // arrows as a lone slot does, and its IR list sits between them.
  const isCabIr = EffectTypeRegistry.resolve(node.type) === EffectGuids.kCabIr;
  if (exposedResources.length > 0) {
    const exposedSlots = exposedResources
      .map((exposedResource, exposedResourceIndex) => {
        const resourceType = exposedResource.resourceType;
        const resourceIndex = exposedResource.resourceIndex ?? exposedResourceIndex;
        const browseAccept = resourceType === "nam"
          ? ".nam,.json"
          : resourceType === "ir"
            ? ".wav"
            : resourceType === "wasm"
              ? ".wasm"
              : "*";
        const preferredResourceIds = collectPreferredNodeResourceIds(node, resourceType);
        const { resources, aliasById } = getDeduplicatedLibraryResources(resourceType, preferredResourceIds);
        const emptyDisplayName = resourceType === "ir"
          ? "No IR selected"
          : resourceType === "nam"
            ? "No model selected"
            : "No resource selected";
        const current = getNodeResourceAtIndex(node, resourceIndex);
        const resolvedCurrentId = resolveResourceIdAlias(current.id ?? "", aliasById);
        // Folder navigation lands a file path with no library id, so key the
        // label off either: id-only would render the empty state for it.
        const hasCurrentSelection = Boolean(current.id || current.filePath);
        const displayName = hasCurrentSelection
          ? getNodeResourceDisplayName(node, resourceIndex, resourceType)
          : "";
        const isMissing = Boolean(current.id)
          && !current.filePath
          && !getLibraryResource(resourceType, current.id);
        const isOptional = isOptionalResourceSlot(node, resourceIndex);
        const labelState = { resourceType, displayName, isMissing, isOptional };
        const canBrowseFile = exposedResource.allowBrowseFile ?? true;
        const isLibraryPicker = resourceType === "nam" || resourceType === "ir";
        const isPluginPicker = resourceType === "plugin";
        const navigationCategoryHint = isLibraryPicker
          ? resolveResourceNavigationCategoryHint(node, preset, resourceType)
          : undefined;
        const navigationContextKey = isLibraryPicker
          ? resolveResourceContextKey(node, resourceType, resourceIndex)
          : undefined;
        const navigation = isCabIr && isLibraryPickerType(resourceType)
          ? libraryPickerNavigation(resourceType, current.id ?? "", current.filePath ?? "", { categoryHint: navigationCategoryHint, contextKey: navigationContextKey })
          : null;
        const resourceOptions = resources.map((res: LibraryResource) => {
          const selected = resolvedCurrentId === res.id && !current.filePath ? "selected" : "";
          return `<option value="${escapeHtml(res.id)}" ${selected}>${escapeHtml(res.name)}</option>`;
        }).join("");
        const customOption = current.filePath
          ? `<option value="__custom__" selected>Custom: ${escapeHtml(current.filePath.split("/").pop())}</option>`
          : "";
        const hostedPluginOpenButton = resourceType === "plugin"
          ? `<button type="button" class="resource-picker-btn plugin-host-open-btn" data-node-id="${escapeHtml(node.id)}" ${hasCurrentSelection ? "" : "disabled"}>Open Plugin</button>`
          : "";
        const hostedPluginSelectionLabel = resourceType === "plugin"
          ? `<div class="plugin-host-selected-name" title="${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, resourceIndex, "plugin") : "No plugin selected")}">${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, resourceIndex, "plugin") : "No plugin selected")}</div>`
          : "";
        const hostedPluginLoadError = resourceType === "plugin"
          ? buildHostedPluginLoadErrorHtml(node, resourceIndex)
          : "";

        // The slot's controls: drawn here under its label, and drawn as they are by a custom
        // layout that places this slot (controlsHtml).
        const controlsHtml = isLibraryPickerType(resourceType)
          ? libraryPickerControlsHtml({
            nodeId: node.id,
            resourceType,
            resourceIndex,
            includeIndexAttr: true,
            exposedResourceId: exposedResource.resourceId,
            labelState,
            hasCurrentSelection,
            showBrowseButton: !hideRedundantLibraryBrowseButton,
            navigation,
            currentFilePath: current.filePath,
          })
          : `
            <div class="resource-controls">
              ${isPluginPicker ? `` : `
                <select
                  class="resource-selector resource-dropdown"
                  data-node-id="${escapeHtml(node.id)}"
                  data-resource-type="${escapeHtml(resourceType)}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                >
                  <option value="">${escapeHtml(emptyDisplayName)}</option>
                  ${resourceOptions}
                  ${customOption}
                </select>
                <button
                  class="resource-clear-btn"
                  data-node-id="${escapeHtml(node.id)}"
                  data-resource-type="${escapeHtml(resourceType)}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  title="Clear selected resource"
                  ${hasCurrentSelection ? "" : "disabled"}
                >${renderIcon("close", "resource-clear-icon")}</button>
              `}
              ${canBrowseFile ? `
                <button
                  class="resource-browse-btn"
                  data-node-id="${escapeHtml(node.id)}"
                  data-resource-type="${escapeHtml(resourceType)}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  data-accept="${browseAccept}"
                  title="Browse for file..."
                >${renderIcon(isPluginPicker ? "plus" : "folder", "resource-browse-icon")}</button>
              ` : ""}
              ${hostedPluginOpenButton}
              ${hostedPluginSelectionLabel}
              ${isPluginPicker ? buildHostedPluginLoadingIndicatorHtml(node, resourceIndex) : ""}
            </div>
            ${isPluginPicker ? buildHostedPluginListHtml(node, resourceIndex, exposedResource.resourceId) : ""}
            ${hostedPluginLoadError}
            ${current.filePath && !isPluginPicker ? `<div class="resource-path-info" title="${escapeHtml(current.filePath)}">${escapeHtml(current.filePath)}</div>` : ""}
          `;

        customLayoutResourceControls.push({
          resourceControlKey: `__resource__:${exposedResource.resourceId}:${resourceIndex}`,
          displayName: exposedResource.displayName || exposedResource.resourceId,
          resourceType,
          resourceIndex,
          exposedResourceId: exposedResource.resourceId,
          navigationCategoryHint,
          navigationContextKey,
          allowBrowseFile: canBrowseFile,
          currentResourceId: current.id,
          currentDisplayName: displayName,
          currentFilePath: current.filePath,
          isMissing,
          isOptional,
          controlsHtml,
        });

        return `
          <div class="node-resource-selector" data-node-id="${escapeHtml(node.id)}" data-resource-index="${resourceIndex}" data-resource-type="${escapeHtml(resourceType)}">
            <label>${escapeHtml(exposedResource.displayName || exposedResource.resourceId)}</label>
            ${controlsHtml}
          </div>
        `;
      });
    if (isCabIr && exposedSlots.length === 2) {
      cabIrPickers = { slots: [exposedSlots[0], exposedSlots[1]], list: buildIrSlotListHtml(node, preset) };
      resourceSelector = `${exposedSlots[0]}${cabIrPickers.list}${exposedSlots[1]}`;
    } else {
      resourceSelector = exposedSlots.join("");
    }
  } else if (typeInfo?.requiresResource && typeInfo.resourceType) {
    const resourceType = typeInfo.resourceType;
    const preferredResourceIds = collectPreferredNodeResourceIds(node, resourceType);
    const { resources, aliasById } = getDeduplicatedLibraryResources(resourceType, preferredResourceIds);
    const browseAccept = resourceType === "nam" ? ".nam,.json" : resourceType === "ir" ? ".wav" : "*";
    const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
    if (blendId) {
      resourceSelector = blendState
        ? renderBlendInfoHtml(node, blendState)
        : `
          <div class="node-resource-selector" data-node-id="${escapeHtml(node.id)}">
            <label>Blend</label>
            <div class="resource-controls">
              ${isFeatureEnabled(Features.BlendTools) ? `<button class="blend-open-btn" data-node-id="${escapeHtml(node.id)}">Edit Blend</button>` : ""}
            </div>
          </div>
        `;
    } else {

    const buildOptions = (currentId: string) => {
      const resolvedCurrentId = resolveResourceIdAlias(currentId, aliasById);
      return resources.map((res: LibraryResource) => {
        const selected = res.id === resolvedCurrentId ? "selected" : "";
        return `<option value="${escapeHtml(res.id)}" ${selected}>${escapeHtml(res.name)}</option>`;
      }).join("");
    };

    const buildSelector = (index: number, label: string, includeIndexAttr: boolean) => {
      const current = getNodeResourceAtIndex(node, index);
      const resourceOptions = buildOptions(current.id);
      const customOption = current.filePath
        ? `<option value="__custom__" selected>Custom: ${escapeHtml(current.filePath.split("/").pop())}</option>`
        : "";
      const indexAttr = includeIndexAttr ? `data-resource-index="${index}"` : "";
      const isPluginPicker = resourceType === "plugin";
      // Folder navigation lands a file path with no library id, so key the
      // label off either: id-only would render the empty state for it.
      const hasCurrentSelection = Boolean(current.id || current.filePath);
      const displayName = hasCurrentSelection ? getNodeResourceDisplayName(node, index) : "";
      const isMissing = Boolean(current.id)
        && !current.filePath
        && !getLibraryResource(resourceType, current.id);
      const isOptional = isOptionalResourceSlot(node, index);
      const labelState = { resourceType, displayName, isMissing, isOptional };
      const navResourceType = resourceType === "nam" || resourceType === "ir" ? resourceType : null;
      const navigationCategoryHint = navResourceType
        ? resolveResourceNavigationCategoryHint(node, preset, navResourceType)
        : undefined;
      const navigationContextKey = navResourceType
        ? resolveResourceContextKey(node, navResourceType, index)
        : undefined;
      const navOptions = { categoryHint: navigationCategoryHint, contextKey: navigationContextKey };
      const navigation = navResourceType
        ? libraryPickerNavigation(navResourceType, current.id ?? "", current.filePath ?? "", navOptions)
        : null;
      const hostedPluginOpenButton = resourceType === "plugin"
        ? `<button type="button" class="resource-picker-btn plugin-host-open-btn" data-node-id="${escapeHtml(node.id)}" ${hasCurrentSelection ? "" : "disabled"}>Open Plugin</button>`
        : "";
      const hostedPluginSelectionLabel = resourceType === "plugin"
        ? `<div class="plugin-host-selected-name" title="${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, index, "plugin") : "No plugin selected")}">${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, index, "plugin") : "No plugin selected")}</div>`
        : "";
      const hostedPluginLoadError = resourceType === "plugin"
        ? buildHostedPluginLoadErrorHtml(node, index)
        : "";

      // The slot's controls: drawn here under its label, and drawn as they are by a custom
      // layout that places this slot (controlsHtml).
      const controlsHtml = navResourceType
        ? libraryPickerControlsHtml({
          nodeId: node.id,
          resourceType: navResourceType,
          resourceIndex: index,
          includeIndexAttr,
          labelState,
          hasCurrentSelection,
          showBrowseButton: !hideRedundantLibraryBrowseButton,
          navigation,
          currentFilePath: current.filePath,
        })
        : `
          <div class="resource-controls">
            ${isPluginPicker ? `` : `
              <select
                class="resource-dropdown"
                data-node-id="${escapeHtml(node.id)}"
                data-resource-type="${escapeHtml(resourceType)}"
                ${indexAttr}
              >
                <option value="">-- Select from Library --</option>
                ${resourceOptions}
                ${customOption}
              </select>
              <button
                class="resource-clear-btn"
                data-node-id="${escapeHtml(node.id)}"
                data-resource-type="${escapeHtml(resourceType)}"
                ${indexAttr}
                title="Clear selected resource"
                ${hasCurrentSelection ? "" : "disabled"}
              >${renderIcon("close", "resource-clear-icon")}</button>
            `}
            <button
                class="resource-browse-btn"
                data-node-id="${escapeHtml(node.id)}"
                data-resource-type="${escapeHtml(resourceType)}"
                ${indexAttr}
                data-accept="${browseAccept}"
                title="Browse for file..."
              >${renderIcon(isPluginPicker ? "plus" : "folder", "resource-browse-icon")}</button>
            ${hostedPluginOpenButton}
            ${hostedPluginSelectionLabel}
            ${isPluginPicker ? buildHostedPluginLoadingIndicatorHtml(node, index) : ""}
          </div>
          ${isPluginPicker ? buildHostedPluginListHtml(node, index) : ""}
          ${hostedPluginLoadError}
          ${current.filePath && !isPluginPicker ? `<div class="resource-path-info" title="${escapeHtml(current.filePath)}">${escapeHtml(current.filePath)}</div>` : ""}
        `;

      customLayoutResourceControls.push({
        resourceControlKey: `__resource__:primary:${index}`,
        displayName: label,
        resourceType,
        resourceIndex: index,
        navigationCategoryHint,
        navigationContextKey,
        allowBrowseFile: true,
        currentResourceId: current.id,
        currentDisplayName: displayName,
        currentFilePath: current.filePath,
        isMissing,
        isOptional,
        controlsHtml,
      });

      return `
        <div class="node-resource-selector" data-node-id="${escapeHtml(node.id)}" data-resource-index="${index}" data-resource-type="${escapeHtml(resourceType)}">
          <label>${label}</label>
          ${controlsHtml}
        </div>
      `;
    };

      if (node.type === EffectGuids.kAmpNamBlend) {
        const items = (node as unknown as { resources?: unknown[] }).resources ?? [];
        const modelSelectors = items.length ? items.map((_, index) => {
          const paramValue = getNodeResourceAtIndex(node, index).parameterValue ?? index;
          return `
            ${buildSelector(index, `Model ${index + 1}`, true)}
            <div class="node-resource-meta">
              <label>Model ${index + 1} Value</label>
              <input class="resource-param-value" type="number" step="0.1" data-node-id="${escapeHtml(node.id)}" data-resource-index="${index}" value="${paramValue}" />
            </div>
          `;
        }).join("") : `
          ${buildSelector(0, "Model 1", true)}
          <div class="node-resource-meta">
            <label>Model 1 Value</label>
            <input class="resource-param-value" type="number" step="0.1" data-node-id="${escapeHtml(node.id)}" data-resource-index="0" value="0" />
          </div>
        `;
        resourceSelector = modelSelectors;
      } else if (node.type === EffectGuids.kCabIr) {
        const irSlotA = buildSelector(0, "IR A", true);
        const irSlotB = buildSelector(1, "IR B", true);
        // The IR list sits between the slots it drags onto.
        cabIrPickers = { slots: [irSlotA, irSlotB], list: buildIrSlotListHtml(node, preset) };
        resourceSelector = `${irSlotA}${cabIrPickers.list}${irSlotB}`;
      } else {
        resourceSelector = buildSelector(0, resourceType === "nam" ? "Model" : resourceType === "ir" ? "IR" : resourceType === "plugin" ? "Plugin" : "Resource", false);
      }
    }
  }
  return { html: resourceSelector, layoutControls: customLayoutResourceControls, cabIrPickers };
}
