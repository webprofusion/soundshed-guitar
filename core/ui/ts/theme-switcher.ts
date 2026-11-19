/**
 * Theme Switcher
 * Puts the chosen theme's classes on <body> and stores the choice. The themes themselves are
 * listed in ./themes.ts.
 */

import { DEFAULT_THEME, LEGACY_THEME_NAMES, THEMES, type ThemeName } from './themes.js';

export type { ThemeName } from './themes.js';

/** Every theme, in the order the pickers list them, with the name each one shows. */
export const THEME_OPTIONS: ReadonlyArray<{ value: ThemeName; label: string }> = THEMES.map((theme) => ({
  value: theme.id,
  label: theme.label,
}));

const THEME_LIST: ThemeName[] = THEMES.map((theme) => theme.id);

/**
 * The body classes a theme switches on. A theme built on another keeps that one's class (Stage and
 * High Gain keep `theme-dark`, the studio themes `theme-light`), so its rules still apply, and adds
 * its own for its stylesheets to build on.
 */
const THEME_BODY_CLASSES = Object.fromEntries(THEMES.map((theme) => [theme.id, [...theme.classes]])) as Record<
  ThemeName,
  string[]
>;

const ALL_THEME_BODY_CLASSES = [...new Set(Object.values(THEME_BODY_CLASSES).flat())];

/** A stored or received theme name as one this build knows; anything else is dark. */
export function normalizeThemeName(theme: unknown): ThemeName {
  if (typeof theme !== 'string') {
    return DEFAULT_THEME;
  }
  if (THEME_LIST.includes(theme as ThemeName)) {
    return theme as ThemeName;
  }
  return LEGACY_THEME_NAMES[theme] ?? DEFAULT_THEME;
}

export class ThemeSwitcher {
  private currentTheme: ThemeName = DEFAULT_THEME;
  private body: HTMLElement;

  constructor() {
    this.body = document.body;
    this.setTheme(this.currentTheme, false);
  }

  /**
   * Set the active theme
   */
  setTheme(theme: ThemeName, persist: boolean = true): void {
    const next = normalizeThemeName(theme);

    // Remove all theme classes
    this.body.classList.remove(...ALL_THEME_BODY_CLASSES);

    // Add new theme class
    this.body.classList.add(...THEME_BODY_CLASSES[next]);

    this.currentTheme = next;

    if (persist) {
      // Persist via backend
      import('./bridge.js').then(({ postMessage }) => {
        postMessage({ type: 'setTheme', theme: next });
      }).catch(() => {});
    }

    // Dispatch event for other components to react
    window.dispatchEvent(new CustomEvent('themeChanged', { detail: { theme: next } }));
  }

  applyTheme(theme: ThemeName): void {
    this.setTheme(theme, false);
  }

  /**
   * Get the current theme
   */
  getCurrentTheme(): ThemeName {
    return this.currentTheme;
  }

  /**
   * Cycle to the next theme
   */
  cycleTheme(): void {
    const currentIndex = THEME_LIST.indexOf(this.currentTheme);
    const nextIndex = (currentIndex + 1) % THEME_LIST.length;
    this.setTheme(THEME_LIST[nextIndex]);
  }

  /**
   * Get theme display name
   */
  getThemeDisplayName(theme?: ThemeName): string {
    const t = theme || this.currentTheme;
    return THEME_OPTIONS.find((option) => option.value === t)?.label ?? t;
  }
}

// Export singleton instance
export const themeSwitcher = new ThemeSwitcher();
