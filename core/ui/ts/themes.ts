/**
 * The themes: the one list every part of the app reads.
 *
 * - ts/theme-switcher.ts takes the names, the pickers' order and labels, and the body classes.
 * - scripts/assemble-html.js writes each theme's stylesheets into index.html, at the template's
 *   <!--#theme-palettes--> (themes that are their own palette) and <!--#theme-variants--> (themes
 *   built on one) markers.
 * - tools/gen-native-theme.mjs resolves each theme's colours for the native UI (Nano Q) and writes
 *   this list, with those colours, into core/src/uiclient/ThemeTokens.generated.h.
 *
 * Plain data, with no imports, so Node reads this file as it is (its type annotations are stripped).
 * Adding a theme: an entry here, its sheets under css/themes/, its flyout picture
 * (tools/theme-thumbnails/capture.mjs), then `npm run build` and `node tools/gen-native-theme.mjs`.
 */

export type ThemeScheme = 'light' | 'dark';

export interface ThemeDefinition {
  /** The stored name (the shared "theme" app setting). */
  id: string;
  /** The name the pickers show. */
  label: string;
  /**
   * The palette the theme builds on: itself for a palette, else the theme whose class it keeps on
   * the body, so that theme's rules keep applying under it.
   */
  base: string;
  /** Light or dark: the native UI's light/dark choices, and the ground a picture fades toward. */
  scheme: ThemeScheme;
  /** The classes it puts on <body>, the base's first. */
  classes: readonly string[];
  /**
   * Its palette, under css/: one sheet of nothing but the theme's colour tokens, set on its class.
   * The native UI (Nano Q) reads it too.
   */
  palette: string;
  /**
   * Its skin, under css/, loaded after the palette: everything else in its look (component rules,
   * pictures, textures, type and shape), which only the web UI draws.
   */
  skin: readonly string[];
  /** Type faces the theme uses in place of Inter, by family name (css/fonts/). */
  fonts?: { readonly ui?: string; readonly title?: string };
}

export const THEMES = [
  {
    id: 'light',
    label: 'Light',
    base: 'light',
    scheme: 'light',
    classes: ['theme-light'],
    palette: 'themes/light.css',
    skin: ['themes/light-skin.css'],
  },
  {
    id: 'dark',
    label: 'Dark',
    base: 'dark',
    scheme: 'dark',
    classes: ['theme-dark'],
    palette: 'themes/dark.css',
    skin: ['themes/dark-skin.css'],
  },
  {
    id: 'classic',
    label: 'Vintage',
    base: 'classic',
    scheme: 'dark',
    classes: ['theme-classic'],
    palette: 'themes/classic.css',
    skin: ['themes/classic-skin.css'],
  },
  {
    id: 'stage',
    label: 'Stage',
    base: 'dark',
    scheme: 'dark',
    classes: ['theme-dark', 'theme-stage'],
    palette: 'themes/stage.css',
    skin: ['themes/stage-skin.css'],
    fonts: { ui: 'Archivo' },
  },
  {
    id: 'high-gain',
    label: 'High Gain',
    base: 'dark',
    scheme: 'dark',
    classes: ['theme-dark', 'theme-high-gain'],
    palette: 'themes/high-gain.css',
    skin: ['themes/high-gain-skin.css'],
    fonts: { ui: 'Oswald', title: 'Grenze Gotisch' },
  },
  {
    id: 'studio',
    label: 'Studio',
    base: 'light',
    scheme: 'light',
    classes: ['theme-light', 'theme-studio'],
    palette: 'themes/studio.css',
    skin: ['themes/studio-skin.css'],
  },
  {
    id: 'studio-2003',
    label: 'Studio 2003',
    base: 'light',
    scheme: 'light',
    classes: ['theme-light', 'theme-studio-2003'],
    palette: 'themes/studio-2003.css',
    skin: ['themes/studio-2003-skin.css'],
  },
] as const satisfies readonly ThemeDefinition[];

/** A theme's stored name. */
export type ThemeName = (typeof THEMES)[number]['id'];

/** What an unknown name becomes. */
export const DEFAULT_THEME: ThemeName = 'dark';

/** Names earlier builds stored, and the theme each is now. */
export const LEGACY_THEME_NAMES: Readonly<Record<string, ThemeName>> = {
  default: 'dark',
  gritty: 'dark',
};
