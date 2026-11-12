/**
 * The compact footer bar holds as much as fits, in a fixed order of priority, and
 * the OPTIONS sheet holds the rest.
 *
 * At full density the footer is one long bar: chain history and A/B, then the
 * demo clip, tools, tempo and view options. At compact density the bar keeps the
 * full bar's order for what it shows: the demo clip and its play button at the
 * left, then the tools (Setlists, Tuner, Record, Loop), then the tempo, the
 * metronome and its play by OPTIONS at the right. The chain's history and A/B are
 * always in the sheet, under its "Chain" row. On the landscape rail the Tuner and
 * Loop are rail buttons (css/compact/rail.css), so Loop stays in the sheet, where
 * the rail's CSS hides it.
 *
 * Each fitted item has a place on the bar and a place in the sheet, and goes to
 * the bar in priority order:
 *
 *   Record, Loop, the tempo, the metronome, the metronome's play.
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
  /** Whether the compact bar may have them now (Loop gives way to the rail's button). */
  allowed: () => boolean;
}

/** The items that move by fit, highest priority first. */
export const FOOTER_PRIORITY = [
  ["#footer-riff-record-btn"],
  ["#footer-looper-btn"],
  ["#footer-bpm-btn"],
  ["#footer-metronome-btn"],
  ["#footer-metronome-toggle"],
] as const;

/** The chain's history and A/B: on the full bar, and always in the compact sheet. */
export const FOOTER_SHEET_ONLY = [
  ["#footer-chain-undo", "#footer-chain-redo"],
  [".footer-ab-switch"],
  ["#footer-chain-copy"],
] as const;

/** The demo clip's chooser and play button: in the sheet's demo row at full density, first on the compact bar. */
export const FOOTER_DEMO_ON_BAR = ["#footer-demo-audio-select", "#footer-play-demo-audio"] as const;

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

const isRail = (): boolean => document.documentElement.dataset.compactLayout === "rail";

/**
 * @param isActive whether the footer is in its compact form (the sheet folded behind OPTIONS).
 */
export function initializeFooterFit(isActive: () => boolean): void {
  const footer = document.querySelector<HTMLElement>(".footer-bar");
  const demoBar = footer?.querySelector<HTMLElement>(".footer-compact-demo") ?? null;
  const shortcuts = footer?.querySelector<HTMLElement>(".footer-compact-shortcuts") ?? null;
  const tempoBar = footer?.querySelector<HTMLElement>(".footer-compact-tempo") ?? null;
  const historySheet = footer?.querySelector<HTMLElement>(".footer-history-sheet-group") ?? null;
  const demoContainer = document.getElementById("footer-demo-audio-container");
  const toggle = document.getElementById("footer-actions-toggle");
  if (installed || !footer || !demoBar || !shortcuts || !tempoBar || !historySheet || !demoContainer || !toggle) {
    return;
  }

  const makeItem = (selectors: readonly string[]): FooterItem | null => {
    const elements = selectors.map((selector) => footer.querySelector<HTMLElement>(selector)).filter(Boolean) as HTMLElement[];
    if (elements.length !== selectors.length) {
      return null;
    }
    const homeOnBar = elements[0].closest(".footer-chain-history") !== null;
    const home = elements.map((element) => {
      const marker = document.createComment(`footer home: ${element.id || element.className}`);
      element.before(marker);
      return marker;
    });
    const toTempo = elements[0].closest(".footer-tempo-group") !== null;
    const isLoop = selectors[0] === "#footer-looper-btn";
    return {
      elements,
      home,
      homeOnBar,
      bar: homeOnBar ? null : toTempo ? tempoBar : shortcuts,
      sheet: homeOnBar ? historySheet : null,
      allowed: isLoop ? () => !isRail() : () => true,
    };
  };
  const items = FOOTER_PRIORITY.map(makeItem).filter((item): item is FooterItem => item !== null);
  const sheetOnly = FOOTER_SHEET_ONLY.map(makeItem).filter((item): item is FooterItem => item !== null);
  installed = true;

  /**
   * The demo clip's chooser and play button. demoAudio.ts draws them into their
   * container (again when the clip list first arrives), so they are looked up each
   * time rather than held, and copies left on the bar by a redraw are dropped.
   */
  const placeDemo = (): void => {
    if (demoContainer.querySelector(FOOTER_DEMO_ON_BAR[0])) {
      demoBar.replaceChildren();
    }
    const elements = FOOTER_DEMO_ON_BAR.map((selector) => footer.querySelector<HTMLElement>(selector))
      .filter((element): element is HTMLElement => element !== null);
    if (isActive()) {
      elements.forEach((element) => demoBar.appendChild(element));
      return;
    }
    const controls = demoContainer.querySelector<HTMLElement>(".footer-demo-controls");
    if (controls && elements.length && elements[0].parentElement === demoBar) {
      controls.prepend(...elements);
    }
  };

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
      placeDemo();
      if (!isActive()) {
        [...sheetOnly, ...items].forEach((item) => (item.homeOnBar ? placeOnBar(item) : placeInSheet(item)));
        return;
      }
      sheetOnly.forEach(placeInSheet);
      const allowed = items.filter((item) => item.allowed());
      items.filter((item) => !item.allowed()).forEach(placeInSheet);
      // Every item on the bar, then measure what each one adds there, with the demo
      // clip's chooser at its narrowest: it gives up width before an item leaves the
      // bar. (A data attribute, which the class observer below does not see.)
      allowed.forEach(placeOnBar);
      footer.dataset.fitMeasuring = "";
      const widths = allowed.map((item) => {
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
      allowed.slice(keep).forEach(placeInSheet);
    } finally {
      delete footer.dataset.fitMeasuring;
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
  // The demo controls are drawn (and redrawn) after this runs.
  attributes.observe(demoContainer, { childList: true });
  // The tempo's text changes its width ("88.0 BPM", "120.0 BPM").
  const bpmValue = document.getElementById("footer-bpm-value");
  if (bpmValue) {
    attributes.observe(bpmValue, { childList: true, characterData: true, subtree: true });
  }
  window.addEventListener("densityChanged", refit);
  refit();
}
