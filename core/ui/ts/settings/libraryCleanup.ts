/**
 * Finding library resources no preset references any more, and removing them.
 */

import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { showNotification } from "../notifications.js";
import { libraryCleanupButton, libraryCleanupRow, libraryCleanupSelect } from "./dom.js";
import { getLibraryItems, inferResourceOrigin } from "./libraryData.js";
import { buildUsedResourceSet } from "./resourceUsage.js";

export function initLibraryCleanup(): void {
  if (!libraryCleanupButton) {
    return;
  }

  if (libraryCleanupButton.dataset.bound === "true") {
    return;
  }

  libraryCleanupButton.dataset.bound = "true";
  libraryCleanupButton.addEventListener("click", () => void cleanupUnusedResources());
}

export function updateResourceCleanupVisibility(enabled: boolean): void {
  if (libraryCleanupRow) {
    libraryCleanupRow.toggleAttribute("hidden", !enabled);
  }
  if (libraryCleanupSelect) {
    libraryCleanupSelect.disabled = !enabled;
  }
  if (libraryCleanupButton) {
    libraryCleanupButton.disabled = !enabled;
  }
}

export async function cleanupUnusedResources(): Promise<void> {
  if (!isFeatureEnabled(Features.ResourceCleanup)) {
    showNotification("Cleanup unavailable", "Enable Resource Cleanup Tools in Settings > User Interface > Features to use Resource Library cleanup.");
    return;
  }

  const scope = libraryCleanupSelect?.value ?? "all";
  const allItems = getLibraryItems();
  const usedResources = buildUsedResourceSet();

  const unused = allItems.filter((item) => !usedResources.has(`${item.type}:${item.id}`));
  const scoped = scope === "all"
    ? unused
    : unused.filter((item) => item.type === scope);
  const deletable = scoped.filter((item) => inferResourceOrigin(item.filePath, item.metadata) === "Imported");
  const skippedBuiltIn = scoped.length - deletable.length;

  if (!deletable.length) {
    showNotification("Cleanup", skippedBuiltIn ? "No unused imported resources found." : "No unused resources found.");
    return;
  }

  const scopeLabel = scope === "all" ? "all unused resources" : `unused ${scope.toUpperCase()} resources`;
  const message = `Remove ${deletable.length} ${scopeLabel}?`
    + (skippedBuiltIn ? ` ${skippedBuiltIn} built-in resources will be kept.` : "");

  const confirmed = await showConfirm(message, "Cleanup");
  if (!confirmed) {
    return;
  }

  postMessage({
    type: "cleanupResourceLibrary",
    scope,
    removeFiles: true,
    resources: deletable.map((item) => ({ type: item.type, id: item.id })),
  });
}
