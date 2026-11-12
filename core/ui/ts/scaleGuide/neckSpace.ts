/**
 * How far down its panel the Scale Guide's neck starts, kept on the guide as `--sg-above-neck` for
 * the compact layout's neck height (css/compact/panels.css): the controls above the neck wrap onto
 * more or fewer lines with the width and the mode, so a fixed allowance either cut the neck short
 * under the footer or left it smaller than it had room to be.
 */

/** Measures now and whenever the guide changes size. The neck's own height does not move its top, so this settles at once. */
export function watchNeckSpace(root: HTMLElement, neck: HTMLElement): void {
  if (typeof ResizeObserver === "undefined") {
    return;
  }

  new ResizeObserver(() => measureAboveNeck(root, neck)).observe(root);
}

export function measureAboveNeck(root: HTMLElement, neck: HTMLElement): void {
  const panel = root.parentElement;

  if (!panel || neck.clientWidth === 0) {
    return;
  }

  const above = Math.round(neck.getBoundingClientRect().top - panel.getBoundingClientRect().top + panel.scrollTop);
  const value = `${Math.max(0, above)}px`;

  if (root.style.getPropertyValue("--sg-above-neck") !== value) {
    root.style.setProperty("--sg-above-neck", value);
  }
}
