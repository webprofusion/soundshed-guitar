const ICON_BASE = "/images/icons";
import { resolveEffectType } from "./effectGuids.js";
import { CATEGORIES, DEFAULT_ICON, EFFECTS } from "./generated/effectPresentation.js";

export type IconKey =
  | "amp"
  | "guitar"
  | "pedal"
  | "bolt"
  | "speaker"
  | "output"
  | "sliders"
  | "layout"
  | "wave"
  | "delay"
  | "doubler"
  | "reverb"
  | "reverb-advanced"
  | "reverb-ambient"
  | "wrench"
  | "door"
  | "meter"
  | "bulb"
  | "flame"
  | "blend"
  | "megaphone"
  | "split"
  | "parallel-split"
  | "parallel-join"
  | "mixer"
  | "note"
  | "gear"
  | "folder"
  | "flask"
  | "microscope"
  | "link"
  | "package"
  | "mute"
  | "close"
  | "plus"
  | "trash"
  | "star"
  | "heart"
  | "heart-filled"
  | "chevron-down"
  | "chevron-left"
  | "chevron-right"
  | "arrow-left"
  | "arrow-right"
  | "play"
  | "stop"
  | "check"
  | "x"
  | "settings";

export function renderIcon(icon: IconKey, className: string, title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<span class="${className}" style="--icon-url: url('${ICON_BASE}/${icon}.svg')" aria-hidden="true"${titleAttr}></span>`;
}

// Which icon each category and effect shows comes from core/ui/data/effect-presentation.json,
// shared with Soundshed Guitar Nano. Typing the icons as IconKey here is what makes the
// compiler catch an icon named in the table that renderIcon does not know.

export function getFxCategoryIcon(categoryId: string): string {
  const icon: IconKey = CATEGORIES[categoryId]?.icon ?? DEFAULT_ICON;
  return renderIcon(icon, "fx-category-icon");
}

export function getFxEffectIcon(effectType: string): string {
  const icon: IconKey = EFFECTS[resolveEffectType(effectType)]?.icon ?? DEFAULT_ICON;
  return renderIcon(icon, "fx-effect-icon");
}

export function getBadgeIcon(type: "resource" | "blend", titleOverride?: string): string {
  const icon = type === "resource" ? "folder" : "flask";
  const title = titleOverride ?? (type === "resource" ? "Requires resource" : "Custom blend");
  return renderIcon(icon, "fx-badge-icon", title);
}

/**
 * Inline SVG icon helpers for glyphs used in dynamic content
 */

export function getCheckmarkSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"${titleAttr}><polyline points="20 6 9 17 4 12"></polyline></svg>`;
}

export function getXSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"${titleAttr}><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>`;
}

export function getPlaySvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="currentColor"${titleAttr}><polygon points="5 3 19 12 5 21 5 3"></polygon></svg>`;
}

export function getStopSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="currentColor"${titleAttr}><rect x="4" y="4" width="16" height="16" rx="2"></rect></svg>`;
}

export function getXMarkSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"${titleAttr}><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>`;
}
