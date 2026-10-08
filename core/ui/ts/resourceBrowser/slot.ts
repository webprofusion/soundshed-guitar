/**
 * What the slot a browser was opened for decides: the library category it opens filtered
 * to, the Tone3000 gear it searches, the category an import is filed under, and which
 * remembered filters and results it reopens with.
 *
 * IRs are filed under "cab" or "reverb" (the engine's IrResourceCategory.h). The IR Cabinet
 * and the IR Reverb both load type "ir" files, so the category is the only thing that tells
 * their pickers apart; "ir" itself is the type, never a category.
 */

import { DEFAULT_RESOURCE_CONTEXT_KEY } from "./settings.js";
import type { ResourceBrowserOptions } from "./types.js";

type SlotOptions = Pick<ResourceBrowserOptions, "resourceType" | "libraryCategoryHint" | "tone3000CategoryFilter" | "contextKey">;

export type IrLibraryCategory = "cab" | "reverb";

/// Tone3000's IR gear: cab IRs, rooms ("space"), or every IR, reverb pedals included.
export const TONE3000_IR_GEAR_OPTIONS: ReadonlyArray<{ value: string; label: string }> = [
  { value: "cab", label: "Cab IRs" },
  { value: "space", label: "Room & Reverb IRs" },
  { value: "ir", label: "All IRs" },
];

export function irSlotCategory(options: SlotOptions): IrLibraryCategory {
  return options.libraryCategoryHint === "reverb" || options.contextKey === "ir-reverb" ? "reverb" : "cab";
}

/// The library category the browser opens on.
export function slotLibraryCategoryHint(options: SlotOptions): string | undefined {
  return options.resourceType === "ir" ? irSlotCategory(options) : options.tone3000CategoryFilter;
}

/// The Tone3000 gear an IR browser opens searching.
export function slotTone3000IrGear(options: SlotOptions): string {
  return irSlotCategory(options) === "reverb" ? "space" : "cab";
}

/// What an import made from this browser is filed under. An IR goes to the slot it was
/// picked for. A NAM capture's own metadata wins over this in the engine, so the slot's
/// gear only matters for a capture that does not say what it is.
export function slotImportCategory(options: SlotOptions | null, fallback: string): string {
  if (options?.resourceType === "ir") {
    return irSlotCategory(options);
  }

  return options?.tone3000CategoryFilter ?? (fallback.trim() || "Local");
}

/// Remembered filters and Tone3000 results are per slot, so the reverb picker never
/// reopens on the cab picker's search, nor the pedal picker on the amp's.
export function slotStateKey(options: SlotOptions): string {
  const contextKey = options.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
  return `${options.resourceType}:${contextKey}:${slotLibraryCategoryHint(options) ?? ""}`;
}

export function slotTitle(options: SlotOptions): string {
  if (options.resourceType !== "ir") {
    return "Select Amp Model";
  }

  return irSlotCategory(options) === "reverb" ? "Select Reverb IR" : "Select IR Cabinet";
}
