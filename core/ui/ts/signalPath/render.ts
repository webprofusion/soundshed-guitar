/**
 * Indirection for "re-draw the signal path bar".
 *
 * The renderer itself lives in signalPath.ts, because it needs the whole
 * rendering pipeline. Several smaller modules — the mixer, the scene panel,
 * the add menu — need to ask for a redraw after they change something, and
 * importing signalPath.ts to do that would put them in an import cycle with it.
 *
 * So the relationship is stated the way it actually is: those modules *request*
 * a render, they do not own one. signalPath.ts registers the implementation
 * once, at module load.
 */

let render: (() => void) | null = null;

/** Called once by signalPath.ts to supply the real renderer. */
export function setSignalPathRenderer(fn: () => void): void {
  render = fn;
}

/** Redraws the signal path bar, if the renderer has been registered yet. */
export function requestSignalPathRender(): void {
  render?.();
}

let refreshParams: (() => void) | null = null;

/** Called once by signalPath.ts to supply the real params-panel refresh. */
export function setNodeParamsRefresher(fn: () => void): void {
  refreshParams = fn;
}

/**
 * Re-renders the parameters panel for the currently selected node. Used by the
 * modules that change a node's parameters — the effect-presets flyout, for one
 * — and would otherwise have to import signalPath.ts to redraw.
 */
export function requestNodeParamsRefresh(): void {
  refreshParams?.();
}
