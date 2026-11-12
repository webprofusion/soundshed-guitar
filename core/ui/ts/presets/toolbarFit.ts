/**
 * The preset name comes first on the top strip. At compact density the toolbar
 * beside it gives way when the two do not fit: its lowest-priority buttons leave
 * the bar, one at a time, and appear as entries in the preset "..." menu instead
 * (Random, then Publish, then Edit, then Save; New and "..." always stay). The
 * name's pill keeps at least MIN_NAME_PX, or MIN_NAME_SHARE of the strip, which
 * ever is larger, and BADGE_MIN_PX when the preset has a status badge (Factory)
 * to show; the toolbar's own row must not overflow either (the narrow layout
 * puts it on a line of its own).
 *
 * Decided by measuring, on every resize of the strip and whenever a button is
 * shown, hidden, enabled or disabled: all the buttons go back, then leave in
 * order until the room is there. Each menu entry clicks its button, so it does
 * exactly what the button does, and carries the button's disabled and hidden
 * state.
 */

export const MIN_NAME_PX = 200;
export const MIN_NAME_SHARE = 0.4;

/** The buttons that may leave the bar, the first to go first. */
export const OVERFLOW_ORDER = ["preset-random-btn", "preset-publish-btn", "preset-edit-btn", "preset-save-btn"] as const;

const OVERFLOW_CLASS = "is-toolbar-overflow";

/** The pill width at which its status badge (Factory, ...) shows: the container query in css/compact/shell.css. */
export const BADGE_MIN_PX = 340;

export interface ToolbarRoom {
  /** The name pill's width. */
  name: number;
  /** The pill has a status badge to show, which needs BADGE_MIN_PX. */
  badge?: boolean;
  /** The pill's content-box width, which the badge's container query measures (defaults to name). */
  content?: number;
  /** The whole strip's width. */
  strip: number;
  /** The toolbar is wider than the row it sits in. */
  toolbarOverflows: boolean;
}

export function nameRoomNeeded(stripWidth: number): number {
  return Math.max(MIN_NAME_PX, stripWidth * MIN_NAME_SHARE);
}

export function isShortOfRoom(room: ToolbarRoom): boolean {
  // A strip narrower than the floor itself can never give the name that much.
  const need = Math.min(nameRoomNeeded(room.strip), room.strip);
  if (room.toolbarOverflows || room.name + 0.5 < need) {
    return true;
  }
  // The badge's query measures the pill's content box.
  return Boolean(room.badge) && (room.content ?? room.name) + 0.5 < BADGE_MIN_PX;
}

/**
 * Puts every button back, then moves them out in order while the strip is short
 * of room. Returns how many left the bar.
 */
export function fitToolbar(
  buttons: readonly HTMLElement[],
  measure: () => ToolbarRoom,
  enabled: boolean,
): number {
  for (const button of buttons) {
    button.classList.remove(OVERFLOW_CLASS);
  }
  if (!enabled) {
    return 0;
  }
  const moved: HTMLElement[] = [];
  /** Moves buttons out while `short` holds; true when it no longer does. */
  const moveWhile = (short: (room: ToolbarRoom) => boolean): boolean => {
    for (const button of buttons) {
      if (!short(measure())) {
        return true;
      }
      if (button.hidden || moved.includes(button)) {
        continue; // not on the bar anyway: moving it frees nothing
      }
      button.classList.add(OVERFLOW_CLASS);
      moved.push(button);
    }
    return !short(measure());
  };
  // The name's own room first; then the badge's, but only if moving more buttons
  // gets there: a badge that would not show anyway is not worth a button.
  moveWhile((room) => isShortOfRoom({ ...room, badge: false }));
  const forName = moved.length;
  if (measure().badge && !moveWhile(isShortOfRoom)) {
    for (const button of moved.splice(forName)) {
      button.classList.remove(OVERFLOW_CLASS);
    }
  }
  return moved.length;
}

/** Shows each menu entry exactly when its button has left the bar, with the button's disabled state. */
export function syncOverflowItems(menu: ParentNode): void {
  for (const item of Array.from(menu.querySelectorAll<HTMLButtonElement>("[data-overflow-for]"))) {
    const button = document.getElementById(item.dataset.overflowFor ?? "") as HTMLButtonElement | null;
    const away = Boolean(button && button.classList.contains(OVERFLOW_CLASS) && !button.hidden);
    item.hidden = !away;
    item.disabled = Boolean(button?.disabled);
  }
}

/** A toolbar group whose every button has left the bar leaves too, so its gap does not stay behind. */
function syncGroups(buttons: readonly HTMLElement[]): void {
  const groups = new Set(buttons.map((b) => b.closest<HTMLElement>(".preset-toolbar-group")).filter(Boolean) as HTMLElement[]);
  for (const group of groups) {
    const left = Array.from(group.querySelectorAll<HTMLElement>(".preset-action-btn"))
      .some((b) => !b.classList.contains(OVERFLOW_CLASS));
    group.classList.toggle(OVERFLOW_CLASS, !left);
  }
}

let installed = false;

export function initializePresetToolbarFit(onItemClick: () => void): void {
  if (installed) {
    return;
  }
  const selector = document.getElementById("preset-selector");
  const toolbar = document.querySelector<HTMLElement>(".preset-toolbar");
  const menu = document.getElementById("preset-extra-actions-menu");
  const strip = selector?.closest<HTMLElement>(".control-bar-body-inner") ?? selector?.parentElement ?? null;
  const buttons = OVERFLOW_ORDER.map((id) => document.getElementById(id)).filter(Boolean) as HTMLElement[];
  if (!selector || !toolbar || !menu || !strip || !buttons.length) {
    return;
  }
  installed = true;

  // Children's boxes, not scrollWidth: the touch hit areas (::after) reach past the
  // bar's edge by design and would read as overflow.
  const overflows = (el: HTMLElement): boolean => {
    const right = el.getBoundingClientRect().right;
    return Array.from(el.children).some((child) => {
      const box = child.getBoundingClientRect();
      return box.width > 0 && box.right > right + 1;
    });
  };
  const measure = (): ToolbarRoom => {
    const row = toolbar.parentElement ?? toolbar;
    const status = selector.querySelector<HTMLElement>(".preset-selector-status");
    const style = getComputedStyle(selector);
    const name = selector.getBoundingClientRect().width;
    const frame = ["paddingLeft", "paddingRight", "borderLeftWidth", "borderRightWidth"] as const;
    return {
      name,
      content: name - frame.reduce((sum, key) => sum + (Number.parseFloat(style[key]) || 0), 0),
      badge: Boolean(status && !status.hidden && status.textContent?.trim()),
      strip: strip.clientWidth,
      toolbarOverflows: [toolbar, row, strip].some(overflows),
    };
  };

  let fitting = false;
  const refit = (): void => {
    if (fitting) {
      return;
    }
    fitting = true;
    try {
      const compact = document.documentElement.dataset.density === "compact";
      fitToolbar(buttons, measure, compact && strip.clientWidth > 0);
      syncGroups(buttons);
      syncOverflowItems(menu);
    } finally {
      fitting = false;
    }
  };

  for (const item of Array.from(menu.querySelectorAll<HTMLButtonElement>("[data-overflow-for]"))) {
    item.addEventListener("click", () => {
      onItemClick();
      document.getElementById(item.dataset.overflowFor ?? "")?.click();
    });
  }

  new ResizeObserver(() => refit()).observe(strip);
  const attributes = new MutationObserver(() => refit());
  for (const button of buttons) {
    attributes.observe(button, { attributes: true, attributeFilter: ["disabled", "hidden"] });
  }
  attributes.observe(document.documentElement, { attributes: true, attributeFilter: ["data-density", "data-compact-layout"] });
  // A preset with a status badge (Factory, ...) wants the badge's room; one without does not.
  const status = selector.querySelector(".preset-selector-status");
  if (status) {
    attributes.observe(status, { childList: true, characterData: true, subtree: true, attributes: true, attributeFilter: ["hidden"] });
  }
  refit();
}
