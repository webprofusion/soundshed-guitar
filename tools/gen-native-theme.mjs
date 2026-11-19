#!/usr/bin/env node
/**
 * The web UI's theme colours, for Soundshed Nano Q's native UI.
 *
 * Soundshed Guitar draws its themes from CSS custom properties: the defaults on :root in
 * core/ui/css/variables.css, overridden per palette (light, dark, classic) in
 * core/ui/css/themes/<theme>.css. Nano Q cannot read CSS, so this resolves the tokens it uses
 * for each palette - following var() references, and taking a gradient's first stop - and
 * writes them as ARGB constants to core/src/uiclient/ThemeTokens.generated.h, which
 * Nano Q draws with. It also writes the list of themes there, from core/ui/ts/themes.ts, so
 * the native code knows every theme's name, label and light or dark; a theme built on a
 * palette draws with that palette's colours.
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

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const CSS = path.join(ROOT, 'core', 'ui', 'css');
const OUT = path.join(ROOT, 'core', 'src', 'uiclient', 'ThemeTokens.generated.h');
const { loadThemes } = createRequire(import.meta.url)(path.join(ROOT, 'core', 'ui', 'scripts', 'themes-manifest.js'));
const MANIFEST = loadThemes();

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
  ['modalBg', ['--modal-bg']],
  ['modalBgRaised', ['--modal-bg-raised']],
  ['modalBorder', ['--modal-border']],
  ['modalBorderMedium', ['--modal-border-medium']],
  ['modalText', ['--modal-text']],
  ['modalTextSecondary', ['--modal-text-secondary']],
  ['modalTextMuted', ['--modal-text-muted']],
];

/** The C++ name of a palette's tokens: kLight for "light". */
const paletteName = (id) => `k${id.replace(/(^|-)(\w)/g, (_, __, c) => c.toUpperCase())}`;

/** The palettes: the themes that are their own base, each with its sheet and class. */
const THEMES = MANIFEST.THEMES.filter((theme) => theme.base === theme.id).map((theme) => [
  paletteName(theme.id),
  theme.sheets[0],
  `.${theme.classes[0]}`,
]);

const cppString = (text) => JSON.stringify(text);

/** The custom properties declared in the first `selector { ... }` block of a stylesheet. */
function readBlock(file, selector) {
  const text = fs.readFileSync(path.join(CSS, file), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '');
  const start = text.search(new RegExp(`(^|\\n)\\s*${selector.replace('.', '\\.')}\\s*\\{`));
  if (start < 0) throw new Error(`${file}: no ${selector} block`);
  const open = text.indexOf('{', start);
  const close = text.indexOf('}', open);
  const vars = new Map();
  for (const m of text.slice(open + 1, close).matchAll(/(--[\w-]+)\s*:\s*([^;]+);/g)) vars.set(m[1], m[2].trim());
  return vars;
}

function resolve(value, vars, depth = 0) {
  if (depth > 10) throw new Error(`var() loop resolving ${value}`);
  return value.replace(/var\(\s*(--[\w-]+)\s*(?:,\s*([^)]+))?\)/g, (_, name, fallback) => {
    if (vars.has(name)) return resolve(vars.get(name), vars, depth + 1);
    if (fallback) return resolve(fallback.trim(), vars, depth + 1);
    throw new Error(`undefined ${name}`);
  });
}

function toArgb(value) {
  let v = value.trim();
  const gradient = v.match(/gradient\([^,]+,\s*(#[0-9a-f]{3,8}|rgba?\([^)]*\))/i);
  if (gradient) v = gradient[1];
  const mix = v.match(/color-mix\(in \w+,\s*(#[0-9a-f]{3,8}|rgba?\([^)]*\))/i);
  if (mix) v = mix[1];
  let m = v.match(/^#([0-9a-f]{3}|[0-9a-f]{6})$/i);
  if (m) {
    const hex = m[1].length === 3 ? [...m[1]].map((c) => c + c).join('') : m[1];
    return (0xff000000 | parseInt(hex, 16)) >>> 0;
  }
  m = v.match(/^rgba?\(([^)]*)\)$/i);
  if (m) {
    const parts = m[1].split(/[\s,/]+/).filter(Boolean);
    const [r, g, b] = parts.slice(0, 3).map(Number);
    let a = parts.length > 3 ? parts[3] : '1';
    a = a.endsWith('%') ? Number(a.slice(0, -1)) / 100 : Number(a);
    return ((Math.round(a * 255) << 24) | (r << 16) | (g << 8) | b) >>> 0;
  }
  throw new Error(`not a colour: ${value}`);
}

function generate() {
  const declWidth = Math.max(...TOKENS.map(([field]) => `std::uint32_t ${field};`.length));
  const base = readBlock('variables.css', ':root');
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
  for (const [name, file, selector] of THEMES) {
    const vars = new Map([...base, ...readBlock(file, selector)]);
    const values = TOKENS.map(([field, props]) => {
      const prop = props.find((p) => vars.has(p));
      if (!prop) throw new Error(`${name}: none of ${props.join(', ')} defined`);
      return `0x${toArgb(resolve(vars.get(prop), vars)).toString(16).padStart(8, '0')}`;
    });
    lines.push(`inline constexpr ThemeTokens ${name}{`);
    TOKENS.forEach(([field], i) => lines.push(`    ${values[i]}, // ${field}`));
    lines.push('};', '');
  }
  lines.push(
    '/// A theme of the web UI (core/ui/ts/themes.ts): its stored name, the name pickers show, whether',
    '/// it is dark, and the palette it draws with.',
    'struct ThemeInfo',
    '{',
    '    std::string_view id;',
    '    std::string_view label;',
    '    bool dark;',
    '    const ThemeTokens* tokens;',
    '};',
    '',
    '/// Every theme, in the order the pickers list them.',
    'inline constexpr ThemeInfo kThemes[] = {',
    ...MANIFEST.THEMES.map(
      (theme) =>
        `    {${cppString(theme.id)}, ${cppString(theme.label)}, ${theme.scheme === 'dark'}, &${paletteName(theme.base)}},`,
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
