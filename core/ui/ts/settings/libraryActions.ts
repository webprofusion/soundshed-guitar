/**
 * What a row in the library can do: edit its metadata, replace the file behind
 * it, copy its path, or take in a file dropped onto the list.
 *
 * A dropped file's bytes are read in the WebView and sent over the bridge —
 * WebView2 is standard Chromium, so `File.path` is always undefined here.
 */

import { arrayBufferToBase64, sanitizeFilename } from "../archiveUtils.js";
import { postMessage } from "../bridge.js";
import { showPrompt } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { copyTextToClipboard, sha256HexFromBase64 } from "../utils.js";
import { libraryResults } from "./dom.js";
import { getLibraryItems, inferResourceOrigin, splitLibraryTags } from "./libraryData.js";
import type { LibraryItem } from "./libraryData.js";

export function bindLibraryActions(): void {
  // Bound to a local so the null check narrows inside the closures below; a
  // narrowing on an imported binding does not survive into a closure.
  const results = libraryResults;
  if (!results) {
    return;
  }

  if (results.dataset.bound === "true") {
    return;
  }

  results.dataset.bound = "true";
  results.addEventListener("click", (event) => {
    const target = event.target as HTMLElement | null;
    if (!target) {
      return;
    }

    const copyPathButton = target.closest(".equipment-library-copy-path") as HTMLButtonElement | null;
    if (copyPathButton) {
      const resourceType = copyPathButton.dataset.resourceType ?? "";
      const resourceId = copyPathButton.dataset.resourceId ?? "";
      const item = getLibraryItems().find((entry) => entry.type === resourceType && entry.id === resourceId);
      if (!item) {
        showNotification("Copy path failed", "Resource not found in library.");
        return;
      }
      void copyLibraryResourcePath(item);
      return;
    }

    const editButton = target.closest(".equipment-library-edit") as HTMLButtonElement | null;
    if (editButton) {
      const resourceType = editButton.dataset.resourceType ?? "";
      const resourceId = editButton.dataset.resourceId ?? "";
      const item = getLibraryItems().find((entry) => entry.type === resourceType && entry.id === resourceId);
      if (!item) {
        showNotification("Edit failed", "Resource not found in library.");
        return;
      }
      void promptEditLibraryResource(item);
      return;
    }

    const browseButton = target.closest(".equipment-library-browse") as HTMLButtonElement | null;
    if (!browseButton) {
      return;
    }

    const resourceType = browseButton.dataset.resourceType ?? "";
    const resourceId = browseButton.dataset.resourceId ?? "";
    if (!resourceType || !resourceId) {
      return;
    }

    const item = getLibraryItems().find((entry) => entry.type === resourceType && entry.id === resourceId);
    if (!item) {
      showNotification("Replace failed", "Resource not found in library.");
      return;
    }

    if (inferResourceOrigin(item.filePath, item.metadata) === "Local") {
      postMessage({
        type: "browseLibraryResourcePath",
        resourceType: item.type,
        resourceId: item.id,
      });
      return;
    }

    promptReplaceLibraryResource(item);
  });

  const updateDropActive = (active: boolean) => {
    results.classList.toggle("riff-drop-active", active);
  };

  const extractSupportedLocalFiles = (event: DragEvent): File[] => {
    return Array.from(event.dataTransfer?.files ?? []).filter((file) => inferDroppedLocalResourceType(file.name) !== null);
  };

  results.addEventListener("dragover", (event) => {
    const files = extractSupportedLocalFiles(event);
    if (!files.length) {
      return;
    }
    event.preventDefault();
    updateDropActive(true);
    if (event.dataTransfer) {
      event.dataTransfer.dropEffect = "copy";
    }
  });

  results.addEventListener("dragleave", (event) => {
    if (event.target === results) {
      updateDropActive(false);
    }
  });

  results.addEventListener("drop", (event) => {
    const files = extractSupportedLocalFiles(event);
    updateDropActive(false);
    if (!files.length) {
      return;
    }
    event.preventDefault();
    void importDroppedLibraryFiles(files);
  });
}

export function inferDroppedLocalResourceType(fileName: string): "nam" | "ir" | null {
  const lower = fileName.trim().toLowerCase();
  if (lower.endsWith(".wav") || lower.endsWith(".ir")) {
    return "ir";
  }
  if (lower.endsWith(".nam") || lower.endsWith(".json")) {
    return "nam";
  }
  return null;
}

export async function importDroppedLibraryFiles(files: File[]): Promise<void> {
  for (const file of files) {
    await importDroppedLibraryFile(file);
  }
}

export async function importDroppedLibraryFile(file: File): Promise<void> {
  const resourceType = inferDroppedLocalResourceType(file.name);
  if (!resourceType) {
    return;
  }

  const buffer = await file.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  const hash = await sha256HexFromBase64(data);
  const nativePath = typeof (file as File & { path?: unknown }).path === "string"
    ? String((file as File & { path?: unknown }).path)
    : "";
  const label = file.name.replace(/\.[^.]+$/, "").trim() || file.name;

  postMessage({
    type: "saveLocalLibraryResource",
    resourceType,
    ...(nativePath ? { filePath: nativePath } : { data }),
    fileName: sanitizeFilename(file.name),
    name: label,
    category: "Local",
    tags: [],
    hash,
    metadata: {
      provider: "local",
    },
  });
}

export async function promptEditLibraryResource(item: LibraryItem): Promise<void> {
  if (inferResourceOrigin(item.filePath, item.metadata) === "Built-in") {
    showNotification("Edit unavailable", "Built-in resources cannot be edited.");
    return;
  }

  const name = await showPrompt("Title", item.name, { title: "Edit Resource" });
  if (name === null) {
    return;
  }
  const category = await showPrompt("Category", item.category, { title: "Edit Resource" });
  if (category === null) {
    return;
  }
  const description = await showPrompt("Description", item.description ?? "", { title: "Edit Resource", multiline: true });
  if (description === null) {
    return;
  }
  const tagsText = await showPrompt("Tags (comma-separated)", (item.tags ?? []).join(", "), { title: "Edit Resource" });
  if (tagsText === null) {
    return;
  }
  const metadataText = await showPrompt("Metadata JSON", JSON.stringify(item.metadata ?? {}, null, 2), { title: "Edit Resource", multiline: true });
  if (metadataText === null) {
    return;
  }

  const tags = tagsText.trim()
    ? Array.from(new Set(splitLibraryTags(tagsText)))
    : [];
  let metadata: Record<string, string> = {};
  const trimmedMetadata = metadataText.trim();
  if (trimmedMetadata) {
    try {
      const parsed = JSON.parse(trimmedMetadata) as Record<string, unknown>;
      if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
        throw new Error("Metadata must be a JSON object.");
      }
      metadata = Object.fromEntries(
        Object.entries(parsed)
          .filter(([, value]) => value !== null && value !== undefined)
          .map(([key, value]) => [key, String(value)]),
      );
    } catch (error) {
      showNotification("Invalid metadata", error instanceof Error ? error.message : "Metadata must be valid JSON.");
      return;
    }
  }

  postMessage({
    type: "updateLibraryResource",
    resourceType: item.type,
    resourceId: item.id,
    name: name.trim() || item.id,
    category: category.trim(),
    description,
    tags,
    metadata,
  });
}

export function promptReplaceLibraryResource(item: LibraryItem): void {
  const input = document.createElement("input");
  input.type = "file";
  input.accept = item.type === "nam" ? ".nam,.json" : item.type === "ir" ? ".wav" : "*";
  input.addEventListener("change", () => {
    void replaceLibraryResourceFromInput(item, input);
  });
  input.click();
}

export async function replaceLibraryResourceFromInput(item: LibraryItem, input: HTMLInputElement): Promise<void> {
  const file = input.files?.[0];
  input.value = "";
  if (!file) {
    return;
  }

  const buffer = await file.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  const hash = await sha256HexFromBase64(data);
  const provider = item.metadata?.provider ?? "manual";

  postMessage({
    type: "importRemoteResource",
    provider,
    resourceType: item.type,
    resourceId: item.id,
    name: item.name,
    description: item.description,
    category: item.category,
    tags: item.tags ?? [],
    fileName: sanitizeFilename(file.name),
    data,
    hash,
    metadata: item.metadata ?? {},
  });

  showNotification("Resource updated", item.name || item.id);
}

export async function copyLibraryResourcePath(item: LibraryItem): Promise<void> {
  const path = item.filePath.trim();
  if (!path || inferResourceOrigin(item.filePath, item.metadata) !== "Local") {
    showNotification("Copy path unavailable", "This resource does not have a local file path.");
    return;
  }

  try {
    await copyTextToClipboard(path);
    showNotification("Path copied", path);
  } catch {
    const promptResult = await showPrompt("Copy this path by hand:", path, { title: "Copy Path" });
    if (promptResult === null) {
      showNotification("Copy cancelled", "Local path was not copied.");
      return;
    }
    showNotification("Path ready", "Local resource path is shown for manual copy.");
  }
}
