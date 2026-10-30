/**
 * Custom effect layouts drawn over the panel: scaling them to the panel width,
 * and the bypass hotspots on top.
 */

import { fitDefaultControlsAreas } from "../../layoutControlsArea.js";
import { computeLayoutScale, type LayoutScaling } from "../../layoutScaling.js";
import type { GraphNode, Preset } from "../../types.js";
import { toggleSignalPathNodeBypass } from "../bypass.js";
import { nodeParamsPanelElement } from "../state.js";

let overlayBypassClickCleanup: (() => void) | null = null;

let layoutScaleObserverCleanups: (() => void)[] = [];

/** A layout's `scaling` block, as the renderer writes it on the container; undefined when absent or unreadable. */
function readScaling(inner: HTMLElement): LayoutScaling | undefined {
  const raw = inner.dataset.layoutScaling;
  if (!raw) return undefined;
  try {
    const parsed = JSON.parse(raw) as unknown;
    return parsed && typeof parsed === "object" ? (parsed as LayoutScaling) : undefined;
  } catch {
    return undefined;
  }
}

/** The nearest ancestor that scrolls vertically, which bounds the height a "fit" layout may take. */
function scrollingAncestor(element: HTMLElement): HTMLElement | null {
  for (let node = element.parentElement; node; node = node.parentElement) {
    const overflowY = getComputedStyle(node).overflowY;
    if ((overflowY === "auto" || overflowY === "scroll") && node.clientHeight > 0) {
      return node;
    }
  }
  return null;
}

/** Height from the layout's top to the bottom of what can be seen without scrolling. */
function availableHeightBelow(outer: HTMLElement, scroller: HTMLElement | null): number {
  const top = outer.getBoundingClientRect().top;
  const bottom = scroller ? scroller.getBoundingClientRect().top + scroller.clientHeight : window.innerHeight;
  return bottom - top - 8;
}

/**
 * Apply uniform CSS-transform scaling to custom layout containers so that
 * background images and control placements always match the design canvas,
 * regardless of the available panel width.
 *
 * The whole layout (backgrounds + controls + labels) scales as a single unit so
 * nothing diverges. How much is computeLayoutScale's call: a layout with no
 * `scaling` block is drawn at its native size, centred, when the panel is wider,
 * and shrunk to the panel's width when it is narrower, as every layout always was.
 *
 * Uses ResizeObserver so the scale stays correct if the panel is resized.
 */
export function applyCustomLayoutScaling(container: HTMLElement | null): void {
  // Disconnect any previous observers from a prior render.
  layoutScaleObserverCleanups.forEach((fn) => fn());
  layoutScaleObserverCleanups = [];

  if (!container) return;

  const outers = container.querySelectorAll<HTMLElement>(".custom-layout-scale-outer");
  outers.forEach((outer) => {
    const inner = outer.querySelector<HTMLElement>(".custom-layout-container[data-design-w]");
    if (!inner) return;

    const designW = parseInt(inner.dataset.designW ?? "0", 10);
    const designH = parseInt(inner.dataset.designH ?? "0", 10);
    if (!designW || !designH) return;

    // Default controls with an area shrink into it; the outer scale below is applied on top.
    layoutScaleObserverCleanups.push(fitDefaultControlsAreas(inner));

    const scaling = readScaling(inner);
    const scroller = scaling?.mode === "fit" ? scrollingAncestor(outer) : null;

    const applyScale = () => {
      const outerW = outer.offsetWidth;
      if (!outerW) return;

      const { scale, centred, overflows } = computeLayoutScale({
        designWidth: designW,
        designHeight: designH,
        availableWidth: outerW,
        availableHeight: scaling?.mode === "fit" ? availableHeightBelow(outer, scroller) : undefined,
        scaling,
      });

      // The container's stylesheet pins its margin to 0 (!important), so a scaled layout that
      // sits in the middle is moved there by the transform. Without a scaling block that never
      // happens: a layout at its own size is centred by its fit-content section, as it always was.
      const offset = centred && scale !== 1 ? (outerW - designW * scale) / 2 : 0;
      inner.style.transform = scale === 1 ? "" : `${offset ? `translateX(${offset}px) ` : ""}scale(${scale})`;
      inner.style.marginLeft = centred ? "auto" : "0";
      inner.style.marginRight = centred ? "auto" : "0";
      outer.style.height = scale === 1 ? `${designH}px` : `${Math.round(designH * scale)}px`;
      if (scaling) {
        outer.style.overflowX = overflows ? "auto" : "hidden";
      }
    };

    applyScale();

    const ro = new ResizeObserver(applyScale);
    ro.observe(outer);
    if (scroller) {
      ro.observe(scroller);
    }
    layoutScaleObserverCleanups.push(() => ro.disconnect());
  });
}

export function bindLayoutOverlayBypassToggles(node: GraphNode, preset: Preset): void {
  if (!nodeParamsPanelElement) {
    return;
  }

  overlayBypassClickCleanup?.();

  const clickHandler = (event: Event) => {
    const target = event.target as HTMLElement | null;
    const overlay = target?.closest('.custom-layout-overlay[data-toggle-bypass="true"]') as HTMLElement | null;
    if (!overlay) {
      return;
    }

    event.preventDefault();
    event.stopPropagation();

    toggleSignalPathNodeBypass(node, preset);
  };

  nodeParamsPanelElement.addEventListener("click", clickHandler);
  overlayBypassClickCleanup = () => {
    nodeParamsPanelElement?.removeEventListener("click", clickHandler);
  };
}
