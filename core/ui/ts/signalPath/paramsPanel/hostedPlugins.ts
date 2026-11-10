/**
 * Third-party plugin nodes: the scan list, and opening a plugin's own editor.
 */

import { postMessage } from "../../bridge.js";
import { showConfirm } from "../../dialogs.js";
import { showNotification } from "../../notifications.js";
import { getLibraryResource } from "../../resourceLibrary.js";
import type { GraphNode } from "../../types.js";
import { sendNodeResourceUpdate, sendSignalPathNodeConfigUpdate } from "../commands.js";
import { buildHostedPluginWarningMarkup, buildUnsupportedPluginWarningMarkup, clearInlineHostedPluginLoadError, hostedPluginLoadFailures, isBlockedHostedPluginLibraryEntry, markHostedPluginLoadPending, renderHostedPluginWarningIntoOpenPanel, toggleHostedPluginFavorite } from "../hostedPlugins.js";
import { getNodeResourceAtIndex } from "../nodeResources.js";
import { requestNodeParamsRefresh } from "../render.js";
import { nodeParamsPanelElement } from "../state.js";

export function bindHostedPluginActionControls(node: GraphNode): void {
  const openButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(".plugin-host-open-btn");
  openButtons?.forEach((openButton) => openButton.addEventListener("click", () => {
    sendSignalPathNodeConfigUpdate(node.id, "showPluginEditor", "1", false);
  }));
}

export function bindHostedPluginListControls(node: GraphNode): void {
  const lists = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(".plugin-host-list");
  lists?.forEach((list) => {
    const nodeId = list.dataset.nodeId;
    const resourceIndex = list.dataset.resourceIndex ? parseInt(list.dataset.resourceIndex, 10) : 0;
    const exposedResourceId = list.dataset.exposedResourceId;
    if (!nodeId) {
      return;
    }

    const selectedItem = list.querySelector<HTMLElement>(".plugin-host-item.is-selected");
    selectedItem?.scrollIntoView({ block: "nearest" });

    const getItems = (): HTMLElement[] => Array.from(
      list.querySelectorAll<HTMLElement>(".plugin-host-item[data-resource-id]"),
    );

    const focusPluginListItem = (item: HTMLElement | null): void => {
      if (!item) {
        return;
      }
      item.focus();
      item.scrollIntoView({ block: "nearest" });
    };

    const focusItemByOffset = (origin: HTMLElement | null, offset: number): void => {
      const items = getItems();
      if (!items.length) {
        return;
      }

      const currentIndex = origin ? items.indexOf(origin) : -1;
      const nextIndex = currentIndex < 0
        ? (offset > 0 ? 0 : items.length - 1)
        : Math.max(0, Math.min(items.length - 1, currentIndex + offset));
      focusPluginListItem(items[nextIndex]);
    };

    const focusBoundaryItem = (first: boolean): void => {
      const items = getItems();
      if (!items.length) {
        return;
      }
      focusPluginListItem(first ? items[0] : items[items.length - 1]);
    };

    list.addEventListener("keydown", (event) => {
      if (list.classList.contains("is-loading")) {
        return;
      }

      const key = event.key;
      if (key !== "ArrowDown" && key !== "ArrowUp" && key !== "Home" && key !== "End") {
        return;
      }

      const activeElement = document.activeElement as HTMLElement | null;
      const activeItem = activeElement?.closest<HTMLElement>(".plugin-host-item[data-resource-id]") ?? null;
      event.preventDefault();

      if (key === "ArrowDown") {
        focusItemByOffset(activeItem, 1);
      } else if (key === "ArrowUp") {
        focusItemByOffset(activeItem, -1);
      } else if (key === "Home") {
        focusBoundaryItem(true);
      } else if (key === "End") {
        focusBoundaryItem(false);
      }
    });

    list.querySelectorAll<HTMLElement>(".plugin-host-item[data-resource-id]").forEach((item) => {
      item.addEventListener("click", () => {
        if (list.classList.contains("is-loading")) {
          return;
        }
        const resourceId = item.dataset.resourceId;
        if (!resourceId || item.classList.contains("is-selected")) {
          return;
        }

        if (isBlockedHostedPluginLibraryEntry(resourceId)) {
          showNotification(
            "Blocked plugin",
            "Sepia plugins cannot be loaded in the hosted plugin slot. Remove this entry if it is invalid.",
          );
          renderHostedPluginWarningIntoOpenPanel(
            nodeId,
            resourceIndex,
            buildHostedPluginWarningMarkup(
              "Blocked Plugin",
              "Sepia plugins cannot be loaded in the hosted plugin slot.",
            ),
          );
          return;
        }

        hostedPluginLoadFailures.delete(nodeId);
        clearInlineHostedPluginLoadError(list);
        renderHostedPluginWarningIntoOpenPanel(
          nodeId,
          resourceIndex,
          buildUnsupportedPluginWarningMarkup(getLibraryResource("plugin", resourceId)),
        );

        list.querySelectorAll(".plugin-host-item").forEach((el) => el.classList.toggle("is-selected", el === item));
        const openButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(
          `.plugin-host-open-btn[data-node-id="${nodeId}"]`,
        );
        openButtons?.forEach((button) => {
          button.disabled = false;
        });
        item.scrollIntoView({ block: "nearest" });
        markHostedPluginLoadPending(nodeId, resourceIndex);
        sendNodeResourceUpdate(nodeId, "plugin", resourceId, "", resourceIndex, undefined, exposedResourceId, false);
      });
      item.addEventListener("keydown", (event) => {
        const target = event.target as HTMLElement | null;
        if (target && target !== item && target.closest("button")) {
          return;
        }

        if (event.key === "ArrowDown") {
          event.preventDefault();
          focusItemByOffset(item, 1);
          return;
        }
        if (event.key === "ArrowUp") {
          event.preventDefault();
          focusItemByOffset(item, -1);
          return;
        }
        if (event.key === "Home") {
          event.preventDefault();
          focusBoundaryItem(true);
          return;
        }
        if (event.key === "End") {
          event.preventDefault();
          focusBoundaryItem(false);
          return;
        }

        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          item.click();
        }
      });
    });

    list.querySelectorAll<HTMLButtonElement>(".plugin-host-remove-btn").forEach((removeBtn) => {
      removeBtn.addEventListener("click", (event) => {
        event.stopPropagation();
        const resourceId = removeBtn.dataset.resourceId;
        const resourceName = removeBtn.dataset.resourceName || "this plugin";
        if (!resourceId) {
          return;
        }
        void (async () => {
          const confirmed = await showConfirm(
            `Remove "${resourceName}" from your plugin library? The plugin file on disk will not be deleted.`,
            "Remove Plugin",
          );
          if (!confirmed) {
            return;
          }

          const current = getNodeResourceAtIndex(node, resourceIndex);
          if (current.id === resourceId) {
            // Clear the node's selection first so the library entry is no
            // longer in use by the active preset when the cleanup runs.
            hostedPluginLoadFailures.delete(nodeId);
            clearInlineHostedPluginLoadError(list);
            sendNodeResourceUpdate(nodeId, "plugin", "", "", resourceIndex, undefined, exposedResourceId);
          }

          postMessage({
            type: "cleanupResourceLibrary",
            scope: "plugin",
            removeFiles: false,
            resources: [{ type: "plugin", id: resourceId }],
          });
        })();
      });
    });

    list.querySelectorAll<HTMLButtonElement>(".plugin-host-favorite-btn").forEach((favoriteBtn) => {
      favoriteBtn.addEventListener("click", (event) => {
        event.stopPropagation();
        const resourceId = favoriteBtn.dataset.resourceId;
        if (!resourceId) {
          return;
        }
        toggleHostedPluginFavorite(resourceId);
        requestNodeParamsRefresh();
      });
    });
  });
}
