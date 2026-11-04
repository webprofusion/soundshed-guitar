/**
 * Sync points: where bars of the score start in the reference's time.
 *
 * Pure functions, no alphaTab and no DOM, so the rules can be tested on their
 * own. alphaTab does the mapping itself once it has the points (it interpolates
 * between them and carries the last tempo on past the final one); what lives
 * here is keeping the list something it can use:
 *
 * - Bars are addressed by *slot*: a bar plus which pass through it (repeats and
 *   jumps play some bars more than once). `buildPlaybackOrder` lists the slots
 *   in the order they play, from alphaTab's tick lookup.
 * - Times must rise with playback order. Setting a point removes any other
 *   point that would contradict it (an earlier slot at a later time, or a later
 *   slot at an earlier time), so a mis-tap fixes itself on the next try instead
 *   of leaving the cursor running backwards.
 */

import type { PracticeToolTabSyncPoint } from "../types.js";

/** One bar's pass, in the order the score plays. */
export interface BarSlot {
  barIndex: number;
  occurrence: number;
  /** Midi tick the pass starts at (alphaTab's tick lookup). */
  startTick: number;
}

/** Two points closer together than this are treated as the same instant. */
export const MIN_SYNC_GAP_MS = 20;

/** No score has more than this; a stored list longer than it is not ours. */
export const MAX_SYNC_POINTS = 5000;

/**
 * Slots in playback order from alphaTab's `tickCache.masterBars`, which lists
 * every pass through every bar in the order it plays. The occurrence of a pass
 * is how many times its bar has played before it.
 */
export function buildPlaybackOrder(masterBars: readonly { masterBar: { index: number }; start: number }[]): BarSlot[] {
  const seen = new Map<number, number>();
  return masterBars.map((lookup) => {
    const barIndex = lookup.masterBar.index;
    const occurrence = seen.get(barIndex) ?? 0;
    seen.set(barIndex, occurrence + 1);
    return { barIndex, occurrence, startTick: lookup.start };
  });
}

/** Position of a slot in playback order, or -1 when the score has no such pass. */
export function slotOrderIndex(order: readonly BarSlot[], barIndex: number, occurrence: number): number {
  return order.findIndex((slot) => slot.barIndex === barIndex && slot.occurrence === occurrence);
}

/** The slot after `barIndex`/`occurrence` in playback order, or null at the end. */
export function nextSlot(order: readonly BarSlot[], barIndex: number, occurrence: number): BarSlot | null {
  const index = slotOrderIndex(order, barIndex, occurrence);
  return index >= 0 && index + 1 < order.length ? order[index + 1] : null;
}

/**
 * The pass through `barIndex` whose start is closest to `tick` — which pass the
 * user means when they click a bar that repeats. Falls back to the first pass.
 */
export function slotForBarNearTick(order: readonly BarSlot[], barIndex: number, tick: number): BarSlot | null {
  let best: BarSlot | null = null;
  for (const slot of order) {
    if (slot.barIndex !== barIndex) {
      continue;
    }
    if (!best || Math.abs(slot.startTick - tick) < Math.abs(best.startTick - tick)) {
      best = slot;
    }
  }
  return best;
}

function orderKey(order: readonly BarSlot[], point: PracticeToolTabSyncPoint): number {
  const index = slotOrderIndex(order, point.barIndex, point.barOccurence);
  // A point for a pass the score doesn't have sorts last and is dropped by callers.
  return index < 0 ? Number.MAX_SAFE_INTEGER : index + Math.min(1, Math.max(0, point.barPosition)) * 0.999;
}

/** Points in playback order; any for a slot the score lacks are dropped. */
export function sortSyncPoints(order: readonly BarSlot[], points: readonly PracticeToolTabSyncPoint[]): PracticeToolTabSyncPoint[] {
  return points
    .filter((point) => slotOrderIndex(order, point.barIndex, point.barOccurence) >= 0)
    .map((point) => ({ ...point }))
    .sort((a, b) => orderKey(order, a) - orderKey(order, b));
}

/**
 * Sets the start of one bar pass to `millisecondOffset`, replacing any point
 * already on that slot and removing those that would put time out of order.
 * Returns the new list in playback order; the input is not changed.
 */
export function setBarStart(
  order: readonly BarSlot[],
  points: readonly PracticeToolTabSyncPoint[],
  slot: { barIndex: number; occurrence: number },
  millisecondOffset: number
): PracticeToolTabSyncPoint[] {
  const target = slotOrderIndex(order, slot.barIndex, slot.occurrence);
  if (target < 0 || !Number.isFinite(millisecondOffset)) {
    return sortSyncPoints(order, points);
  }
  const ms = Math.max(0, Math.round(millisecondOffset));
  const kept = sortSyncPoints(order, points).filter((point) => {
    const index = slotOrderIndex(order, point.barIndex, point.barOccurence);
    if (index === target && point.barPosition === 0) {
      return false; // replaced
    }
    const at = orderKey(order, point);
    if (at < target) {
      return point.millisecondOffset <= ms - MIN_SYNC_GAP_MS;
    }
    return point.millisecondOffset >= ms + MIN_SYNC_GAP_MS;
  });
  kept.push({ barIndex: slot.barIndex, barOccurence: slot.occurrence, barPosition: 0, millisecondOffset: ms });
  return sortSyncPoints(order, kept);
}

/** Removes the point on one bar pass, if there is one. */
export function removeBarStart(
  points: readonly PracticeToolTabSyncPoint[],
  slot: { barIndex: number; occurrence: number }
): PracticeToolTabSyncPoint[] {
  return points
    .filter((point) => !(point.barIndex === slot.barIndex && point.barOccurence === slot.occurrence && point.barPosition === 0))
    .map((point) => ({ ...point }));
}

/** Moves every point by `deltaMs` — the whole score earlier or later against the reference. */
export function shiftSyncPoints(points: readonly PracticeToolTabSyncPoint[], deltaMs: number): PracticeToolTabSyncPoint[] {
  const smallest = points.reduce((min, point) => Math.min(min, point.millisecondOffset), Number.POSITIVE_INFINITY);
  // Never past zero: the first point would land before the reference starts.
  const delta = Number.isFinite(smallest) ? Math.max(-smallest, deltaMs) : 0;
  return points.map((point) => ({ ...point, millisecondOffset: Math.round(point.millisecondOffset + delta) }));
}

/** Narrows stored data back to sync points, dropping anything malformed. */
export function sanitizeSyncPoints(value: unknown): PracticeToolTabSyncPoint[] {
  if (!Array.isArray(value)) {
    return [];
  }
  const points: PracticeToolTabSyncPoint[] = [];
  for (const entry of value.slice(0, MAX_SYNC_POINTS)) {
    if (!entry || typeof entry !== "object") {
      continue;
    }
    const record = entry as Record<string, unknown>;
    const barIndex = record.barIndex;
    const barOccurence = record.barOccurence;
    const barPosition = record.barPosition;
    const millisecondOffset = record.millisecondOffset;
    if (
      typeof barIndex !== "number" || !Number.isInteger(barIndex) || barIndex < 0
      || typeof barOccurence !== "number" || !Number.isInteger(barOccurence) || barOccurence < 0
      || typeof barPosition !== "number" || !Number.isFinite(barPosition)
      || typeof millisecondOffset !== "number" || !Number.isFinite(millisecondOffset) || millisecondOffset < 0
    ) {
      continue;
    }
    points.push({ barIndex, barOccurence, barPosition: Math.min(1, Math.max(0, barPosition)), millisecondOffset });
  }
  return points;
}

/** "m:ss.cc" for a sync time. */
export function formatSyncTime(ms: number): string {
  const total = Math.max(0, ms) / 1000;
  const minutes = Math.floor(total / 60);
  const seconds = total - minutes * 60;
  return `${minutes}:${seconds.toFixed(2).padStart(5, "0")}`;
}
