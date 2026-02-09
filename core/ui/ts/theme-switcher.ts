/**
 * Theme Switcher
 * Manages switching between light, dark, and classic themes
 */

export type ThemeName = 'light' | 'dark' | 'classic';

const THEME_LIST: ThemeName[] = ['light', 'dark', 'classic'];
const LEGACY_THEME_MAP: Record<string, ThemeName> = {
  default: 'dark',
  gritty: 'dark',
};

export class ThemeSwitcher {
  private currentTheme: ThemeName = 'dark';
  private body: HTMLElement;

  constructor() {
    this.body = document.body;
    // Load saved theme from localStorage
    const saved = localStorage.getItem('guitarfx-theme');
    if (saved) {
      this.setTheme(this.normalizeTheme(saved));
    } else {
      this.setTheme(this.currentTheme);
    }
  }

  private normalizeTheme(theme: string): ThemeName {
    if (THEME_LIST.includes(theme as ThemeName)) {
      return theme as ThemeName;
    }
    return LEGACY_THEME_MAP[theme] ?? 'dark';
  }

  /**
   * Set the active theme
   */
  setTheme(theme: ThemeName): void {
    // Remove all theme classes
    this.body.classList.remove('theme-light', 'theme-dark', 'theme-classic');
    
    // Add new theme class
    this.body.classList.add(`theme-${theme}`);
    
    this.currentTheme = theme;
    
    // Save to localStorage
    localStorage.setItem('guitarfx-theme', theme);
    
    // Dispatch event for other components to react
    window.dispatchEvent(new CustomEvent('themeChanged', { detail: { theme } }));
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
    const names: Record<ThemeName, string> = {
      light: 'Light',
      dark: 'Dark',
      classic: '70s'
    };
    return names[t];
  }
}

// Export singleton instance
export const themeSwitcher = new ThemeSwitcher();
