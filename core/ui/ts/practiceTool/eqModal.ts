/**
 * The Backing Track EQ modal — open/close plus the binding that points the
 * shared `EqPanel` at the Practice Tool's own EQ state.
 *
 * Everything you can see and touch inside the modal (band knobs, curve, the
 * enable toggle, reset) is `ts/eqPanel.ts`, the same component the Global EQ
 * modal uses. All this module contributes is where the values live and how a
 * change gets to the engine.
 *
 * It is a modal rather than part of the panel because the Practice Tool panel
 * already carries a waveform, a loop list and four faders.
 */

import { EqPanel } from "../eqPanel.js";
import { uiState } from "../state.js";
import type { PracticeToolEqState } from "../types.js";
import { schedulePracticeToolEqParams, sendPracticeToolEqEnabled, sendPracticeToolEqParams } from "./eqSend.js";

// Built on first open, not at load: the canvas has no size while the modal is
// display:none, and EqPanel refuses to build a curve against a zero-sized one.
let panel: EqPanel | null = null;

// Registered by practiceTool.ts — the panel's EQ button reflects whether the
// EQ is on and shaping, so a change made in here has to ask for that redraw
// rather than importing the facade back (see practiceTool/projects.ts for the
// same seam and why it exists).
let notifyChanged: (() => void) | null = null;

export function setPracticeToolEqChangeListener(fn: () => void): void {
  notifyChanged = fn;
}

function getEqState(): PracticeToolEqState | null {
  return uiState.practiceTool?.eq ?? null;
}

function isOpen(): boolean {
  const modal = document.getElementById("practice-tool-eq-modal");
  return Boolean(modal && modal.style.display !== "none");
}

function ensurePanel(): EqPanel | null {
  if (panel) {
    return panel;
  }
  const bandsHost = document.getElementById("practice-tool-eq-bands");
  if (!bandsHost) {
    return null;
  }
  panel = new EqPanel(
    {
      bandsHost,
      canvas: document.getElementById("practice-tool-eq-canvas") as HTMLCanvasElement | null,
      toggle: document.getElementById("practice-tool-eq-enabled") as HTMLInputElement | null,
      resetButton: document.getElementById("practice-tool-eq-reset"),
      idPrefix: "practice_tool_eq",
      onChanged: () => notifyChanged?.(),
    },
    {
      label: "practice tool EQ",
      readParams: () => getEqState()?.params ?? {},
      writeParams: (changed, commit) => {
        const eq = getEqState();
        if (!eq) {
          return;
        }
        Object.assign(eq.params, changed);
        // Dragging fires continuously, so in-progress ticks are coalesced and
        // the end of the gesture goes out immediately — the same shape as the
        // Practice Tool's own speed and pitch faders.
        if (commit) {
          sendPracticeToolEqParams(changed);
        } else {
          schedulePracticeToolEqParams(changed);
        }
      },
      readEnabled: () => getEqState()?.enabled ?? false,
      writeEnabled: (enabled) => {
        const eq = getEqState();
        if (!eq) {
          return;
        }
        eq.enabled = enabled;
        sendPracticeToolEqEnabled(enabled);
      },
    }
  );
  return panel;
}

/** Resyncs the modal from state. A no-op while it is closed, which is what
 * lets the Practice Tool panel call it on any change without checking. */
export function renderPracticeToolEqModal(): void {
  if (!isOpen()) {
    return;
  }
  ensurePanel()?.render();
}

export function openPracticeToolEqModal(): void {
  const modal = document.getElementById("practice-tool-eq-modal");
  if (!modal) {
    return;
  }
  modal.style.display = "flex";
  // The canvas only gets a real size once the modal is laid out, so the curve
  // lands on the frame after it becomes visible, not in this one.
  ensurePanel()?.render();
  requestAnimationFrame(() => ensurePanel()?.render());
}

export function closePracticeToolEqModal(): void {
  const modal = document.getElementById("practice-tool-eq-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

export function bindPracticeToolEqModal(): void {
  const closeBtn = document.getElementById("practice-tool-eq-close");
  if (closeBtn && closeBtn.dataset.bound !== "true") {
    closeBtn.dataset.bound = "true";
    closeBtn.addEventListener("click", () => closePracticeToolEqModal());
  }

  const modal = document.getElementById("practice-tool-eq-modal");
  if (modal && modal.dataset.bound !== "true") {
    modal.dataset.bound = "true";
    modal.addEventListener("click", (event) => {
      if (event.target === modal) {
        closePracticeToolEqModal(); // click the backdrop to dismiss, like the other modals
      }
    });
  }
}
