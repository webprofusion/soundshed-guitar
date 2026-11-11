/**
 * The Scale Guide's instrument button: tuning, capo and hand are chosen once and then left alone,
 * so they sit behind one button that says what is set, with how LISTEN marks the neck. The button
 * is a `<details>`; this closes it on a click elsewhere or Escape, as a menu would.
 */

import type { ScaleGuideSettings, ScaleGuideView } from "./types.js";

/** What the button reads: "Standard E", "Drop D · Capo 2 · Left-handed". */
export function instrumentLabel(view: ScaleGuideView, settings: ScaleGuideSettings): string {
  const parts = [view.tuning.name];

  if (view.capo > 0) {
    parts.push(`Capo ${view.capo}`);
  }

  if (settings.leftHanded) {
    parts.push("Left-handed");
  }

  return parts.join(" · ");
}

/** The menu's switches, as saved. */
export function showSetupChoices(settings: ScaleGuideSettings): void {
  const set = (id: string, on: boolean): void => {
    const box = document.getElementById(id) as HTMLInputElement | null;

    if (box) {
      box.checked = on;
    }
  };

  set("sg-left-handed", settings.leftHanded);
  set("sg-listen-everywhere", settings.listenEverywhere);
}

export function bindSetupMenu(root: HTMLElement): void {
  const menu = root.querySelector<HTMLDetailsElement>("#sg-setup");

  if (!menu) {
    return;
  }

  document.addEventListener("pointerdown", (event) => {
    if (menu.open && !menu.contains(event.target as Node | null)) {
      menu.open = false;
    }
  });

  menu.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && menu.open) {
      menu.open = false;
      menu.querySelector<HTMLElement>("summary")?.focus();
    }
  });
}
