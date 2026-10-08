/**
 * Small pure helpers the browser leans on: reading loosely-typed library fields,
 * and the two formatting details the rows need.
 */

import type { LibraryResource } from "../types.js";

export function normalizeFilterValue(value: string): string {
  return value.trim().replace(/\s+/g, " ").toLowerCase();
}

/**
 * Resolves a Tone3000 category hint to the best-matching library category name.
 * Matching is case-insensitive and keyword-based so it works regardless of how
 * users have named their library categories.
 *
 * Examples: "pedal" matches "Pedal", "FX Pedals", "Pedals"
 *           "full-rig"  matches "Full Rig", "Full-Rig", "Full Rigs"
 *           "amp"       matches "Amp", "Amps", "Amplifiers" (not "Full Rig" entries)
 *           "cab"       matches "cab" — what the engine files every cabinet IR under
 *
 * "ir" is the old name for the cab hint. It is the resource type, not a category: reverb
 * IRs are type "ir" too, so it must never pick out "Reverb IRs".
 */
export function resolveLibraryCategoryFromHint(hint: string, availableCategories: string[]): string | null {
  let normalizedHint = hint.trim().toLowerCase();
  if (normalizedHint === "ir") {
    normalizedHint = "cab";
  }
  if (!normalizedHint) {
    return null;
  }

  // 1. Exact case-insensitive match
  const exact = availableCategories.find((c) => c.toLowerCase() === normalizedHint);
  if (exact) return exact;

  // 2. Keyword-based fuzzy match
  for (const cat of availableCategories) {
    const lower = cat.toLowerCase();
    if (normalizedHint === "pedal" && lower.includes("pedal")) return cat;
    if (normalizedHint === "full-rig" && (lower.includes("full-rig") || lower.includes("full rig") || lower.includes("fullrig"))) return cat;
    if (normalizedHint === "amp" && lower.includes("amp") && !lower.includes("full")) return cat;
    if (normalizedHint === "cab" && lower.includes("cab")) return cat;
    if (normalizedHint === "reverb" && lower.includes("reverb")) return cat;
  }

  return null;
}

export function splitTagValues(raw: string): string[] {
  return raw
    .split(/[,;/|]/g)
    .map((tag) => tag.trim())
    .filter((tag) => Boolean(tag));
}

export function getResourceTags(resource: Pick<LibraryResource, "tags" | "metadata">): string[] {
  const collected = new Set<string>();

  if (Array.isArray(resource.tags)) {
    resource.tags.forEach((tag) => {
      const value = tag.trim();
      if (value) {
        collected.add(value);
      }
    });
  }

  const metadataTags = resource.metadata?.tags;
  if (typeof metadataTags === "string") {
    splitTagValues(metadataTags).forEach((tag) => collected.add(tag));
  }

  return Array.from(collected);
}

export function getResourceCreator(resource: Pick<LibraryResource, "metadata">): string {
  const metadata = resource.metadata ?? {};
  return (
    metadata.creatorName
    ?? metadata.authorUsername
    ?? metadata.modeledBy
    ?? metadata.creator
    ?? metadata.provider
    ?? ""
  ).trim();
}

export function getResourceLibraryFacets(resources: LibraryResource[]): { tags: string[]; creators: string[] } {
  const tags = new Set<string>();
  const creators = new Set<string>();

  resources.forEach((resource) => {
    getResourceTags(resource).forEach((tag) => tags.add(tag));
    const creator = getResourceCreator(resource);
    if (creator) {
      creators.add(creator);
    }
  });

  return {
    tags: Array.from(tags).sort((a, b) => a.localeCompare(b)),
    creators: Array.from(creators).sort((a, b) => a.localeCompare(b)),
  };
}

/// Controls inside a result row (favourite, edit, delete, attribution links)
/// own their own clicks, and the expanded details panel is text the user may
/// well be double clicking to select, so neither means "take this resource".
export function isDoubleClickExempt(target: HTMLElement): boolean {
  return Boolean(
    target.closest("button, a, input, select, textarea")
    || target.closest(".resource-browser-item-details-panel"),
  );
}

export function sanitizeFilename(raw: string): string {
  const trimmed = raw.trim() || "resource";
  return trimmed.replace(/[^a-z0-9-_.]+/gi, "-");
}

export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  let value = bytes;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

/** The NAM architecture badge shown on a row, from whatever the metadata spells it as. */
export function normalizeArchitectureBadge(raw: string): string {
  const normalized = raw.trim().toLowerCase();
  if (!normalized) {
    return "";
  }
  if (normalized === "2" || normalized === "a2") {
    return "A2";
  }
  if (normalized === "1" || normalized === "a1") {
    return "A1";
  }
  if (normalized === "custom") {
    return "Custom";
  }
  return "";
}

/** Same, but also recognising the descriptive names NAM files sometimes carry. */
export function normalizeNamArchitectureBadge(raw: string): string {
  const direct = normalizeArchitectureBadge(raw);
  if (direct) {
    return direct;
  }
  const normalized = raw.trim().toLowerCase();
  if (!normalized) {
    return "";
  }
  if (normalized.includes("slimmable")) {
    return "A2";
  }
  if (normalized.includes("wavenet")) {
    return "A1";
  }
  return "";
}
