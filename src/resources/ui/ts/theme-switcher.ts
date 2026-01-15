/**
 * Theme Switcher
 * Manages switching between light, dark, and classic themes
 */

export type ThemeName = 'default' | 'light' | 'dark' | 'classic' | 'gritty';

export class ThemeSwitcher {
  private currentTheme: ThemeName = 'default';
  private body: HTMLElement;

  constructor() {
    this.body = document.body;
    // Load saved theme from localStorage
    const saved = localStorage.getItem('guitarfx-theme') as ThemeName;
    if (saved) {
      this.setTheme(saved);
    }
  }

  /**
   * Set the active theme
   */
  setTheme(theme: ThemeName): void {
    // Remove all theme classes
    this.body.classList.remove('theme-light', 'theme-dark', 'theme-classic', 'theme-gritty');
    
    // Add new theme class (default has no class)
    if (theme !== 'default') {
      this.body.classList.add(`theme-${theme}`);
    }
    
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
    const themes: ThemeName[] = ['default', 'light', 'dark', 'classic', 'gritty'];
    const currentIndex = themes.indexOf(this.currentTheme);
    const nextIndex = (currentIndex + 1) % themes.length;
    this.setTheme(themes[nextIndex]);
  }

  /**
   * Get theme display name
   */
  getThemeDisplayName(theme?: ThemeName): string {
    const t = theme || this.currentTheme;
    const names: Record<ThemeName, string> = {
      default: 'Default',
      light: 'Light',
      dark: 'Dark',
      classic: 'Classic 70s',
      gritty: 'Worn Pedal'
    };
    return names[t];
  }
}

// Export singleton instance
export const themeSwitcher = new ThemeSwitcher();
