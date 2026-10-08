/**
 * Routing icons for the input modes, drawn the same way so they read as a set: the two inputs
 * on the left (filled when the mode uses them), the chain as a box in the middle, and the two
 * sides leaving on the right. Wider than tall (30 x 20), so the box and the curves stay
 * legible at the trigger's 24 x 16.
 *
 * - Mono: the input runs into one small box, which splits to both sides.
 * - Stereo: two lanes through one tall box, a single chain carrying both sides.
 * - Dual mono: two lanes, each through a box of its own, kept apart.
 *
 * Static markup, so it is safe to assign to innerHTML.
 */

export type InputModeIconName = "mono1" | "mono2" | "monoSum" | "stereo" | "dualMono" | "mono";

const OPEN =
  '<svg viewBox="0 0 30 20" fill="none" stroke="currentColor" stroke-width="1.7" ' +
  'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false">';
const CLOSE = "</svg>";

const IN_1 = '<circle cx="3" cy="5" r="2.2" fill="currentColor" stroke="none"/>';
const IN_2 = '<circle cx="3" cy="15" r="2.2" fill="currentColor" stroke="none"/>';
const IN_1_UNUSED = '<circle class="input-mode-icon-unused" cx="3" cy="5" r="1.7" stroke-width="1.3"/>';
const IN_2_UNUSED = '<circle class="input-mode-icon-unused" cx="3" cy="15" r="1.7" stroke-width="1.3"/>';
const IN_CENTRE = '<circle cx="3" cy="10" r="2.2" fill="currentColor" stroke="none"/><path d="M5.2 10h6.3"/>';

const FROM_1 = '<path d="M5.2 5c3.3 0 3 5 6.3 5"/>';
const FROM_2 = '<path d="M5.2 15c3.3 0 3-5 6.3-5"/>';
const MONO_CHAIN =
  '<rect x="11.5" y="6.5" width="7" height="7" rx="1.8"/>' +
  '<path d="M18.5 10c2.6 0 2.6-5 5.2-5H27M18.5 10c2.6 0 2.6 5 5.2 5H27"/>';

const LANES = '<path d="M5.2 5h6.3M18.5 5H27M5.2 15h6.3M18.5 15H27"/>';
const STEREO_CHAIN = '<rect x="11.5" y="1.5" width="7" height="17" rx="2"/>';
const DUAL_CHAINS = '<rect x="11.5" y="1.5" width="7" height="7" rx="1.8"/><rect x="11.5" y="11.5" width="7" height="7" rx="1.8"/>';

const ICONS: Record<InputModeIconName, string> = {
  mono1: OPEN + IN_1 + IN_2_UNUSED + FROM_1 + MONO_CHAIN + CLOSE,
  mono2: OPEN + IN_1_UNUSED + IN_2 + FROM_2 + MONO_CHAIN + CLOSE,
  monoSum: OPEN + IN_1 + IN_2 + FROM_1 + FROM_2 + MONO_CHAIN + CLOSE,
  stereo: OPEN + IN_1 + IN_2 + LANES + STEREO_CHAIN + CLOSE,
  dualMono: OPEN + IN_1 + IN_2 + LANES + DUAL_CHAINS + CLOSE,
  // One input and no choice of which: a DAW's mono track.
  mono: OPEN + IN_CENTRE + MONO_CHAIN + CLOSE,
};

export function inputModeIcon(name: InputModeIconName): string {
  return ICONS[name];
}
