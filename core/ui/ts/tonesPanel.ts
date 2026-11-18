/**
 * The Tones panel: tabs for Community Presets (tone sharing, in toneSharingPanel.ts), Local
 * Presets and the Tone 3000 browser. This is the public face; the parts are in ./tonesPanel/.
 *
 * navigation.ts opens it through tonesPanelLink.ts rather than importing this, so main.ts
 * imports it to have it loaded.
 */

export { activateTonesTab, initializeTonesPanel, isTonesTabEnabled, resolveTonesTabId } from "./tonesPanel/tabs.js";
export type { TonesTabId } from "./tonesPanel/tabs.js";
