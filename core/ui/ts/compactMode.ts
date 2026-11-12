/**
 * Display density — the one switch the compact layout hangs off.
 *
 * Every compact rule in `css/compact/` keys off `data-density="compact"` on the
 * root element, and the handful of layout behaviours that cannot be expressed in
 * CSS (the control-bar overlay, the footer overflow popover, the forced-compact
 * signal chain) read `getDensity()` instead of running their own `matchMedia`.
 * That is deliberate: before this module the shell had four independent width
 * breakpoints in three files, none of which could see how *short* the window was,
 * which is the constraint that actually breaks this UI. At 640x400 — the app's
 * own minimum editor size — the chrome added up to the full viewport height and
 * the main content area was laid out at zero pixels.
 *
 * Two things this does that a media query cannot:
 *
 *  - **Height counts.** A 1280x400 window is as unusable as a 640x800 one, and
 *    only `data-density` sees both.
 *  - **Zoom counts.** The UI zoom setting is `zoom` on `document.body`, so a
 *    viewport is only as large as `innerWidth / zoom` once the content is laid
 *    out. At 150% zoom a 1280x800 window has a 853x533 layout box and wants the
 *    compact shell; a media query would still read 1280x800 and give it the
 *    desktop one.
 *
 * A preference of `auto` follows the viewport and the pointer; `compact` and
 * `full` pin it. The choice persists as an app setting so it survives a restart.
 *
 * The pointer counts because a tablet is the size of a small desktop window but
 * is driven by a finger. With a mouse, Auto gives the compact shell at 980 wide or
 * 620 tall or less. When the primary pointer is coarse it does so up to 1200x850,
 * which takes in every iPad but the 12.9" Pro: there the full shell spent 212px
 * on chrome and had mouse-sized controls, and the compact shell with the chain
 * over the effect (`data-compact-split`) reads like it with a third of the chrome.
 */

import { updateAppSetting } from "./appSettingsStore.js";

export type Density = "compact" | "full";

export type DensityPreference = "auto" | Density;

/**
 * How the compact shell is arranged, which follows the shape of the viewport.
 *
 * A landscape compact viewport is starved of height and has width to spare. That
 * is a phone — and on Android landscape is the only way the app runs — or a short
 * desktop window. So the top-level navigation leaves its row for a rail down the
 * left edge (`rail`), and the row goes back to the effect controls. A portrait
 * viewport keeps the stacked bars (`stack`): there the rail would take a real
 * share of a narrow screen's width, and height is what it has to give.
 *
 * Stamped on the root as `data-compact-layout`, and only while density is compact.
 */
export type CompactLayout = "rail" | "stack";

export const DENSITY_SETTING = "ui.density";

/**
 * Widths at or below this get the compact shell, with a mouse (see
 * {@link COMPACT_TOUCH_MAX_WIDTH} for a finger). 980 is not a new number — it is
 * the breakpoint the control bar already stacked at, kept so nothing that worked
 * before regresses.
 */
export const COMPACT_MAX_WIDTH = 980;

/**
 * Heights at or below this get the compact shell. The full shell spends ~361px on
 * chrome, and the node params panel wants ~300px to show a model row and a knob
 * grid without scrolling, so anything under ~660px is already squeezed.
 *
 * Keep this and {@link COMPACT_MAX_WIDTH} in step with the pre-paint bootstrap in
 * `index.template.html`, which applies the same test before this module loads.
 */
export const COMPACT_MAX_HEIGHT = 620;

/**
 * The same two limits when the primary pointer is a finger (a tablet). 1200x850
 * takes in the iPad mini, 9.7", 10.2", Air and 11" Pro in landscape (1024x768 to
 * 1194x834 CSS px), which get the compact shell split; the 12.9" Pro at 1366x1024
 * keeps the full shell, which has the room for it.
 */
export const COMPACT_TOUCH_MAX_WIDTH = 1200;

export const COMPACT_TOUCH_MAX_HEIGHT = 850;

/** Whether the primary pointer is a finger. Auto reads it; nothing else should. */
export const COARSE_POINTER_QUERY = "(pointer: coarse)";

/** What Auto picks for a viewport of this size, given the kind of pointer. */
export function densityForViewportSize(width: number, height: number, coarsePointer: boolean): Density {
  const maxWidth = coarsePointer ? COMPACT_TOUCH_MAX_WIDTH : COMPACT_MAX_WIDTH;
  const maxHeight = coarsePointer ? COMPACT_TOUCH_MAX_HEIGHT : COMPACT_MAX_HEIGHT;
  return width <= maxWidth || height <= maxHeight ? "compact" : "full";
}

/**
 * Landscape gets the rail; portrait and square keep the stacked bars. The
 * pre-paint bootstrap in `index.template.html` applies the same test.
 */
export function compactLayoutForViewport(width: number, height: number): CompactLayout {
  return width > height ? "rail" : "stack";
}

/**
 * The smallest stacked compact viewport that shows the signal chain and the
 * selected effect together, as the full shell does, instead of taking turns on
 * the Chain/Effect tabs (ts/compactStage.ts).
 *
 * The tabs exist because a short window cannot give both a useful share. A tall
 * narrow one can: an app window snapped to half of a 1920x1080 monitor is about
 * 960x1040, compact by width alone, and with the tabs it showed a two-line chain
 * above 700px of nothing. Width matters too, since a narrow window wraps the
 * chain onto more lines: a phone held upright needs the chain's whole stage.
 *
 * Stamped on the root as `data-compact-split`. The pre-paint bootstrap in
 * `index.template.html` applies the same test.
 */
export const COMPACT_SPLIT_MIN_WIDTH = 600;

export const COMPACT_SPLIT_MIN_HEIGHT = 800;

/**
 * The shortest landscape (rail) viewport that splits. The rail's chrome is the
 * control bar and the footer, about 85px, so 600 leaves a ~515px stage: a one-
 * or two-line chain over an editor that shows a model row and a row of knobs.
 * An iPad in landscape (768 tall and up) and a 1024x600 panel split; a phone
 * (under 430) and an 800x480 panel keep the Chain/Effect turn-taking, where the
 * chain alone fills the stage. The rail is never narrower than it is tall, so
 * width needs no limit of its own.
 */
export const COMPACT_RAIL_SPLIT_MIN_HEIGHT = 600;

export function compactSplitForViewport(width: number, height: number): boolean {
  if (compactLayoutForViewport(width, height) === "rail") {
    return height >= COMPACT_RAIL_SPLIT_MIN_HEIGHT;
  }
  return width >= COMPACT_SPLIT_MIN_WIDTH && height >= COMPACT_SPLIT_MIN_HEIGHT;
}

/**
 * The tallest compact viewport that counts as short: a phone held sideways (354
 * to 430) and an 800x480 panel. There a list or a dialog has about 300px under
 * its header, so the preset library folds its folders into a drop-down, cards get
 * denser and dialogs lay out in columns. 1024x600 is not short.
 *
 * Stamped on the root as `data-compact-short`. The pre-paint bootstrap in
 * `index.template.html` applies the same test.
 */
export const COMPACT_SHORT_MAX_HEIGHT = 500;

export function compactShortForViewport(height: number): boolean {
  return height <= COMPACT_SHORT_MAX_HEIGHT;
}

const DENSITY_PREFERENCES: readonly DensityPreference[] = ["auto", "compact", "full"];

let preference: DensityPreference = "auto";
let density: Density = "full";
let layout: CompactLayout | null = null;
let split = false;
let short = false;
let initialized = false;
let evaluateRaf = 0;

export function isDensityPreference(value: unknown): value is DensityPreference {
  return typeof value === "string" && DENSITY_PREFERENCES.includes(value as DensityPreference);
}

/**
 * The zoom actually applied to the document. `windowSettings` owns this setting,
 * but reading it back off the element keeps this module a leaf — importing the
 * settings module from here would put a cycle through `bridge`.
 */
function currentZoom(): number {
  const inline = Number.parseFloat(document.body?.style.zoom || "");
  if (Number.isFinite(inline) && inline > 0) {
    return inline;
  }
  const computed = Number.parseFloat(window.getComputedStyle(document.body).zoom);
  return Number.isFinite(computed) && computed > 0 ? computed : 1;
}

/** Viewport in the CSS pixels the laid-out content actually gets, zoom included. */
export function effectiveViewport(): { width: number; height: number } {
  const zoom = currentZoom();
  return {
    width: window.innerWidth / zoom,
    height: window.innerHeight / zoom,
  };
}

function isCoarsePointer(): boolean {
  return typeof window.matchMedia === "function" && window.matchMedia(COARSE_POINTER_QUERY).matches;
}

function densityForViewport(): Density {
  const { width, height } = effectiveViewport();
  return densityForViewportSize(width, height, isCoarsePointer());
}

function resolveDensity(): Density {
  return preference === "auto" ? densityForViewport() : preference;
}

export function getDensity(): Density {
  return density;
}

export function isCompact(): boolean {
  return density === "compact";
}

/** How the compact shell is arranged, or null at full density. */
export function getCompactLayout(): CompactLayout | null {
  return layout;
}

/** Whether a compact viewport is tall enough to show the chain and the effect together. */
export function isCompactSplit(): boolean {
  return split;
}

/** At compact density, whether the viewport is short (see {@link COMPACT_SHORT_MAX_HEIGHT}). */
export function isCompactShort(): boolean {
  return short;
}

/** At compact density, whether the chain and the effect take turns on the stage. */
export function isCompactStaged(): boolean {
  return density === "compact" && !split;
}

export function getDensityPreference(): DensityPreference {
  return preference;
}

function applyResolvedDensity(): void {
  const next = resolveDensity();
  const { width, height } = effectiveViewport();
  const nextLayout = next === "compact" ? compactLayoutForViewport(width, height) : null;
  const nextSplit = next === "compact" && compactSplitForViewport(width, height);
  const nextShort = next === "compact" && compactShortForViewport(height);
  const root = document.documentElement;
  root.dataset.densityPref = preference;

  const unchanged = next === density && root.dataset.density === next
    && nextLayout === layout && (root.dataset.compactLayout ?? null) === nextLayout
    && nextSplit === split && ("compactSplit" in root.dataset) === nextSplit
    && nextShort === short && ("compactShort" in root.dataset) === nextShort;
  if (unchanged) {
    return;
  }

  density = next;
  layout = nextLayout;
  split = nextSplit;
  short = nextShort;
  if (nextShort) {
    root.dataset.compactShort = "";
  } else {
    delete root.dataset.compactShort;
  }
  root.dataset.density = next;
  if (nextLayout) {
    root.dataset.compactLayout = nextLayout;
  } else {
    delete root.dataset.compactLayout;
  }
  if (nextSplit) {
    root.dataset.compactSplit = "";
  } else {
    delete root.dataset.compactSplit;
  }
  // A layout change on its own is announced too: turning a compact window from
  // portrait to landscape moves the navigation to another edge, and a stacked one
  // growing tall enough puts the chain and the effect on screen together. Anything
  // that sizes itself off the shell needs to hear that as much as a density change.
  window.dispatchEvent(new CustomEvent("densityChanged", { detail: { density: next, layout: nextLayout, preference } }));
}

function scheduleEvaluate(): void {
  if (evaluateRaf) {
    return;
  }
  evaluateRaf = requestAnimationFrame(() => {
    evaluateRaf = 0;
    applyResolvedDensity();
  });
}

/**
 * Apply a preference without persisting it — for the value arriving from stored
 * app settings, which would otherwise be written straight back to the engine.
 */
export function applyDensityPreference(next: DensityPreference): void {
  preference = next;
  applyResolvedDensity();
  // Fires on the stored-settings path too, not just on a user change: the footer
  // selector is bound during bootstrap, long before app settings arrive, so this
  // is how it learns what the stored preference turned out to be.
  window.dispatchEvent(new CustomEvent("densityPreferenceChanged", { detail: { preference: next } }));
}

/** Apply a preference the user just chose, and remember it. */
export function setDensityPreference(next: DensityPreference): void {
  applyDensityPreference(next);
  updateAppSetting(DENSITY_SETTING, next);
}

/** Pick the stored preference out of an app-settings blob, if it carries one. */
export function applyDensityAppSettings(appSettings: Record<string, unknown> | undefined): void {
  const stored = appSettings?.[DENSITY_SETTING];
  applyDensityPreference(isDensityPreference(stored) ? stored : "auto");
}

export function initCompactMode(): void {
  if (initialized) {
    applyResolvedDensity();
    return;
  }
  initialized = true;

  window.addEventListener("resize", scheduleEvaluate);
  // Zoom changes the layout box without changing the window, so the resize event
  // never fires for them.
  window.addEventListener("uiSettingsApplied", scheduleEvaluate);
  // A convertible that loses its keyboard, or a tablet given a mouse, changes the
  // primary pointer without changing the window.
  if (typeof window.matchMedia === "function") {
    window.matchMedia(COARSE_POINTER_QUERY).addEventListener?.("change", scheduleEvaluate);
  }
  applyResolvedDensity();
}
