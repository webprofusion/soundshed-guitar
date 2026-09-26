/**
 * The floating + in the chain's corner, and the menu it opens: Add FX… and Add Scene….
 *
 * Add FX… opens the FX library, whose effects are dragged onto the chain. On a small
 * display that does not work. The chain has the stage to itself there, so the library
 * can only open over it, and on a touch screen a drag that starts upwards out of the
 * library's list scrolls the list instead. So when the chain has the stage to itself
 * Add FX… opens the effect chooser at the end of the chain instead: the list the +
 * between two nodes opens, where a tap adds the effect. A compact window tall enough
 * to show the effect under the chain keeps the library, which drops over the effect.
 *
 * Adding a scene belongs to the signal path, which hands it in when it binds the menu.
 */

import { isCompactStaged } from "../compactMode.js";
import { expandFxSelector } from "../fxSelector.js";
import {
  signalPathAddMenu,
  signalPathAddMenuOptions,
  signalPathAddMenuTrigger,
  signalPathAddSceneButton,
  signalPathNodesElement,
} from "./state.js";

let initialized = false;

function setSignalPathAddMenuOpen(open: boolean): void {
  if (!signalPathAddMenu || !signalPathAddMenuTrigger || !signalPathAddMenuOptions) {
    return;
  }
  signalPathAddMenuOptions.hidden = !open;
  signalPathAddMenuTrigger.setAttribute("aria-expanded", String(open));
  if (open) {
    const triggerRect = signalPathAddMenuTrigger.getBoundingClientRect();
    signalPathAddMenuOptions.style.right = `${Math.max(8, window.innerWidth - triggerRect.right)}px`;
    signalPathAddMenuOptions.style.bottom = `${Math.max(8, window.innerHeight - triggerRect.top + 8)}px`;
  } else {
    signalPathAddMenuOptions.style.removeProperty("right");
    signalPathAddMenuOptions.style.removeProperty("bottom");
  }
}

export function updateSignalPathAddMenuAvailability(available: boolean): void {
  signalPathAddMenu?.classList.toggle("is-disabled", !available);
  if (signalPathAddMenuTrigger) {
    signalPathAddMenuTrigger.disabled = !available;
  }
  if (!available) {
    setSignalPathAddMenuOpen(false);
  }
}

/** The + on the connection into the output: the end of the main chain. */
function endOfChainAddButton(): HTMLElement | null {
  return signalPathNodesElement?.querySelector<HTMLElement>('.signal-add-btn[data-edge-to="__output__"]') ?? null;
}

function openAddEffect(): void {
  const endOfChain = isCompactStaged() ? endOfChainAddButton() : null;
  if (!endOfChain) {
    expandFxSelector({ focusSearch: true });
    return;
  }
  // The chooser hangs from its button, so the button has to be on screen: a long chain
  // scrolls within its stage.
  endOfChain.scrollIntoView({ block: "nearest", inline: "nearest" });
  endOfChain.click();
}

export function initSignalPathAddMenu(options: { onAddScene: () => void }): void {
  if (initialized) {
    return;
  }
  initialized = true;

  signalPathAddMenuTrigger?.addEventListener("click", (event) => {
    event.stopPropagation();
    setSignalPathAddMenuOpen(signalPathAddMenuOptions?.hidden ?? true);
  });

  document.getElementById("signal-path-floating-add-fx")?.addEventListener("click", () => {
    setSignalPathAddMenuOpen(false);
    openAddEffect();
  });

  signalPathAddSceneButton?.addEventListener("click", () => {
    setSignalPathAddMenuOpen(false);
    options.onAddScene();
  });

  document.addEventListener("click", (event) => {
    if (!signalPathAddMenu?.contains(event.target as Node)) {
      setSignalPathAddMenuOpen(false);
    }
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && signalPathAddMenuOptions && !signalPathAddMenuOptions.hidden) {
      setSignalPathAddMenuOpen(false);
      signalPathAddMenuTrigger?.focus();
    }
  });
}
