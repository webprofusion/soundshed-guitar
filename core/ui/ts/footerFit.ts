/**
 * The compact footer bar holds as much as fits, in a fixed order of priority, and
 * the OPTIONS sheet holds the rest.
 *
 * At full density the footer is one long bar: chain history and A/B, then the
 * demo clip, tools, tempo and view options. At compact density everything after
 * the history folds into the OPTIONS sheet, which left a phone held sideways with
 * hundreds of spare pixels on the bar while Record, the tempo and the metronome's
 * play sat a tap further away. So here each item has a place on the bar and a
 * place in the sheet, and goes to the bar in priority order:
 *
 *   Setlists, Tuner (always on the bar), Record, Loop, undo/redo, A/B, copy,
 *   the tempo, the metronome's play; OPTIONS stays at the end.
 *
 * Decided by measuring, on every resize of the bar and whenever an item is shown
 * or hidden: every item goes to the bar, then the lowest priority leaves for the
 * sheet, one at a time, until the bar's contents fit on one line. Moving the
 * elements themselves (not copies) keeps every listener and state the modules
 * that own them set up. Away from compact density each item goes back where the
 * full bar has it.
 */

/** Where one footer item lives at full density, on the compact bar, and in the compact sheet. */
interface FooterItem {
  elements: HTMLElement[];
  /** Placeholders where the elements sit at full density. */
  home: Comment[];
  /** True when the full bar has them on the bar (the chain history). */
  homeOnBar: boolean;
  /** The compact bar's container for them, or null when the home is on the bar. */
  bar: HTMLElement | null;
  /** The compact sheet's container for them, or null when the home is in the sheet. */
  sheet: HTMLElement | null;
}

/** The items that move, highest priority first (ids, or selectors for the A/B switch). */
export const FOOTER_PRIORITY = [
  ["#footer-riff-record-btn"],
  ["#footer-looper-btn"],
  ["#footer-chain-undo", "#footer-chain-redo"],
  [".footer-ab-switch"],
  ["#footer-chain-copy"],
  ["#footer-bpm-btn"],
  ["#footer-metronome-toggle"],
] as const;

/**
 * How many items (from the top of the priority list) the bar has room for, given
 * the width each would add and the room left once the fixed controls are placed.
 * An item with no width (hidden) costs nothing and stays with its neighbours.
 */
export function itemsThatFit(widths: readonly number[], room: number): number {
  let used = 0;
  for (let index = 0; index < widths.length; index++) {
    used += widths[index];
    if (used > room + 0.5) {
      return index;
    }
  }
  return widths.length;
}

function placeOnBar(item: FooterItem): void {
  if (item.homeOnBar || !item.bar) {
    item.home.forEach((marker, index) => marker.after(item.elements[index]));
  } else {
    item.elements.forEach((element) => item.bar?.appendChild(element));
  }
}

function placeInSheet(item: FooterItem): void {
  if (!item.homeOnBar || !item.sheet) {
    item.home.forEach((marker, index) => marker.after(item.elements[index]));
  } else {
    item.elements.forEach((element) => item.sheet?.appendChild(element));
  }
}

let installed = false;

/**
 * @param isActive whether the footer is in its compact form (the sheet folded behind OPTIONS).
 */
export function initializeFooterFit(isActive: () => boolean): void {
  const footer = document.querySelector<HTMLElement>(".footer-bar");
  const shortcuts = footer?.querySelector<HTMLElement>(".footer-compact-shortcuts") ?? null;
  const tempoBar = footer?.querySelector<HTMLElement>(".footer-compact-tempo") ?? null;
  const historySheet = footer?.querySelector<HTMLElement>(".footer-history-sheet-group") ?? null;
  const toggle = document.getElementById("footer-actions-toggle");
  if (installed || !footer || !shortcuts || !tempoBar || !historySheet || !toggle) {
    return;
  }

  const items: FooterItem[] = [];
  for (const selectors of FOOTER_PRIORITY) {
    const elements = selectors.map((selector) => footer.querySelector<HTMLElement>(selector)).filter(Boolean) as HTMLElement[];
    if (elements.length !== selectors.length) {
      continue;
    }
    const homeOnBar = elements[0].closest(".footer-chain-history") !== null;
    const home = elements.map((element) => {
      const marker = document.createComment(`footer home: ${element.id || element.className}`);
      element.before(marker);
      return marker;
    });
    const toTempo = selectors[0] === "#footer-bpm-btn" || selectors[0] === "#footer-metronome-toggle";
    items.push({
      elements,
      home,
      homeOnBar,
      bar: homeOnBar ? null : toTempo ? tempoBar : shortcuts,
      sheet: homeOnBar ? historySheet : null,
    });
  }
  installed = true;

  const visibleWidth = (element: HTMLElement): number => {
    const width = element.hidden ? 0 : element.getBoundingClientRect().width;
    if (width <= 0) {
      return 0;
    }
    const style = getComputedStyle(element);
    return width + (Number.parseFloat(style.marginLeft) || 0) + (Number.parseFloat(style.marginRight) || 0);
  };
  const gapOf = (element: HTMLElement): number => Number.parseFloat(getComputedStyle(element).columnGap) || 0;

  let fitting = false;
  const refit = (): void => {
    // Nothing moves under a finger: while the sheet is open (starting the
    // metronome changes the tempo's width) the fit waits for it to close.
    if (fitting || (footer.classList.contains("footer-actions-open") && isActive())) {
      return;
    }
    fitting = true;
    try {
      if (!isActive()) {
        items.forEach((item) => (item.homeOnBar ? placeOnBar(item) : placeInSheet(item)));
        return;
      }
      // Every item on the bar, then measure what each one adds there.
      items.forEach(placeOnBar);
      const widths = items.map((item) => {
        const width = item.elements.reduce((sum, element) => sum + visibleWidth(element), 0);
        if (width <= 0) {
          return 0;
        }
        const parent = item.elements[0].parentElement;
        return width + (parent ? gapOf(parent) * item.elements.length : 0);
      });
      // The room: the bar's content box, less what is on it besides the moving items.
      const style = getComputedStyle(footer);
      const inner = footer.clientWidth - (Number.parseFloat(style.paddingLeft) || 0) - (Number.parseFloat(style.paddingRight) || 0);
      const inFlow = Array.from(footer.children).filter((child): child is HTMLElement =>
        child instanceof HTMLElement && getComputedStyle(child).position !== "absolute" && child.getBoundingClientRect().width > 0);
      const total = inFlow.reduce((sum, child) => sum + child.getBoundingClientRect().width, 0) + gapOf(footer) * Math.max(0, inFlow.length - 1);
      const moving = widths.reduce((sum, width) => sum + width, 0);
      const room = inner - (total - moving);
      const keep = itemsThatFit(widths, room);
      items.slice(keep).forEach(placeInSheet);
    } finally {
      fitting = false;
    }
  };

  new ResizeObserver(() => refit()).observe(footer);
  const attributes = new MutationObserver(() => refit());
  for (const item of items) {
    for (const element of item.elements) {
      attributes.observe(element, { attributes: true, attributeFilter: ["hidden"] });
    }
  }
  attributes.observe(document.documentElement, { attributes: true, attributeFilter: ["data-density", "data-compact-layout"] });
  // Record and the tempo show only in the standalone app, which a body class says.
  attributes.observe(document.body, { attributes: true, attributeFilter: ["class"] });
  // The sheet closing lets a deferred fit run.
  attributes.observe(footer, { attributes: true, attributeFilter: ["class"] });
  // The tempo's text changes its width ("88.0 BPM", "120.0 BPM").
  const bpmValue = document.getElementById("footer-bpm-value");
  if (bpmValue) {
    attributes.observe(bpmValue, { childList: true, characterData: true, subtree: true });
  }
  window.addEventListener("densityChanged", refit);
  refit();
}
