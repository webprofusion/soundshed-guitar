/**
 * Theme Switcher
 * Manages switching between the light, dark, vintage and studio themes
 */

export type ThemeName = 'light' | 'dark' | 'classic' | 'studio';

/** Every theme, in the order the pickers list them, with the name each one shows. */
export const THEME_OPTIONS: ReadonlyArray<{ value: ThemeName; label: string }> = [
  { value: 'light', label: 'Light' },
  { value: 'dark', label: 'Dark' },
  { value: 'classic', label: 'Vintage' },
  { value: 'studio', label: 'Studio' },
];

const THEME_LIST: ThemeName[] = THEME_OPTIONS.map((option) => option.value);
const LEGACY_THEME_MAP: Record<string, ThemeName> = {
  default: 'dark',
  gritty: 'dark',
};

/**
 * The body classes a theme switches on. Studio is the light theme drawn as hardware: it
 * keeps `theme-light`, so the light rules across the stylesheets still apply, and adds its
 * own class for css/themes/studio.css to build on.
 */
const THEME_BODY_CLASSES: Record<ThemeName, string[]> = {
  light: ['theme-light'],
  dark: ['theme-dark'],
  classic: ['theme-classic'],
  studio: ['theme-light', 'theme-studio'],
};

const ALL_THEME_BODY_CLASSES = [...new Set(Object.values(THEME_BODY_CLASSES).flat())];

/** A stored or received theme name as one this build knows; anything else is dark. */
export function normalizeThemeName(theme: unknown): ThemeName {
  if (typeof theme !== 'string') {
    return 'dark';
  }
  if (THEME_LIST.includes(theme as ThemeName)) {
    return theme as ThemeName;
  }
  return LEGACY_THEME_MAP[theme] ?? 'dark';
}

export class ThemeSwitcher {
  private currentTheme: ThemeName = 'dark';
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
