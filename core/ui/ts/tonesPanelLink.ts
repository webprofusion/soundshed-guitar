/**
 * The seam between navigation and the Tones panel.
 *
 * navigation.ts opens Tones and records which of its tabs is showing. The Tones panel's
 * Your Presets tab loads presets, and preset loading reaches back to navigation (a shared
 * preset that needs a Tone3000 key opens Settings), so navigation importing the panel would
 * be a cycle. Each side talks to this module instead, which imports nothing: the panel
 * registers how to open it, navigation registers what to do when the tab changes.
 */

export interface TonesPanelHooks {
  /** Tones has just come on screen: show `tabId`, or the tab it was left on. */
  open(tabId?: string | null): void;
  /** Select a tab, whether or not Tones is on screen. */
  activateTab(tabId: string): void;
}

let hooks: TonesPanelHooks | null = null;

/** Called once by tonesPanel/tabs.ts. */
export function setTonesPanelHooks(next: TonesPanelHooks): void {
  hooks = next;
}

export function openTonesPanel(tabId?: string | null): void {
  hooks?.open(tabId);
}

export function requestTonesTab(tabId: string): void {
  hooks?.activateTab(tabId);
}

let tabChangeListener: ((tabId: string) => void) | null = null;

/** Called once by navigation.ts, which keeps the tab in the saved view. */
export function setTonesTabChangeListener(listener: (tabId: string) => void): void {
  tabChangeListener = listener;
}

export function notifyTonesTabChanged(tabId: string): void {
  tabChangeListener?.(tabId);
}
