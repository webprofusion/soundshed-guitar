import type {
  GraphNode,
  Preset,
} from "../types.js";
import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { resourceBrowserModal, type ResourceNavigationResult as ResourceNavigationSelection } from "../resourceBrowser.js";
import { findMatchingResourcePickerLabel } from "../resourcePickerLabel.js";
import { getLibraryResource } from "../resourceLibrary.js";
import {
  nodeParamsPanelElement,
} from "./state.js";
import { sendBrowseNodeResource, sendNodeResourceUpdate } from "./commands.js";
import { buildUnsupportedPluginWarningMarkup, clearHostedPluginLoadPending, clearInlineHostedPluginLoadError, hostedPluginLoadFailures, markHostedPluginLoadPending, renderHostedPluginWarningIntoOpenPanel } from "./hostedPlugins.js";
import { getNodeResourceAtIndex } from "./nodeResources.js";
import { getLibraryResourceName, handleNamIrFileDrop, inferResourceTypeFromFile } from "./nodeTypes.js";
import { resolveResourceBrowserLibraryCategoryHint, resolveResourceBrowserTone3000CategoryFilter, resolveResourceContextKey, resolveResourceNavigationCategoryHint } from "./resourceContext.js";
/// Resource loads round-trip through the backend, and the params panel re-renders
/// from whatever the graph says at that moment. Without this, holding down
/// next/prev repeatedly recomputes "the one after the old resource" and appears
/// to stick. Entries are dropped as soon as the graph catches up.
export const pendingResourceNavSelections = new Map<string, { resourceId: string; filePath: string; at: number }>();

export const PENDING_RESOURCE_NAV_TTL_MS = 5000;

/// Navigation keys with a Tone3000 step in flight (fetch + import). Keyed the
/// same way as the pending selections so prev and next share one guard and
/// cannot race each other from the same starting resource.
export const inFlightTone3000NavKeys = new Set<string>();

export function buildResourceNavKey(
  nodeId: string,
  resourceType: string,
  resourceIndex: number,
  exposedResourceId: string | undefined,
): string {
  return `${nodeId}:${resourceType}:${resourceIndex}:${exposedResourceId ?? ""}`;
}

/// Remote capture artwork can fail to load (offline, image withdrawn): swap in
/// the stock equipment image rather than leaving an empty panel.
export function bindEquipmentImageFallback(): void {
  const images = nodeParamsPanelElement?.querySelectorAll<HTMLImageElement>(
    ".default-effect-shell-equipment-image[data-fallback-src]",
  ) ?? [];
  images.forEach((image) => {
    image.addEventListener("error", () => {
      const fallback = image.dataset.fallbackSrc;
      if (!fallback || image.src.endsWith(fallback)) {
        return;
      }
      delete image.dataset.fallbackSrc;
      image.src = fallback;
    }, { once: true });
  });
}

export function bindResourceControls(node: GraphNode, preset: Preset): void {
  const syncResourceNavigationButtons = (
    nodeId: string,
    resourceType: "nam" | "ir",
    resourceIndex: number,
    exposedResourceId: string | undefined,
    currentResourceId: string,
    currentFilePath: string,
  ): void => {
    const prevButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(
      `.resource-nav-btn[data-node-id="${nodeId}"][data-resource-type="${resourceType}"][data-resource-index="${resourceIndex}"][data-nav-direction="prev"]`,
    );
    const nextButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(
      `.resource-nav-btn[data-node-id="${nodeId}"][data-resource-type="${resourceType}"][data-resource-index="${resourceIndex}"][data-nav-direction="next"]`,
    );

    const navOptions = {
      categoryHint: resolveResourceNavigationCategoryHint(node, preset, resourceType),
      contextKey: resolveResourceContextKey(node, resourceType),
    };
    const tone3000NavActive = resourceBrowserModal.isTone3000NavigationActive(resourceType, navOptions);
    if (prevButton) {
      const prev = tone3000NavActive
        || Boolean(resourceBrowserModal.getAdjacentResourceSelection(resourceType, currentResourceId, currentFilePath, -1, navOptions));
      prevButton.disabled = !prev;
      prevButton.setAttribute("aria-disabled", prev ? "false" : "true");
    }
    if (nextButton) {
      const next = tone3000NavActive
        || Boolean(resourceBrowserModal.getAdjacentResourceSelection(resourceType, currentResourceId, currentFilePath, 1, navOptions));
      nextButton.disabled = !next;
      nextButton.setAttribute("aria-disabled", next ? "false" : "true");
    }

    const clearButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(
      `.resource-clear-btn[data-node-id="${nodeId}"][data-resource-type="${resourceType}"]`,
    ) ?? [];
    clearButtons.forEach((clearBtn) => {
      const clearResourceIndex = clearBtn.dataset.resourceIndex ? parseInt(clearBtn.dataset.resourceIndex, 10) : 0;
      const clearExposedResourceId = clearBtn.dataset.exposedResourceId;
      if (clearResourceIndex === resourceIndex && (clearExposedResourceId ?? "") === (exposedResourceId ?? "")) {
        clearBtn.disabled = !(currentResourceId || currentFilePath);
      }
    });
  };

  // Bind resource dropdowns
  const dropdowns = nodeParamsPanelElement?.querySelectorAll(".resource-dropdown") as NodeListOf<HTMLSelectElement> | null;
  dropdowns?.forEach((dropdown) => {
    dropdown.addEventListener("change", () => {
      const nodeId = dropdown.dataset.nodeId;
      const resourceType = dropdown.dataset.resourceType;
      const resourceId = dropdown.value;
      const resourceIndex = dropdown.dataset.resourceIndex ? parseInt(dropdown.dataset.resourceIndex, 10) : undefined;

      if (resourceType === "plugin") {
        if (nodeId) {
          hostedPluginLoadFailures.delete(nodeId);
        }
        clearInlineHostedPluginLoadError(dropdown);
        const selectedResource = resourceId && resourceId !== "__custom__"
          ? getLibraryResource("plugin", resourceId)
          : null;
        if (nodeId) {
          renderHostedPluginWarningIntoOpenPanel(nodeId, resourceIndex, buildUnsupportedPluginWarningMarkup(selectedResource));
        }
      }
      
      if (nodeId && resourceType && resourceId && resourceId !== "__custom__") {
        sendNodeResourceUpdate(
          nodeId,
          resourceType,
          resourceId,
          "",
          resourceIndex,
          undefined,
          undefined,
          false,
        );
      }
    });
  });

  const resourcePickers = nodeParamsPanelElement?.querySelectorAll(".resource-picker-btn, .resource-picker-label") as NodeListOf<HTMLElement> | null;
  resourcePickers?.forEach((picker) => {
    picker.addEventListener("click", () => {
      const nodeId = picker.dataset.nodeId;
      const resourceType = picker.dataset.resourceType as "nam" | "ir" | undefined;
      const resourceIndex = picker.dataset.resourceIndex ? parseInt(picker.dataset.resourceIndex, 10) : 0;
      const exposedResourceId = picker.dataset.exposedResourceId;
      if (!nodeId || !resourceType || (resourceType !== "nam" && resourceType !== "ir")) {
        return;
      }

      const current = getNodeResourceAtIndex(node, resourceIndex);
      const tone3000CategoryFilter = resourceType === "nam"
        ? resolveResourceBrowserTone3000CategoryFilter(node, preset)
        : undefined;
      const libraryCategoryHint = resourceType === "ir"
        ? resolveResourceBrowserLibraryCategoryHint(node, resourceType)
        : undefined;
      resourceBrowserModal.open({
        resourceType,
        currentId: current.id,
        nodeId,
        resourceIndex,
        exposedResourceId,
        libraryCategoryHint,
        tone3000CategoryFilter,
        contextKey: resolveResourceContextKey(node, resourceType),
        onSelect: (resourceId) => {
          // An explicit pick supersedes any in-flight next/prev step.
          pendingResourceNavSelections.set(
            buildResourceNavKey(nodeId, resourceType, resourceIndex, exposedResourceId),
            { resourceId, filePath: "", at: performance.now() },
          );
          sendNodeResourceUpdate(nodeId, resourceType, resourceId, "", resourceIndex, undefined, exposedResourceId);
          const label = getLibraryResourceName(resourceType, resourceId) || resourceId || "";
          const labelText = label || (resourceType === "ir" ? "No IR selected" : "No model selected");
          const labelCandidates = nodeParamsPanelElement?.querySelectorAll(
            `.resource-picker-label[data-node-id="${nodeId}"]`,
          ) as NodeListOf<HTMLElement> | null;
          const labelEl = findMatchingResourcePickerLabel(
            labelCandidates,
            nodeId,
            resourceType,
            resourceIndex,
            exposedResourceId,
          );

          if (labelEl) {
            labelEl.textContent = labelText;
            labelEl.title = labelText;
            const missing = Boolean(resourceId) && !getLibraryResource(resourceType, resourceId);
            labelEl.classList.toggle("is-missing", missing);
          }

          syncResourceNavigationButtons(nodeId, resourceType, resourceIndex, exposedResourceId, resourceId, "");
        },
      });
    });
  });

  const resourceNavButtons = nodeParamsPanelElement?.querySelectorAll(".resource-nav-btn") as NodeListOf<HTMLButtonElement> | null;
  resourceNavButtons?.forEach((navBtn) => {
    // This runs on every panel render, i.e. whenever the graph state we render
    // from has moved: retire the in-flight selection once it has landed.
    {
      const nodeId = navBtn.dataset.nodeId;
      const resourceType = navBtn.dataset.resourceType;
      const resourceIndex = navBtn.dataset.resourceIndex ? parseInt(navBtn.dataset.resourceIndex, 10) : 0;
      if (nodeId && resourceType) {
        const navKey = buildResourceNavKey(nodeId, resourceType, resourceIndex, navBtn.dataset.exposedResourceId);
        const pending = pendingResourceNavSelections.get(navKey);
        if (pending) {
          const live = getNodeResourceAtIndex(node, resourceIndex);
          const landed = (pending.resourceId && pending.resourceId === (live.id ?? ""))
            || (pending.filePath && pending.filePath === (live.filePath ?? ""));
          if (landed || (performance.now() - pending.at) >= PENDING_RESOURCE_NAV_TTL_MS) {
            pendingResourceNavSelections.delete(navKey);
          }
        }
      }
    }

    const applyResourceNavSelection = (
      nodeId: string,
      resourceType: "nam" | "ir",
      resourceIndex: number,
      exposedResourceId: string | undefined,
      navKey: string,
      next: ResourceNavigationSelection,
    ): void => {
      pendingResourceNavSelections.set(navKey, {
        resourceId: next.resourceId ?? "",
        filePath: next.filePath ?? "",
        at: performance.now(),
      });

      sendNodeResourceUpdate(
        nodeId,
        resourceType,
        next.resourceId ?? "",
        next.filePath ?? "",
        resourceIndex,
        undefined,
        exposedResourceId,
      );

      const nextResource = next.resourceId
        ? getLibraryResource(resourceType, next.resourceId)
        : undefined;
      // A just-imported resource is not in the library snapshot yet, so fall
      // back to the name the navigation step reported.
      const labelText = nextResource?.name
        || next.displayName
        || (next.resourceId ?? "")
        || (next.filePath?.split(/[\\/]/).pop() ?? "");

      const labelCandidates = nodeParamsPanelElement?.querySelectorAll(
        `.resource-picker-label[data-node-id="${nodeId}"]`,
      ) as NodeListOf<HTMLElement> | null;
      const labelEl = findMatchingResourcePickerLabel(
        labelCandidates,
        nodeId,
        resourceType,
        resourceIndex,
        exposedResourceId,
      );

      if (labelEl) {
        labelEl.textContent = labelText || (resourceType === "ir" ? "No IR selected" : "No model selected");
        labelEl.title = labelText || "";
        labelEl.classList.toggle("is-missing", Boolean(next.resourceId) && !nextResource && !next.displayName);
      }
      syncResourceNavigationButtons(nodeId, resourceType, resourceIndex, exposedResourceId, next.resourceId ?? "", next.filePath ?? "");
    };

    navBtn.addEventListener("click", () => {
      const nodeId = navBtn.dataset.nodeId;
      const resourceType = navBtn.dataset.resourceType as "nam" | "ir" | undefined;
      const resourceIndex = navBtn.dataset.resourceIndex ? parseInt(navBtn.dataset.resourceIndex, 10) : 0;
      const exposedResourceId = navBtn.dataset.exposedResourceId;
      const direction = navBtn.dataset.navDirection === "prev" ? -1 : 1;
      if (!nodeId || !resourceType || (resourceType !== "nam" && resourceType !== "ir")) {
        return;
      }

      // Step from the last selection this button requested while it is still in
      // flight, so repeated clicks keep advancing instead of recomputing the
      // same neighbour of a resource we already navigated away from.
      const navKey = buildResourceNavKey(nodeId, resourceType, resourceIndex, exposedResourceId);
      const snapshot = getNodeResourceAtIndex(node, resourceIndex);
      const pending = pendingResourceNavSelections.get(navKey);
      const current = pending && (performance.now() - pending.at) < PENDING_RESOURCE_NAV_TTL_MS
        ? { id: pending.resourceId, filePath: pending.filePath }
        : { id: snapshot.id ?? "", filePath: snapshot.filePath ?? "" };

      const navOptions = {
        categoryHint: resolveResourceNavigationCategoryHint(node, preset, resourceType),
        contextKey: resolveResourceContextKey(node, resourceType),
      };

      // Stepping a Tone3000 result set fetches (and imports) the neighbour, so
      // it runs asynchronously with the button held busy meanwhile.
      if (resourceBrowserModal.isTone3000NavigationActive(resourceType, navOptions)) {
        if (inFlightTone3000NavKeys.has(navKey)) {
          return;
        }
        inFlightTone3000NavKeys.add(navKey);
        navBtn.setAttribute("aria-busy", "true");

        void (async () => {
          try {
            const next = await resourceBrowserModal.stepTone3000Resource(
              resourceType,
              current.id ?? "",
              direction,
              navOptions,
            );
            if (next) {
              applyResourceNavSelection(nodeId, resourceType, resourceIndex, exposedResourceId, navKey, next);
            }
          } finally {
            inFlightTone3000NavKeys.delete(navKey);
            navBtn.removeAttribute("aria-busy");
          }
        })();
        return;
      }

      const next = resourceBrowserModal.getAdjacentResourceSelection(
        resourceType,
        current.id ?? "",
        current.filePath ?? "",
        direction,
        navOptions,
      );
      if (!next) {
        return;
      }

      applyResourceNavSelection(nodeId, resourceType, resourceIndex, exposedResourceId, navKey, next);
    });
  });
  
  // Bind browse buttons
  const browseBtns = nodeParamsPanelElement?.querySelectorAll(".resource-browse-btn") as NodeListOf<HTMLButtonElement> | null;
  browseBtns?.forEach((browseBtn) => {
    browseBtn.addEventListener("click", () => {
      const nodeId = browseBtn.dataset.nodeId;
      const resourceType = browseBtn.dataset.resourceType;
      const resourceIndex = browseBtn.dataset.resourceIndex ? parseInt(browseBtn.dataset.resourceIndex, 10) : undefined;
      const exposedResourceId = browseBtn.dataset.exposedResourceId;

      if (resourceType === "plugin") {
        if (nodeId) {
          hostedPluginLoadFailures.delete(nodeId);
          markHostedPluginLoadPending(nodeId, resourceIndex ?? 0);
        }
        clearInlineHostedPluginLoadError(browseBtn);
      }

      if (nodeId && resourceType) {
        sendBrowseNodeResource(
          nodeId,
          resourceType,
          resourceIndex,
          exposedResourceId,
          resourceType === "plugin",
        );
      }
    });
  });

  // Bind clear buttons
  const clearBtns = nodeParamsPanelElement?.querySelectorAll(".resource-clear-btn") as NodeListOf<HTMLButtonElement> | null;
  clearBtns?.forEach((clearBtn) => {
    clearBtn.addEventListener("click", () => {
      void (async () => {
        if (clearBtn.disabled) {
          return;
        }

        const nodeId = clearBtn.dataset.nodeId;
        const resourceType = clearBtn.dataset.resourceType;
        const resourceIndex = clearBtn.dataset.resourceIndex ? parseInt(clearBtn.dataset.resourceIndex, 10) : undefined;
        const exposedResourceId = clearBtn.dataset.exposedResourceId;
        const emptyLabel = clearBtn.dataset.emptyLabel || "No resource selected";

        if (!nodeId || !resourceType) {
          return;
        }

        pendingResourceNavSelections.delete(
          buildResourceNavKey(nodeId, resourceType, resourceIndex ?? 0, exposedResourceId),
        );

        let selectedPluginResourceId = "";
        if (resourceType === "plugin") {
          const current = getNodeResourceAtIndex(node, resourceIndex ?? 0);
          selectedPluginResourceId = current.id || "";
          if (selectedPluginResourceId) {
            const selectedPluginName = getLibraryResourceName("plugin", selectedPluginResourceId) || selectedPluginResourceId;
            const confirmed = await showConfirm(
              `Remove "${selectedPluginName}" from your plugin library? The plugin file on disk will not be deleted.`,
              "Remove Plugin",
            );
            if (!confirmed) {
              return;
            }
          }

          hostedPluginLoadFailures.delete(nodeId);
          clearInlineHostedPluginLoadError(clearBtn);
          clearHostedPluginLoadPending(nodeId);
          const listItems = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
            `.plugin-host-list[data-node-id="${nodeId}"] .plugin-host-item`,
          );
          listItems?.forEach((item) => item.classList.remove("is-selected"));
          const openButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(
            `.plugin-host-open-btn[data-node-id="${nodeId}"]`,
          );
          openButtons?.forEach((button) => {
            button.disabled = true;
          });
        }

        sendNodeResourceUpdate(nodeId, resourceType, "", "", resourceIndex, undefined, exposedResourceId);

        if (resourceType === "plugin" && selectedPluginResourceId) {
          postMessage({
            type: "cleanupResourceLibrary",
            scope: "plugin",
            removeFiles: false,
            resources: [{ type: "plugin", id: selectedPluginResourceId }],
          });
        }

        const dropdowns = nodeParamsPanelElement?.querySelectorAll<HTMLSelectElement>(
          `.resource-dropdown[data-node-id="${nodeId}"][data-resource-type="${resourceType}"]`,
        ) ?? [];
        dropdowns.forEach((dropdown) => {
          const controlResourceIndex = dropdown.dataset.resourceIndex ? parseInt(dropdown.dataset.resourceIndex, 10) : 0;
          const clearResourceIndex = resourceIndex ?? 0;
          const dropdownExposedResourceId = dropdown.dataset.exposedResourceId;
          if (controlResourceIndex === clearResourceIndex && (dropdownExposedResourceId ?? "") === (exposedResourceId ?? "")) {
            dropdown.value = "";
          }
        });

        const labelCandidates = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
          `.resource-picker-label[data-node-id="${nodeId}"]`,
        ) as NodeListOf<HTMLElement> | null;
        const pickerResourceType = resourceType === "nam" || resourceType === "ir" ? resourceType : null;
        const labelEl = pickerResourceType
          ? findMatchingResourcePickerLabel(
            labelCandidates,
            nodeId,
            pickerResourceType,
            resourceIndex ?? 0,
            exposedResourceId,
          )
          : null;

        if (labelEl) {
          labelEl.textContent = emptyLabel;
          labelEl.title = emptyLabel;
          labelEl.classList.remove("is-missing");
        }

        if (pickerResourceType) {
          syncResourceNavigationButtons(nodeId, pickerResourceType, resourceIndex ?? 0, exposedResourceId, "", "");
        }

        clearBtn.disabled = true;
      })();
    });
  });

  // Bind parameter value inputs for blend models
  const valueInputs = nodeParamsPanelElement?.querySelectorAll(".resource-param-value") as NodeListOf<HTMLInputElement> | null;
  valueInputs?.forEach((input) => {
    input.addEventListener("change", () => {
      const nodeId = input.dataset.nodeId;
      const resourceIndex = input.dataset.resourceIndex ? parseInt(input.dataset.resourceIndex, 10) : undefined;
      const value = parseFloat(input.value);
      if (nodeId && resourceIndex !== undefined && !Number.isNaN(value)) {
        sendNodeResourceUpdate(nodeId, "nam", "", "", resourceIndex, value);
      }
    });
  });

  // Bind per-slot file drop on NAM/IR resource selector rows
  const resourceSelectorEls = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
    ".node-resource-selector[data-resource-index][data-resource-type]",
  ) ?? [];
  resourceSelectorEls.forEach((selectorEl) => {
    const elResourceType = selectorEl.dataset.resourceType as "nam" | "ir" | undefined;
    const elResourceIndex = selectorEl.dataset.resourceIndex !== undefined
      ? parseInt(selectorEl.dataset.resourceIndex, 10)
      : undefined;
    const elNodeId = selectorEl.dataset.nodeId;
    if (!elNodeId || (elResourceType !== "nam" && elResourceType !== "ir") || elResourceIndex === undefined) {
      return;
    }

    selectorEl.addEventListener("dragover", (e: DragEvent) => {
      if (!Array.from(e.dataTransfer?.types ?? []).includes("Files")) return;
      e.preventDefault();
      e.stopPropagation();
      if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
      selectorEl.classList.add("drag-over");
    });

    selectorEl.addEventListener("dragleave", (e: DragEvent) => {
      if (!selectorEl.contains(e.relatedTarget as Node | null)) {
        selectorEl.classList.remove("drag-over");
      }
    });

    selectorEl.addEventListener("drop", (e: DragEvent) => {
      if (!Array.from(e.dataTransfer?.types ?? []).includes("Files")) return;
      e.preventDefault();
      e.stopPropagation();
      selectorEl.classList.remove("drag-over");
      const files = Array.from(e.dataTransfer?.files ?? []);
      const file = files[0];
      if (!file) return;
      const resourceType = inferResourceTypeFromFile(file);
      if (resourceType !== elResourceType) return;
      void handleNamIrFileDrop(file, elNodeId, elResourceIndex);
    });
  });
}
