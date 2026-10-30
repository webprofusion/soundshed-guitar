/**
 * The default controls' area on a backdrop layout (`defaultControlsArea`): a box on the canvas,
 * its top left at `defaultControlsOffset`, that the controls stay inside so they never run past
 * the background's edges. They wrap at the box's width, and while they are still taller than
 * the box they shrink, keeping their shape and the layout's Scale X / Scale Y, until they fit.
 *
 * The renderer gives the wrapper the box's width and writes the box and the scale on it as data
 * attributes; how far to shrink needs the controls' real height, so fitDefaultControls runs once
 * they are in the document, and again whenever their size changes.
 */

export interface LayoutControlsArea {
  width: number;
  height: number;
}

/** Narrowest the controls are shrunk to; past that they are left to overflow. */
const MIN_FIT = 0.2;

/** `value` as an area of positive, finite size, else undefined. */
export function usableControlsArea(value: unknown): LayoutControlsArea | undefined {
  if (!value || typeof value !== "object") return undefined;
  const { width, height } = value as Record<string, unknown>;
  if (typeof width !== "number" || typeof height !== "number") return undefined;
  if (!Number.isFinite(width) || !Number.isFinite(height) || width < 1 || height < 1) return undefined;
  return { width: Math.round(width), height: Math.round(height) };
}

/** The wrapper's data attributes for `area` (already in the wrapper's parent's pixels). */
export function controlsAreaAttributes(area: LayoutControlsArea, scaleX: number, scaleY: number): string {
  return ` data-area-w="${area.width}" data-area-h="${area.height}" data-scale-x="${scaleX}" data-scale-y="${scaleY}"`;
}

function datasetNumber(value: string | undefined): number {
  const n = parseFloat(value ?? "");
  return Number.isFinite(n) && n > 0 ? n : 0;
}

/**
 * Shrink one default-controls wrapper into its area: the largest fit (at most 1) at which the
 * controls, wrapped at the area's width, are no taller than it. Narrower controls wrap into more
 * rows, so their height is measured at each step of a short search rather than worked out once.
 */
export function fitDefaultControls(wrapper: HTMLElement): void {
  const width = datasetNumber(wrapper.dataset.areaW);
  const height = datasetNumber(wrapper.dataset.areaH);
  const scaleX = datasetNumber(wrapper.dataset.scaleX);
  const scaleY = datasetNumber(wrapper.dataset.scaleY);
  if (!width || !height || !scaleX || !scaleY) return;

  // offsetHeight is the height before the transform, so each step is measured as laid out.
  const drawnHeight = (fit: number) => {
    wrapper.style.width = `${width / (scaleX * fit)}px`;
    return wrapper.offsetHeight * scaleY * fit;
  };

  let fit = 1;
  if (drawnHeight(1) > height) {
    let low = MIN_FIT;
    let high = 1;
    for (let step = 0; step < 8; step++) {
      const mid = (low + high) / 2;
      if (drawnHeight(mid) <= height) low = mid;
      else high = mid;
    }
    fit = low;
  }
  // Rounded down, and the width taken from the rounded scale, so the drawn box is never past the area.
  const round = (n: number) => Math.floor(n * 10000) / 10000;
  const drawnX = round(scaleX * fit);
  wrapper.style.width = `${width / drawnX}px`;
  wrapper.style.transform = `scale(${drawnX}, ${round(scaleY * fit)})`;
}

/**
 * Fit every default-controls wrapper with an area under `root`, now and whenever one changes
 * size (a panel shown after it was drawn hidden, a font arriving). Returns the cleanup.
 */
export function fitDefaultControlsAreas(root: ParentNode): () => void {
  const wrappers = Array.from(root.querySelectorAll<HTMLElement>("[data-area-w]"));
  wrappers.forEach(fitDefaultControls);
  if (!wrappers.length || typeof ResizeObserver === "undefined") return () => {};

  // A refit settles on the size it started from, so it does not set off another.
  const observer = new ResizeObserver((entries) => {
    for (const entry of entries) fitDefaultControls(entry.target as HTMLElement);
  });
  wrappers.forEach((wrapper) => observer.observe(wrapper));
  return () => observer.disconnect();
}
