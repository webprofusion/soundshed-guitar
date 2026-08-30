/**
 * Backing-track EQ → engine. Split from ./eq.js purely so that module can stay
 * import-free enough for state.ts to seed the default from it.
 */

import { setPracticeToolEq } from "../bridge.js";
import type { PracticeToolEqState } from "../types.js";

/** Sends the whole curve plus the toggle — used on reset, on project recall,
 * and any time the UI needs the engine to match its state wholesale. */
export function pushPracticeToolEqToEngine(eq: PracticeToolEqState): void {
  setPracticeToolEq({ enabled: eq.enabled, params: { ...eq.params } });
}

// Dragging a band handle fires continuously. Each send is small and the engine
// applies it under the DSP lock, so it is coalesced the same way the speed and
// pitch faders are, then flushed on release so the final value never sits in a
// pending timer.
const EQ_SEND_DEBOUNCE_MS = 40;

let pendingParams: Record<string, number> | null = null;
let pendingTimer: ReturnType<typeof setTimeout> | null = null;

function flushPendingEqParams(): void {
  if (pendingTimer !== null) {
    clearTimeout(pendingTimer);
    pendingTimer = null;
  }
  if (pendingParams) {
    setPracticeToolEq({ params: pendingParams });
    pendingParams = null;
  }
}

/** Coalesced per-band send for an in-progress drag. */
export function schedulePracticeToolEqParams(params: Record<string, number>): void {
  pendingParams = { ...(pendingParams ?? {}), ...params };
  if (pendingTimer === null) {
    pendingTimer = setTimeout(() => {
      pendingTimer = null;
      flushPendingEqParams();
    }, EQ_SEND_DEBOUNCE_MS);
  }
}

/** End-of-gesture send: cancels any pending timer and goes out immediately. */
export function sendPracticeToolEqParams(params: Record<string, number>): void {
  pendingParams = { ...(pendingParams ?? {}), ...params };
  flushPendingEqParams();
}

export function sendPracticeToolEqEnabled(enabled: boolean): void {
  flushPendingEqParams(); // never let a queued curve land after the toggle it belongs with
  setPracticeToolEq({ enabled });
}
