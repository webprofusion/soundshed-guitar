/**
 * The Tones panel's sections: Community Presets, Your Presets and Tone 3000.
 *
 * Which tab is selected is separate from whether its content has started. A tab can be
 * selected while Tones is closed (a restored view, a feature switched off under it), and
 * two of the three talk to a server when they start, so a tab starts only once it is on
 * screen: when it is picked with Tones open, or when Tones opens on it.
 */

import { flushShownViews } from "../deferredView.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "../featureFlags.js";
import { initTone3000Browser, refreshTone3000BrowserOnShow } from "../tone3000Browser.js";
import { initializeToneSharingPanel } from "../toneSharingPanel.js";
import { notifyTonesTabChanged, setTonesPanelHooks } from "../tonesPanelLink.js";
import { initLocalPresetsBrowser } from "./localPresets.js";

export type TonesTabId = "community" | "local" | "tone3000";

/** In the order the strip shows them, which is also the order a fallback is looked for in. */
export const TONES_TAB_IDS: readonly TonesTabId[] = ["community", "local", "tone3000"];

export function isTonesTabId(value: unknown): value is TonesTabId {
  return TONES_TAB_IDS.includes(value as TonesTabId);
}

export function isTonesTabEnabled(tabId: TonesTabId): boolean {
  switch (tabId) {
    case "community":
      return isFeatureEnabled(Features.ToneSharing);
    case "tone3000":
      return isFeatureEnabled(Features.Tone3000);
    default:
      return true;
  }
}

/** The tab to show for a requested one: itself if it exists and is on, else the first that is. */
export function resolveTonesTabId(preferredTabId: string | null | undefined): TonesTabId {
  if (isTonesTabId(preferredTabId) && isTonesTabEnabled(preferredTabId)) {
    return preferredTabId;
  }
  // Your Presets has no feature flag, so there is always one.
  return TONES_TAB_IDS.find((tabId) => isTonesTabEnabled(tabId)) ?? "local";
}

let selectedTab: TonesTabId = "community";
let tone3000Started = false;
let bound = false;

function tabButtons(): HTMLElement[] {
  return Array.from(document.querySelectorAll<HTMLElement>(".tones-tab-btn"));
}

function isTonesPanelShown(): boolean {
  return Boolean(document.getElementById("panel-sharing")?.classList.contains("active"));
}

function startShownTab(): void {
  if (!isTonesPanelShown()) {
    return;
  }
  if (selectedTab === "community") {
    initializeToneSharingPanel();
  } else if (selectedTab === "tone3000") {
    // Its first search goes out here rather than when Settings opens.
    if (!tone3000Started) {
      tone3000Started = true;
      initTone3000Browser();
    } else {
      // Coming back to it fetches only if its results have gone stale.
      refreshTone3000BrowserOnShow();
    }
  } else {
    initLocalPresetsBrowser();
  }
}

/** Selects a tab, or the nearest enabled one, and starts it if Tones is on screen. */
export function activateTonesTab(tabId: string | null | undefined): TonesTabId {
  const resolved = resolveTonesTabId(tabId);
  selectedTab = resolved;

  tabButtons().forEach((button) => {
    const isActive = button.dataset.tonesTab === resolved;
    button.classList.toggle("active", isActive);
    button.setAttribute("aria-selected", isActive ? "true" : "false");
    button.tabIndex = isActive ? 0 : -1;
  });

  document.querySelectorAll<HTMLElement>(".tones-tab-panel").forEach((panel) => {
    panel.classList.toggle("active", panel.id === `tones-tab-${resolved}`);
  });

  startShownTab();
  flushShownViews();
  // navigation.ts keeps the tab in the saved view.
  notifyTonesTabChanged(resolved);
  return resolved;
}

/** Hides the tabs whose feature is off, and moves off one that has just been switched off. */
function syncTonesTabAvailability(): void {
  tabButtons().forEach((button) => {
    const tabId = button.dataset.tonesTab;
    button.toggleAttribute("hidden", !(isTonesTabId(tabId) && isTonesTabEnabled(tabId)));
  });
  if (resolveTonesTabId(selectedTab) !== selectedTab) {
    activateTonesTab(selectedTab);
  }
}

function bindTonesTabs(): void {
  const buttons = tabButtons();
  buttons.forEach((button) => {
    button.addEventListener("click", () => activateTonesTab(button.dataset.tonesTab));

    button.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") {
        return;
      }
      const visible = buttons.filter((candidate) => !candidate.hasAttribute("hidden"));
      const from = visible.indexOf(button);
      if (visible.length < 2 || from === -1) {
        return;
      }
      event.preventDefault();
      const next = visible[(from + (event.key === "ArrowRight" ? 1 : -1) + visible.length) % visible.length];
      next.focus();
      activateTonesTab(next.dataset.tonesTab);
    });
  });

  // A link to another tab from inside one, e.g. Your Presets' empty state.
  document.getElementById("panel-sharing")?.addEventListener("click", (event) => {
    const link = (event.target as HTMLElement | null)?.closest<HTMLElement>("[data-tones-open-tab]");
    if (link) {
      activateTonesTab(link.dataset.tonesOpenTab);
    }
  });

  document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, syncTonesTabAvailability);
}

/**
 * Called each time Tones is opened, after its panel is on screen. Shows `tabId` if one is
 * asked for, otherwise the tab it was left on.
 */
export function initializeTonesPanel(tabId?: string | null): void {
  if (!document.getElementById("panel-sharing")) {
    return;
  }
  if (!bound) {
    bound = true;
    bindTonesTabs();
  }
  // The feature flags arrive with the app settings, which may be after the last sync.
  syncTonesTabAvailability();
  activateTonesTab(tabId ?? selectedTab);
}

// navigation.ts opens the panel through tonesPanelLink.ts; see there for why.
setTonesPanelHooks({
  open: initializeTonesPanel,
  activateTab: (tabId) => {
    activateTonesTab(tabId);
  },
});
