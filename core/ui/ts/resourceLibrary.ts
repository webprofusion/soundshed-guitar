import { uiState } from "./state.js";
import { findResourceById } from "./utils.js";
import type { LibraryResource } from "./types.js";

/**
 * Lookups into the in-memory resource library (`uiState.resourceLibrary`).
 *
 * These were copy-pasted into presets.ts, settings.ts and signalPath.ts, which
 * had drifted: only the signalPath copy guarded against a missing resourceType.
 * That guarded form is the one kept here.
 */

export function getLibraryResource(
  resourceType: string | undefined,
  resourceId: string | null | undefined
): LibraryResource | undefined {
  if (!resourceType || !resourceId) return undefined;
  const resources = uiState.resourceLibrary[resourceType] ?? [];
  return findResourceById(resources, resourceId);
}

export function getLibraryResourceByHash(
  resourceType: string | undefined,
  hash?: string
): LibraryResource | undefined {
  if (!resourceType || !hash) return undefined;
  const resources = uiState.resourceLibrary[resourceType] ?? [];
  const wanted = hash.toLowerCase();
  return resources.find((resource) => resource.hash?.toLowerCase() === wanted);
}
