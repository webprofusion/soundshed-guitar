/**
 * Effect Layout Preferences
 *
 * The layout library stores at most one *default* layout per effect type. That is
 * not enough once several layouts exist for the same effect: a user may want the
 * Fender-style face whenever the loaded NAM model mentions "twin", the plain
 * standard controls for one particular preset, and a generic default everywhere
 * else.
 *
 * This module owns that preference layer. Rules are persisted in app settings
 * (`ui.effectLayoutPreferences`) so they survive restarts, and resolution is a
 * pure function of (rules, layout library, node match text, active preset).
 *
 * Resolution order, most specific first:
 *   1. preset rule      — pinned for one preset id
 *   2. keyword rule     — amp/fx make or model text matched against the node
 *   3. effect-type rule — the user's explicit "standard vs custom" choice
 *   4. layout library default for the effect type
 *   5. standard controls
 *
 * There is no global off switch: the header's split toggle picks the standard
 * controls per effect. (A master `ui.effectLayoutsEnabled` setting was added after
 * the 1.5.0 release and removed before any other; it is no longer read.)
 */

import { updateAppSetting } from "./appSettingsStore.js";
import { uiState } from "./state.js";
import { layoutLookupKey } from "./layoutTypes.js";
import type { EffectLayout, LayoutLibraryEntry } from "./layoutTypes.js";

/** App-settings key holding the serialized rule list. */
export const LAYOUT_PREFERENCES_SETTING = "ui.effectLayoutPreferences";

/** Sentinel layout id meaning "render the standard auto-generated controls". */
export const STANDARD_LAYOUT_ID = "__standard__";

/** Fired on window after rules change so open views can re-render. */
export const LAYOUT_PREFERENCES_CHANGED_EVENT = "effectLayoutPreferencesChanged";

/** What a rule is keyed on. */
export type LayoutPreferenceScope = "effectType" | "keyword" | "preset";

export interface LayoutPreferenceRule {
  id: string;
  /** Layout library lookup key: `effectType` or `effectType::blendId`. */
  lookupKey: string;
  scope: LayoutPreferenceScope;
  /** Target layout id, or STANDARD_LAYOUT_ID for the standard control panel. */
  layoutId: string;
  /** scope === "keyword": lower-cased make/model fragment to match. */
  keyword?: string;
  /** scope === "preset": the preset this rule is pinned to. */
  presetId?: string;
  /** scope === "preset": preset name captured for display in the rules list. */
  presetName?: string;
  createdAt?: string;
}

/** Where a resolved layout selection came from — drives the picker's UI hints. */
export type LayoutSelectionSource = "preset" | "keyword" | "effectType" | "libraryDefault" | "none";

export interface LayoutSelection {
  /** Resolved layout id, STANDARD_LAYOUT_ID, or null when nothing is defined. */
  layoutId: string | null;
  source: LayoutSelectionSource;
  /** The rule that decided this, when the source was a rule. */
  rule?: LayoutPreferenceRule;
}

export interface LayoutResolutionContext {
  effectType: string;
  blendId?: string;
  /** Lower-cased make/model text (display name, resource names) for keyword rules. */
  matchText?: string;
  /** Active preset id, for preset-scoped rules. */
  presetId?: string | null;
}

// ─────────────────────────────────────────────────────────────
// Rule storage
// ─────────────────────────────────────────────────────────────

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function parseRule(raw: unknown): LayoutPreferenceRule | null {
  if (!isRecord(raw)) return null;
  const lookupKey = typeof raw.lookupKey === "string" ? raw.lookupKey.trim() : "";
  const layoutId = typeof raw.layoutId === "string" ? raw.layoutId.trim() : "";
  const scope = raw.scope;
  if (!lookupKey || !layoutId) return null;
  if (scope !== "effectType" && scope !== "keyword" && scope !== "preset") return null;

  const keyword = typeof raw.keyword === "string" ? raw.keyword.trim().toLowerCase() : "";
  const presetId = typeof raw.presetId === "string" ? raw.presetId.trim() : "";
  if (scope === "keyword" && !keyword) return null;
  if (scope === "preset" && !presetId) return null;

  return {
    id: typeof raw.id === "string" && raw.id ? raw.id : generateRuleId(),
    lookupKey,
    scope,
    layoutId,
    keyword: scope === "keyword" ? keyword : undefined,
    presetId: scope === "preset" ? presetId : undefined,
    presetName: typeof raw.presetName === "string" ? raw.presetName : undefined,
    createdAt: typeof raw.createdAt === "string" ? raw.createdAt : undefined,
  };
}

function generateRuleId(): string {
  return `layoutpref-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}

/** All persisted rules, ignoring any malformed entries. */
export function getLayoutPreferenceRules(): LayoutPreferenceRule[] {
  const raw = uiState.appSettings?.[LAYOUT_PREFERENCES_SETTING];
  if (!Array.isArray(raw)) return [];
  return raw.map(parseRule).filter((rule): rule is LayoutPreferenceRule => rule !== null);
}

function persistRules(rules: LayoutPreferenceRule[]): void {
  const serialized = rules.map((rule) => {
    const entry: Record<string, string> = {
      id: rule.id,
      lookupKey: rule.lookupKey,
      scope: rule.scope,
      layoutId: rule.layoutId,
    };
    if (rule.keyword) entry.keyword = rule.keyword;
    if (rule.presetId) entry.presetId = rule.presetId;
    if (rule.presetName) entry.presetName = rule.presetName;
    entry.createdAt = rule.createdAt ?? new Date().toISOString();
    return entry;
  });
  updateAppSetting(LAYOUT_PREFERENCES_SETTING, serialized);
  window.dispatchEvent(new CustomEvent(LAYOUT_PREFERENCES_CHANGED_EVENT));
}

/** Rules that apply to any of the given lookup keys, most recent last. */
export function getLayoutPreferenceRulesForKeys(lookupKeys: readonly string[]): LayoutPreferenceRule[] {
  const keys = new Set(lookupKeys);
  return getLayoutPreferenceRules().filter((rule) => keys.has(rule.lookupKey));
}

/**
 * Adds or replaces a rule. A given (lookupKey, scope, keyword/presetId) tuple can
 * only hold one target layout, so re-applying to the same scope updates in place.
 */
export function setLayoutPreference(input: Omit<LayoutPreferenceRule, "id" | "createdAt">): LayoutPreferenceRule {
  const keyword = input.scope === "keyword" ? (input.keyword ?? "").trim().toLowerCase() : undefined;
  const presetId = input.scope === "preset" ? input.presetId : undefined;
  const rule: LayoutPreferenceRule = {
    ...input,
    keyword,
    presetId,
    id: generateRuleId(),
    createdAt: new Date().toISOString(),
  };

  const rules = getLayoutPreferenceRules().filter((existing) => !(
    existing.lookupKey === rule.lookupKey
    && existing.scope === rule.scope
    && (existing.keyword ?? "") === (rule.keyword ?? "")
    && (existing.presetId ?? "") === (rule.presetId ?? "")
  ));
  rules.push(rule);
  persistRules(rules);
  return rule;
}

/** Removes a single rule by id. No-op when the id is unknown. */
export function removeLayoutPreference(ruleId: string): void {
  const rules = getLayoutPreferenceRules();
  const next = rules.filter((rule) => rule.id !== ruleId);
  if (next.length === rules.length) return;
  persistRules(next);
}

/** Removes every rule for the given lookup keys (the picker's "reset" action). */
export function clearLayoutPreferencesForKeys(lookupKeys: readonly string[]): void {
  const keys = new Set(lookupKeys);
  const rules = getLayoutPreferenceRules();
  const next = rules.filter((rule) => !keys.has(rule.lookupKey));
  if (next.length === rules.length) return;
  persistRules(next);
}

// ─────────────────────────────────────────────────────────────
// Library lookup
// ─────────────────────────────────────────────────────────────

/**
 * Lookup keys for a node, most specific first. Blend effects can have a layout
 * for the specific blend and a generic one for the effect type.
 */
export function layoutLookupKeysFor(effectType: string, blendId?: string): string[] {
  const keys = [layoutLookupKey(effectType, blendId || undefined)];
  const base = layoutLookupKey(effectType);
  if (!keys.includes(base)) {
    keys.push(base);
  }
  return keys;
}

/** Every layout available to a node, de-duplicated across its lookup keys. */
export function getAvailableLayoutEntries(effectType: string, blendId?: string): LayoutLibraryEntry[] {
  const library = uiState.layoutLibrary;
  if (!library) return [];
  const seen = new Set<string>();
  const entries: LayoutLibraryEntry[] = [];
  for (const key of layoutLookupKeysFor(effectType, blendId)) {
    for (const entry of library.byEffectType[key] ?? []) {
      if (!entry?.layoutId || seen.has(entry.layoutId)) continue;
      seen.add(entry.layoutId);
      entries.push(entry);
    }
  }
  return entries;
}

/** Finds a layout by id across a node's lookup keys. */
export function findLayoutById(layoutId: string, effectType: string, blendId?: string): EffectLayout | null {
  if (!layoutId || layoutId === STANDARD_LAYOUT_ID) return null;
  const entry = getAvailableLayoutEntries(effectType, blendId).find((e) => e.layoutId === layoutId);
  return entry?.layout ?? null;
}

// ─────────────────────────────────────────────────────────────
// Resolution
// ─────────────────────────────────────────────────────────────

function findKeywordRule(rules: readonly LayoutPreferenceRule[], matchText: string): LayoutPreferenceRule | undefined {
  if (!matchText) return undefined;
  // Longest keyword wins so "vox ac30" beats a broader "vox" rule.
  let best: LayoutPreferenceRule | undefined;
  for (const rule of rules) {
    const keyword = rule.keyword;
    if (!keyword || !matchText.includes(keyword)) continue;
    if (!best || keyword.length > (best.keyword?.length ?? 0)) {
      best = rule;
    }
  }
  return best;
}

/**
 * Resolves which layout (or the standard controls) a node should render.
 * Returns `layoutId: null` when nothing is configured at all.
 */
export function resolveLayoutSelection(context: LayoutResolutionContext): LayoutSelection {
  const keys = layoutLookupKeysFor(context.effectType, context.blendId);
  const matchText = (context.matchText ?? "").toLowerCase();
  const presetId = context.presetId ?? "";
  const allRules = getLayoutPreferenceRules();

  for (const key of keys) {
    const rules = allRules.filter((rule) => rule.lookupKey === key);
    if (!rules.length) continue;

    if (presetId) {
      const presetRule = rules.find((rule) => rule.scope === "preset" && rule.presetId === presetId);
      if (presetRule) {
        return { layoutId: presetRule.layoutId, source: "preset", rule: presetRule };
      }
    }

    const keywordRule = findKeywordRule(rules.filter((rule) => rule.scope === "keyword"), matchText);
    if (keywordRule) {
      return { layoutId: keywordRule.layoutId, source: "keyword", rule: keywordRule };
    }

    const typeRule = rules.find((rule) => rule.scope === "effectType");
    if (typeRule) {
      return { layoutId: typeRule.layoutId, source: "effectType", rule: typeRule };
    }
  }

  // No rules — fall back to the layout library's own default for this effect.
  const library = uiState.layoutLibrary;
  if (library) {
    for (const key of keys) {
      const defaultId = library.defaults[key];
      if (!defaultId) continue;
      const exists = (library.byEffectType[key] ?? []).some((entry) => entry.layoutId === defaultId);
      if (exists) {
        return { layoutId: defaultId, source: "libraryDefault" };
      }
    }
  }

  return { layoutId: null, source: "none" };
}

/**
 * Switches a node that is showing a custom layout to the standard controls, for the
 * header's one-click toggle. The choice is recorded at the scope that picked the custom
 * layout (the same preset or keyword rule, else the effect type), since a broader rule
 * would lose to the one that decided it. Returns false when the node already shows the
 * standard controls.
 */
export function selectStandardControls(context: LayoutResolutionContext): boolean {
  if (!resolveLayoutForNode(context)) {
    return false;
  }
  const decidingRule = resolveLayoutSelection(context).rule;
  setLayoutPreference({
    // Blend-specific layouts are keyed on the blend, like the picker's Apply.
    lookupKey: layoutLookupKeysFor(context.effectType, context.blendId)[0],
    scope: decidingRule?.scope ?? "effectType",
    layoutId: STANDARD_LAYOUT_ID,
    keyword: decidingRule?.keyword,
    presetId: decidingRule?.presetId,
    presetName: decidingRule?.presetName,
  });
  return true;
}

/**
 * The layout a node should render, honouring preferences. Returns null when the
 * standard auto-generated controls should be used.
 */
export function resolveLayoutForNode(context: LayoutResolutionContext): EffectLayout | null {
  const selection = resolveLayoutSelection(context);
  if (!selection.layoutId || selection.layoutId === STANDARD_LAYOUT_ID) {
    return null;
  }
  return findLayoutById(selection.layoutId, context.effectType, context.blendId);
}

// ─────────────────────────────────────────────────────────────
// Keyword helpers
// ─────────────────────────────────────────────────────────────

const KEYWORD_STOP_WORDS = new Set([
  "the", "and", "amp", "amplifier", "model", "nam", "ir", "cab", "cabinet",
  "wav", "mix", "channel", "clean", "lead", "rhythm", "high", "low", "gain",
]);

/** Normalizes free text into the lower-cased haystack used for keyword matching. */
export function buildLayoutMatchText(parts: ReadonlyArray<string | undefined | null>): string {
  return parts
    .filter((part): part is string => typeof part === "string" && part.trim().length > 0)
    .join(" ")
    .toLowerCase();
}

/**
 * Suggests make/model keywords from a node's text so the picker can prefill the
 * keyword field instead of making the user guess what will match.
 */
export function suggestLayoutKeywords(matchText: string, limit = 6): string[] {
  const tokens = matchText
    .toLowerCase()
    .split(/[^a-z0-9+]+/)
    .map((token) => token.trim())
    .filter((token) => token.length >= 3 && !KEYWORD_STOP_WORDS.has(token) && !/^\d+$/.test(token));
  const unique: string[] = [];
  for (const token of tokens) {
    if (!unique.includes(token)) {
      unique.push(token);
    }
    if (unique.length >= limit) break;
  }
  return unique;
}
