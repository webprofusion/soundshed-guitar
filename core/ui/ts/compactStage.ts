/**
 * Which of the Play panel's two halves is on screen, at compact density.
 *
 * The full layout shows the signal chain and the selected node's controls at the
 * same time, stacked. On a 400px-tall window that split gives the chain 75px and
 * the controls 211px, and neither is comfortable: the chain loses its node labels
 * and the controls lose their knobs below the fold. They are also rarely wanted
 * at the same moment — you pick a node, then you work on it.
 *
 * So at compact density they take turns. The chain tab gets the whole stage,
 * which is enough for full-size nodes with labels and for parallel branches to
 * stack; the detail tab gets the same space for the params panel. Selecting a
 * node hands over automatically, so the common path is still one tap.
 *
 * Inert at full density, in a compact window tall enough to show both halves
 * (`isCompactSplit`), and on every panel but Play — `navigation.ts` stamps
 * `data-main-panel` on the root, and the CSS only shows the tabs for `visualizer`.
 * On the landscape rail the tabs are hidden and the rail's Chain and Effect
 * buttons stand in for them; `navigation.ts` binds those, because they switch
 * the main panel as well.
 *
 * This is a leaf: it imports nothing but `compactMode`. Persisting the choice
 * happens the other way round — it announces `compactStageChanged` and
 * `navigation.ts`, which owns the view state, writes it down.
 */

import { isCompactStaged } from "./compactMode.js";

export type CompactStage = "chain" | "detail";

const STAGE_VALUES: readonly CompactStage[] = ["chain", "detail"];

/**
 * Chain first. The detail tab opens on "No Effect Selected" until something is
 * picked, and starting on a panel that tells you to go somewhere else is a poor
 * first screen; the chain is also what orients you in a rig you did not build
 * this minute.
 */
const DEFAULT_STAGE: CompactStage = "chain";

let stage: CompactStage = DEFAULT_STAGE;
let initialized = false;

export function isCompactStage(value: unknown): value is CompactStage {
  return typeof value === "string" && STAGE_VALUES.includes(value as CompactStage);
}

function tabs(): HTMLButtonElement[] {
  return Array.from(document.querySelectorAll<HTMLButtonElement>("[data-compact-stage]"));
}

function applyStage(): void {
  document.documentElement.dataset.compactStage = stage;
  tabs().forEach((tab) => {
    const isActive = tab.dataset.compactStage === stage;
    tab.classList.toggle("is-active", isActive);
    tab.setAttribute("aria-selected", isActive ? "true" : "false");
    tab.tabIndex = isActive ? 0 : -1;
  });
}

export function getCompactStage(): CompactStage {
  return stage;
}

/** Apply a stage without announcing it — for a value restored from view state. */
export function applyCompactStage(next: CompactStage): void {
  stage = next;
  applyStage();
}

export function setCompactStage(next: CompactStage): void {
  if (next === stage) {
    return;
  }
  applyCompactStage(next);
  window.dispatchEvent(new CustomEvent("compactStageChanged", { detail: { stage: next } }));
}

/**
 * Bring the node controls into view, if they are behind the other tab.
 *
 * Called when the user picks a node in the chain. Deliberately not called when
 * the panel re-renders for its own reasons — a state message that happens to
 * restore a selection must not yank the user off the chain mid-edit.
 */
export function revealCompactNodeDetail(): void {
  if (!isCompactStaged()) {
    return;
  }
  setCompactStage("detail");
}

/**
 * The detail stage is the params panel most of the time and the performance pads
 * when the Play view is switched to them, so whatever names it says which: the
 * stage tab, and the Effect button on the landscape rail.
 */
export function setCompactStageDetailLabel(label: string): void {
  document.querySelectorAll<HTMLElement>("[data-compact-stage-detail-label]").forEach((element) => {
    if (element.textContent !== label) {
      element.textContent = label;
    }
  });
}

export function initCompactStage(): void {
  if (initialized) {
    applyStage();
    return;
  }
  initialized = true;

  tabs().forEach((tab, index, all) => {
    tab.addEventListener("click", () => {
      const next = tab.dataset.compactStage;
      if (isCompactStage(next)) {
        setCompactStage(next);
      }
    });

    tab.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") {
        return;
      }
      event.preventDefault();
      const direction = event.key === "ArrowRight" ? 1 : -1;
      const nextTab = all[(index + direction + all.length) % all.length];
      nextTab.focus();
      const next = nextTab.dataset.compactStage;
      if (isCompactStage(next)) {
        setCompactStage(next);
      }
    });
  });

  applyStage();
}
