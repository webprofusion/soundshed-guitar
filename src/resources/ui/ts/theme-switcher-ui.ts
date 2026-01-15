/**
 * Theme Switcher UI Component
 * Adds a theme selector dropdown to the control bar
 */

import { themeSwitcher, type ThemeName } from './theme-switcher.js';

/**
 * Initialize the theme switcher UI
 * Adds a theme selector to the control bar
 */
export function initializeThemeSwitcher(): void {
  const controlBar = document.querySelector('.control-bar');
  if (!controlBar) {
    console.warn('[ThemeSwitcher] Control bar not found');
    return;
  }

  // Create theme selector group
  const themeGroup = document.createElement('div');
  themeGroup.className = 'control-bar-group theme-selector-group';
  
  const label = document.createElement('span');
  label.className = 'control-label';
  label.textContent = 'Theme:';
  
  const select = document.createElement('select');
  select.className = 'theme-selector';
  select.id = 'theme-selector';
  
  // Add theme options
  const themes: Array<{ value: ThemeName; label: string }> = [
    { value: 'default', label: 'Default' },
    { value: 'light', label: 'Light' },
    { value: 'dark', label: 'Dark' },
    { value: 'classic', label: 'Classic 70s' },
    { value: 'gritty', label: 'Worn Pedal' }
  ];
  
  themes.forEach(theme => {
    const option = document.createElement('option');
    option.value = theme.value;
    option.textContent = theme.label;
    if (theme.value === themeSwitcher.getCurrentTheme()) {
      option.selected = true;
    }
    select.appendChild(option);
  });
  
  // Handle theme changes
  select.addEventListener('change', (e) => {
    const theme = (e.target as HTMLSelectElement).value as ThemeName;
    themeSwitcher.setTheme(theme);
    console.log('[ThemeSwitcher] Theme changed to:', theme);
  });
  
  // Listen for theme changes from other sources
  window.addEventListener('themeChanged', ((e: CustomEvent) => {
    select.value = e.detail.theme;
  }) as EventListener);
  
  themeGroup.appendChild(label);
  themeGroup.appendChild(select);
  
  // Add to control bar (at the end)
  controlBar.appendChild(themeGroup);
  
  console.log('[ThemeSwitcher] UI initialized');
}

/**
 * Alternative: Add theme switcher as icon buttons
 */
export function initializeThemeSwitcherIcons(): void {
  const iconBar = document.querySelector('.icon-bar');
  if (!iconBar) {
    console.warn('[ThemeSwitcher] Icon bar not found');
    return;
  }

  const button = document.createElement('button');
  button.className = 'icon-btn theme-cycle-btn';
  button.title = 'Change Theme';
  button.innerHTML = '🎨';
  
  button.addEventListener('click', () => {
    themeSwitcher.cycleTheme();
    const theme = themeSwitcher.getCurrentTheme();
    button.title = `Theme: ${themeSwitcher.getThemeDisplayName()}`;
    console.log('[ThemeSwitcher] Cycled to:', theme);
  });
  
  iconBar.appendChild(button);
  
  console.log('[ThemeSwitcher] Icon button initialized');
}
