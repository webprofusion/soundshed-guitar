#!/usr/bin/env node
/**
 * The web UI's theme colours, for Soundshed Nano Q's native UI.
 *
 * Soundshed Guitar draws its themes from CSS custom properties: the defaults on :root in
 * core/ui/css/variables.css, each theme's palette sheet (core/ui/css/themes/<theme>.css, on
 * top of its base palette for a variant such as Stage), and component tokens that fall back to
 * their component sheet's base look. Nano Q cannot read CSS, so this resolves the tokens it uses
 * for every theme as the browser does (tools/theme-css.mjs: the cascade, var() and its
 * fallbacks, color-mix(), a gradient's first stop) and writes them as ARGB constants to
 * core/src/uiclient/ThemeTokens.generated.h, which Nano Q draws with. It also writes the list
 * of themes there, from core/ui/ts/themes.ts: every theme's name, label, light or dark, colours
 * and type faces.
 * Both products then change colour together, and the theme a user picks in either applies
 * to both (it is the shared "theme" app setting).
 *
 *   node tools/gen-native-theme.mjs          # regenerate
 *   node tools/gen-native-theme.mjs --check  # fail if the header is out of date (CI)
 */

import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { linkedSheets, loadThemeCss, parseColour, toArgb } from './theme-css.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const UI = path.join(ROOT, 'core', 'ui');
const OUT = path.join(ROOT, 'core', 'src', 'uiclient', 'ThemeTokens.generated.h');
const require = createRequire(import.meta.url);
const MANIFEST = require(path.join(UI, 'scripts', 'themes-manifest.js')).loadThemes();
const { applyThemeStylesheets } = require(path.join(UI, 'scripts', 'assemble-html.js'));
const CSS = loadThemeCss(path.join(UI, 'css'), linkedSheets(UI, applyThemeStylesheets), MANIFEST.THEMES);
/** The face a theme's text is set in when its manifest entry names none (core/ui/css/base.css). */
const DEFAULT_FONT = 'Inter';

// C++ field -> the CSS custom properties it reads, first one defined wins.
const TOKENS = [
  ['accent', ['--color-accent']],
  ['accentHover', ['--color-accent-hover']],
  ['accentDark', ['--color-accent-dark']],
  ['success', ['--color-success']],
  ['info', ['--color-info']],
  ['error', ['--color-error']],
  ['warning', ['--color-warning']],
  ['bgPrimary', ['--bg-primary']],
  ['bgSecondary', ['--bg-secondary']],
  ['bgTertiary', ['--bg-tertiary']],
  ['bgSurface', ['--bg-surface']],
  ['bgElevated', ['--bg-elevated']],
  ['bgPanel', ['--bg-panel']],
  ['bgCard', ['--bg-card']],
  ['bgInput', ['--bg-input']],
  ['bgHeader', ['--bg-header-dark']],
  ['bgFooter', ['--bg-footer']],
  ['textPrimary', ['--text-primary']],
  ['textSecondary', ['--text-secondary']],
  ['textTertiary', ['--text-tertiary']],
  ['textMuted', ['--text-muted']],
  ['textOnDark', ['--text-dark-primary']],
  ['borderLight', ['--border-light']],
  ['borderMedium', ['--border-medium']],
  ['borderStrong', ['--border-strong']],
  ['controlBg', ['--control-bg']],
  ['controlKnobBg', ['--control-knob-bg']],
  ['controlBorder', ['--control-border']],
  ['controlIndicator', ['--control-indicator']],
  ['knobRingStart', ['--knob-ring-start', '--knob-ring-color']],
  ['knobRingEnd', ['--knob-ring-end', '--knob-ring-color']],
  ['knobRingTrack', ['--knob-ring-track']],
  ['toggleBg', ['--toggle-bg']],
  ['eqCurveGrid', ['--eq-curve-grid']],
  ['eqCurveResponse', ['--eq-curve-response']],
  ['eqCurveHandleStroke', ['--eq-curve-handle-stroke']],
  ['eqCurveSpectrumFillTop', ['--eq-curve-spectrum-fill-top']],
  ['eqCurveSpectrumFillBottom', ['--eq-curve-spectrum-fill-bottom']],
  ['eqCurveSpectrumLine', ['--eq-curve-spectrum-line']],
  ['eqCurveTooltipBg', ['--eq-curve-tooltip-bg']],
  ['eqCurveTooltipText', ['--eq-curve-tooltip-text']],
  ['overlayLight', ['--overlay-light']],
  ['modalBg', ['--modal-bg']],
  ['modalBgRaised', ['--modal-bg-raised']],
  ['modalBorder', ['--modal-border']],
  ['modalBorderMedium', ['--modal-border-medium']],
  ['modalText', ['--modal-text']],
  ['modalTextSecondary', ['--modal-text-secondary']],
  ['modalTextMuted', ['--modal-text-muted']],
  // Parts Nano Q draws as the web UI does (component tokens and the parts' own properties).
  ['buttonBg', ['--button-bg']],
  ['buttonBorder', ['--button-border']],
  ['effectShellTitleInk', ['--effect-shell-title-ink']],
  ['effectShellValueInk', ['--effect-shell-value-ink']],
  ['presetFavorite', ['--color-preset-favorite']],
  // The Scale Guide's fretboard and diagrams (core/ui/css/scale-guide.css).
  ['sgSurface', ['--sg-surface']],
  ['sgFret', ['--sg-fret']],
  ['sgNut', ['--sg-nut']],
  ['sgCapo', ['--sg-capo']],
  ['sgOpenSlot', ['--sg-open-slot']],
  ['sgDotFill', ['--sg-dot-fill']],
  ['sgDotEdge', ['--sg-dot-edge']],
  ['sgDotText', ['--sg-dot-text']],
  ['sgDimFill', ['--sg-dim-fill']],
  ['sgDimText', ['--sg-dim-text']],
  ['sgBoxLine', ['--sg-box-line']],
  ['sgBoxDot', ['--sg-box-dot']],
  ['sgBoxText', ['--sg-box-text']],
];

for (const [field] of TOKENS) {
  if (TOKENS.filter(([other]) => other === field).length > 1) throw new Error(`TOKENS lists ${field} twice`);
}

/** The C++ name of a palette's tokens: kLight for "light". */
const paletteName = (id) => `k${id.replace(/(^|-)(\w)/g, (_, __, c) => c.toUpperCase())}`;

const cppString = (text) => JSON.stringify(text);

/** A token's colour under a theme, as 0xAARRGGBB. */
function tokenArgb(theme, props) {
  for (const prop of props) {
    const value = CSS.value(theme.id, prop);
    if (value === undefined) continue;
    try {
      return toArgb(parseColour(value));
    } catch (err) {
      throw new Error(`${theme.id}: ${prop}: ${err.message}`);
    }
  }
  throw new Error(`${theme.id}: none of ${props.join(', ')} defined`);
}

function generate() {
  const declWidth = Math.max(...TOKENS.map(([field]) => `std::uint32_t ${field};`.length));
  const lines = [
    '// Generated by tools/gen-native-theme.mjs from core/ui/css and core/ui/ts/themes.ts - do not edit by hand.',
    '// Regenerate with: node tools/gen-native-theme.mjs',
    '',
    '#pragma once',
    '',
    '#include <cstdint>',
    '#include <string_view>',
    '',
    'namespace guitarfx::uiclient::theme',
    '{',
    '/// One theme\'s colours as 0xAARRGGBB, named after the web UI\'s CSS custom properties.',
    'struct ThemeTokens',
    '{',
    // Aligned as the C++ formatter aligns trailing comments, so a formatted header still passes --check.
    ...TOKENS.map(([field, props]) => `    ${`std::uint32_t ${field};`.padEnd(declWidth)} // ${props[0]}`),
    '};',
    '',
  ];
  for (const theme of MANIFEST.THEMES) {
    const values = TOKENS.map(([, props]) => `0x${tokenArgb(theme, props).toString(16).padStart(8, '0')}`);
    lines.push(`inline constexpr ThemeTokens ${paletteName(theme.id)}{`);
    TOKENS.forEach(([field], i) => lines.push(`    ${values[i]}, // ${field}`));
    lines.push('};', '');
  }
  lines.push(
    '/// A theme of the web UI (core/ui/ts/themes.ts): its stored name, the name pickers show, whether',
    '/// it is dark, its colours, and the faces its text and its titles are set in.',
    'struct ThemeInfo',
    '{',
    '    std::string_view id;',
    '    std::string_view label;',
    '    bool dark;',
    '    const ThemeTokens* tokens;',
    '    std::string_view uiFont;',
    '    std::string_view titleFont;',
    '};',
    '',
    '/// Every theme, in the order the pickers list them.',
    'inline constexpr ThemeInfo kThemes[] = {',
    ...MANIFEST.THEMES.map(
      (theme) =>
        `    {${cppString(theme.id)}, ${cppString(theme.label)}, ${theme.scheme === 'dark'}, &${paletteName(theme.id)}, ` +
        `${cppString(theme.fonts?.ui ?? DEFAULT_FONT)}, ${cppString(theme.fonts?.title ?? theme.fonts?.ui ?? DEFAULT_FONT)}},`,
    ),
    '};',
    '',
    '/// The theme stored as `name`. A name earlier builds stored is the theme it became; any other is',
    `/// ${MANIFEST.DEFAULT_THEME}, as the web UI normalises it (theme-switcher.ts).`,
    'inline const ThemeInfo& FindTheme(std::string_view name) noexcept',
    '{',
    ...Object.entries(MANIFEST.LEGACY_THEME_NAMES).flatMap(([legacy, theme]) => [
      `    if (name == ${cppString(legacy)})`,
      '    {',
      `        name = ${cppString(theme)};`,
      '    }',
      '',
    ]),
    '    for (const auto& theme : kThemes)',
    '    {',
    '        if (theme.id == name)',
    '        {',
    '            return theme;',
    '        }',
    '    }',
    '',
    '    for (const auto& theme : kThemes)',
    '    {',
    `        if (theme.id == ${cppString(MANIFEST.DEFAULT_THEME)})`,
    '        {',
    '            return theme;',
    '        }',
    '    }',
    '',
    '    return kThemes[0];',
    '}',
    '',
  );
  lines.pop(); // the formatter keeps no blank line before the namespace closes
  lines.push('} // namespace guitarfx::uiclient::theme', '');
  return lines.join('\n');
}

const text = generate();
if (process.argv.includes('--check')) {
  const current = fs.existsSync(OUT) ? fs.readFileSync(OUT, 'utf8').replace(/\r\n/g, '\n') : '';
  if (current !== text) {
    console.error('[gen-native-theme] ThemeTokens.generated.h is out of date with core/ui/css or core/ui/ts/themes.ts. Run: node tools/gen-native-theme.mjs');
    process.exitCode = 1;
  } else {
    console.log('[gen-native-theme] OK - native theme tokens match the CSS and the theme list.');
  }
} else {
  fs.mkdirSync(path.dirname(OUT), { recursive: true });
  fs.writeFileSync(OUT, text, 'utf8');
  console.log(`[gen-native-theme] wrote ${path.relative(ROOT, OUT)}`);
}
