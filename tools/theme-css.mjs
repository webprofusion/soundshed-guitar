/**
 * The web UI's theme colours, resolved per theme as the browser resolves them: for
 * tools/gen-native-theme.mjs (Nano Q's tokens) and its tests.
 *
 * A theme's custom properties come from three places, in cascade order:
 *   :root      css/variables.css and the other sheets' :root blocks (the base look)
 *   body       the theme's palette sheet(s), on the body's classes: for a variant (stage), its
 *              base's palette (dark.css) and then its own (stage.css)
 *   component  a component token nobody sets falls back to its component sheet's
 *              var(--token, <fallback>), evaluated under the body
 * A :root value that refers to another property is resolved at :root, not under the body:
 * a theme that changes --bg-primary does not change a :root `--x: var(--bg-primary)`.
 */

import fs from 'node:fs';
import path from 'node:path';

const stripComments = (text) => text.replace(/\/\*[\s\S]*?\*\//g, '');

/** Top-level `selector { body }` rules of a sheet (at-rules skipped). */
export function topLevelRules(text) {
  const rules = [];
  let depth = 0;
  let start = 0;
  let open = -1;
  let quote = null;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (quote) {
      if (ch === quote && text[i - 1] !== '\\') quote = null;
      continue;
    }
    if (ch === '"' || ch === "'") quote = ch;
    else if (ch === '{') {
      if (depth++ === 0) open = i;
    } else if (ch === '}' && --depth === 0) {
      const selector = text.slice(start, open).trim();
      if (!selector.startsWith('@')) rules.push({ selector, body: text.slice(open + 1, i) });
      start = i + 1;
    } else if (ch === ';' && depth === 0) start = i + 1;
  }
  return rules;
}

/** The custom properties a rule body declares, in order (semicolons in url() or quotes kept). */
export function customProperties(body) {
  const vars = new Map();
  let depth = 0;
  let quote = null;
  let start = 0;
  const take = (decl) => {
    const m = decl.match(/^\s*(--[\w-]+)\s*:\s*([\s\S]*?)\s*$/);
    if (m) vars.set(m[1], m[2].replace(/\s*!important$/i, ''));
  };
  for (let i = 0; i < body.length; i++) {
    const ch = body[i];
    if (quote) {
      if (ch === quote && body[i - 1] !== '\\') quote = null;
    } else if (ch === '"' || ch === "'") quote = ch;
    else if (ch === '(') depth++;
    else if (ch === ')') depth--;
    else if (ch === ';' && depth === 0) {
      take(body.slice(start, i));
      start = i + 1;
    }
  }
  take(body.slice(start));
  return vars;
}

/** Splits a function's arguments at top-level commas. */
function splitArgs(text) {
  const out = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    if (text[i] === '(') depth++;
    else if (text[i] === ')') depth--;
    else if (text[i] === ',' && depth === 0) {
      out.push(text.slice(start, i).trim());
      start = i + 1;
    }
  }
  out.push(text.slice(start).trim());
  return out;
}

/** The end of the parenthesised group that opens at `open`. */
function closing(text, open) {
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    if (text[i] === '(') depth++;
    else if (text[i] === ')' && --depth === 0) return i;
  }
  throw new Error(`unbalanced parentheses: ${text}`);
}

/** Substitutes every var() in `value` from `scope`, its fallback where the property is unset. */
export function substitute(value, scope, seen = new Set()) {
  let out = '';
  let i = 0;
  for (;;) {
    const at = value.indexOf('var(', i);
    if (at < 0) return out + value.slice(i);
    out += value.slice(i, at);
    const end = closing(value, at + 3);
    const [name, ...rest] = splitArgs(value.slice(at + 4, end));
    if (scope.has(name)) {
      if (seen.has(name)) throw new Error(`var() loop through ${name}`);
      out += substitute(scope.get(name), scope, new Set([...seen, name]));
    } else if (rest.length) out += substitute(rest.join(','), scope, seen);
    else throw new Error(`undefined ${name}`);
    i = end + 1;
  }
}

/** A resolved colour as [r, g, b, a] with r, g, b in 0..255 and a in 0..1. */
export function parseColour(value) {
  const v = value.trim();
  if (/^transparent$/i.test(v)) return [0, 0, 0, 0];
  if (/^white$/i.test(v)) return [255, 255, 255, 1];
  if (/^black$/i.test(v)) return [0, 0, 0, 1];
  let m = v.match(/^#([0-9a-f]{3,8})$/i);
  if (m) {
    let hex = m[1];
    if (hex.length <= 4) hex = [...hex].map((c) => c + c).join('');
    if (hex.length !== 6 && hex.length !== 8) throw new Error(`not a colour: ${value}`);
    const n = (k) => parseInt(hex.slice(k, k + 2), 16);
    return [n(0), n(2), n(4), hex.length === 8 ? n(6) / 255 : 1];
  }
  m = v.match(/^rgba?\(([\s\S]*)\)$/i);
  if (m) {
    const parts = m[1].split(/[\s,/]+/).filter(Boolean);
    const channel = (p) => (p.endsWith('%') ? (Number(p.slice(0, -1)) * 255) / 100 : Number(p));
    const [r, g, b] = parts.slice(0, 3).map(channel);
    const a = parts.length > 3 ? (parts[3].endsWith('%') ? Number(parts[3].slice(0, -1)) / 100 : Number(parts[3])) : 1;
    if ([r, g, b, a].some(Number.isNaN)) throw new Error(`not a colour: ${value}`);
    return [r, g, b, a];
  }
  m = v.match(/^color-mix\(([\s\S]*)\)$/i);
  if (m) return colourMix(splitArgs(m[1]), value);
  m = v.match(/^(?:linear|radial|conic)-gradient\(([\s\S]*)\)$/i);
  if (m) {
    // A gradient's first colour stop stands for it.
    for (const arg of splitArgs(m[1])) {
      const stop = arg.replace(/\s+-?[\d.]+(%|px|deg)?(\s+-?[\d.]+(%|px|deg)?)?$/, '');
      try {
        return parseColour(stop);
      } catch {
        // a direction or shape, not a stop
      }
    }
  }
  throw new Error(`not a colour: ${value}`);
}

/** color-mix(in srgb, A p%, B q%), as CSS Color 5 mixes: premultiplied, and scaled by p + q < 100%. */
function colourMix(args, text) {
  const [space, ...colours] = args;
  if (!/^in\s+srgb$/i.test(space) || colours.length !== 2) throw new Error(`unsupported: ${text}`);
  const parts = colours.map((arg) => {
    let m = arg.match(/^([\s\S]*?)\s+([\d.]+)%$/);
    if (m) return { colour: parseColour(m[1]), pct: Number(m[2]) };
    m = arg.match(/^([\d.]+)%\s+([\s\S]*)$/);
    if (m) return { colour: parseColour(m[2]), pct: Number(m[1]) };
    return { colour: parseColour(arg), pct: null };
  });
  let [p1, p2] = parts.map((p) => p.pct);
  if (p1 === null && p2 === null) p1 = p2 = 50;
  else if (p1 === null) p1 = 100 - p2;
  else if (p2 === null) p2 = 100 - p1;
  const sum = p1 + p2;
  const w1 = p1 / sum;
  const w2 = p2 / sum;
  const [c1, c2] = parts.map((p) => p.colour);
  const alpha = c1[3] * w1 + c2[3] * w2;
  const channel = (k) => (alpha === 0 ? 0 : (c1[k] * c1[3] * w1 + c2[k] * c2[3] * w2) / alpha);
  return [channel(0), channel(1), channel(2), alpha * Math.min(1, sum / 100)];
}

/** 0xAARRGGBB, rounded as Chromium serialises a computed colour. */
export function toArgb([r, g, b, a]) {
  const c = (x) => Math.max(0, Math.min(255, Math.round(x)));
  return ((c(a * 255) << 24) | (c(r) << 16) | (c(g) << 8) | c(b)) >>> 0;
}

/**
 * Reads the CSS once. `sheets` are the page's stylesheets in link order (paths under cssDir);
 * `themes` the manifest's THEMES. Returns {scope, value, fallbacks}: scope(id) is a Map of every
 * custom property set under that theme's body, values substituted; value(id, prop) also falls
 * back to a component token's var() fallback.
 */
export function loadThemeCss(cssDir, sheets, themes) {
  const rootDecls = []; // [specificity, order, prop, value]
  const classDecls = []; // [cls, specificity, order, prop, value]
  const fallbacks = new Map(); // component token -> [[fallback text, sheet]]
  let order = 0;
  for (const sheet of sheets) {
    const text = stripComments(fs.readFileSync(path.join(cssDir, sheet), 'utf8'));
    for (const rule of topLevelRules(text)) {
      const vars = customProperties(rule.body);
      if (!vars.size) continue;
      for (const sel of rule.selector.split(',').map((s) => s.trim())) {
        const m = sel.match(/^(body)?\.(theme-[\w-]+)$/);
        for (const [k, v] of vars) {
          order++;
          if (sel === ':root') rootDecls.push([0, order, k, v]);
          else if (m) classDecls.push([m[2], m[1] ? 11 : 10, order, k, v]);
        }
      }
    }
    if (sheet.startsWith('themes/')) continue;
    for (const m of text.matchAll(/var\(\s*(--[\w-]+)\s*,/g)) {
      const end = closing(text, m.index + 3);
      const fallback = splitArgs(text.slice(m.index + 4, end)).slice(1).join(',');
      const known = fallbacks.get(m[1]);
      if (!known) fallbacks.set(m[1], [[fallback, sheet]]);
      else if (!known.some(([f]) => f === fallback)) known.push([fallback, sheet]);
    }
  }
  const cascade = (entries) => {
    const vars = new Map();
    for (const [, , k, v] of [...entries].sort((a, b) => a[0] - b[0] || a[1] - b[1])) vars.set(k, v);
    return vars;
  };
  const rootRaw = cascade(rootDecls);
  // A :root value is computed at :root: substitute it there.
  const root = new Map();
  for (const [k, v] of rootRaw) {
    try {
      root.set(k, substitute(v, rootRaw));
    } catch {
      // unresolvable at :root (a picture's url(), say); never a token this tool reads
    }
  }

  const byId = new Map(themes.map((t) => [t.id, t]));
  const scopes = new Map();
  const scope = (id) => {
    if (scopes.has(id)) return scopes.get(id);
    const theme = byId.get(id);
    if (!theme) throw new Error(`unknown theme ${id}`);
    const classes = new Set(theme.classes);
    const body = new Map(root);
    const own = cascade(classDecls.filter(([cls]) => classes.has(cls)).map(([, spec, ord, k, v]) => [spec, ord, k, v]));
    for (const [k, v] of own) body.set(k, v);
    const out = new Map();
    for (const [k, v] of body) {
      try {
        out.set(k, own.has(k) ? substitute(v, body) : v);
      } catch {
        // as above
      }
    }
    scopes.set(id, out);
    return out;
  };
  /** A property's value under theme `id`: as set for its body, else its component fallback's. */
  const value = (id, prop) => {
    const s = scope(id);
    if (s.has(prop)) return s.get(prop);
    const fb = fallbacks.get(prop);
    if (!fb) return undefined;
    if (fb.length > 1) throw new Error(`${prop} has different fallbacks: ${fb.map(([f, sheet]) => `${sheet}: ${f}`).join(' / ')}`);
    return substitute(fb[0][0], s);
  };
  return { scope, value, fallbacks };
}

/** The page's stylesheets in link order, under css/ (index.template.html with the themes' links in). */
export function linkedSheets(uiDir, applyThemeStylesheets) {
  const html = applyThemeStylesheets(fs.readFileSync(path.join(uiDir, 'index.template.html'), 'utf8'));
  return [...html.matchAll(/<link\b[^>]*rel=["']stylesheet["'][^>]*href=["']css\/([^"'?]+)["']/g)].map((m) => m[1]);
}
