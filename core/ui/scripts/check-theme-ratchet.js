#!/usr/bin/env node
/**
 * Theme ratchet: themes should restyle the UI through tokens, not through more and more
 * per-theme rules. Three counts are pinned per file and may only go down:
 *
 *   literals        colour literals (#hex, rgb(), hsl()) in a component sheet, outside a
 *                   var(--token, fallback): each one is a colour no theme can change
 *   themeSelectors  selectors naming a .theme-* class in a component sheet: theme rules
 *                   belong in css/themes/
 *   skinRules       declarations in a theme's skin sheet (css/themes/<id>-skin.css): what
 *                   that theme restyles by hand instead of through its palette
 *
 * A count going up fails. A new component sheet starts at zero, so a literal colour in it
 * fails too: read a token (var(--x, <base value>)) and set it in the palettes that differ.
 * See docs/theme-system.md.
 *
 *   node scripts/check-theme-ratchet.js            # verify against the baseline
 *   node scripts/check-theme-ratchet.js --update   # re-pin the baseline (deliberate)
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const CSS_DIR = path.join(ROOT, 'css');
const BASELINE = path.join(__dirname, 'theme-ratchet-baseline.json');
const toPosix = (p) => p.split(path.sep).join('/');

function listCss(dir, acc = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) listCss(full, acc);
    else if (entry.name.endsWith('.css')) acc.push(full);
  }
  return acc;
}

const stripComments = (css) => css.replace(/\/\*[\s\S]*?\*\//g, '');

/** Removes every var(...) call, nested parentheses included, so fallback literals do not count. */
function stripVars(css) {
  let out = '';
  let i = 0;
  while (i < css.length) {
    const at = css.indexOf('var(', i);
    if (at < 0) {
      out += css.slice(i);
      break;
    }
    out += css.slice(i, at);
    let depth = 0;
    let j = at + 3;
    for (; j < css.length; j++) {
      if (css[j] === '(') depth++;
      else if (css[j] === ')' && --depth === 0) break;
    }
    i = j + 1;
  }
  return out;
}

const LITERAL = /#[0-9a-fA-F]{3,8}\b|\b(?:rgba?|hsla?)\(/g;

/** Selector preludes (the text before each '{' that opens a style rule) and the declarations. */
function scan(css) {
  const selectors = [];
  let declarations = 0;
  let prelude = '';
  for (const ch of css) {
    if (ch === '{') {
      const p = prelude.trim();
      if (p && !p.startsWith('@')) selectors.push(...p.split(','));
      prelude = '';
    } else if (ch === '}' || ch === ';') {
      if (/^\s*[-a-zA-Z]+\s*:/.test(prelude)) declarations++;
      prelude = '';
    } else prelude += ch;
  }
  return { selectors, declarations };
}

function measure() {
  const counts = {};
  for (const file of listCss(CSS_DIR)) {
    const rel = toPosix(path.relative(CSS_DIR, file));
    const css = stripComments(fs.readFileSync(file, 'utf8'));
    if (rel.startsWith('themes/')) {
      if (rel.endsWith('-skin.css')) counts[rel] = { skinRules: scan(css).declarations };
      continue;
    }
    const literals = (stripVars(css).match(LITERAL) || []).length;
    const themeSelectors = scan(css).selectors.filter((s) => s.includes('.theme-')).length;
    if (literals || themeSelectors) counts[rel] = { literals, themeSelectors };
  }
  return counts;
}

function main() {
  const current = measure();
  if (process.argv.includes('--update')) {
    fs.writeFileSync(BASELINE, `${JSON.stringify(current, null, 2)}\n`);
    console.log(`[check-theme-ratchet] baseline re-pinned for ${Object.keys(current).length} sheet(s).`);
    return;
  }
  if (!fs.existsSync(BASELINE)) {
    console.error('[check-theme-ratchet] no baseline found. Run: node scripts/check-theme-ratchet.js --update');
    process.exitCode = 1;
    return;
  }
  const baseline = JSON.parse(fs.readFileSync(BASELINE, 'utf8'));
  const worse = [];
  let better = 0;
  for (const [file, counts] of Object.entries(current)) {
    for (const [metric, value] of Object.entries(counts)) {
      const pinned = baseline[file]?.[metric] ?? 0;
      if (value > pinned) worse.push(`  css/${file}: ${metric} ${pinned} -> ${value}`);
      else if (value < pinned) better += pinned - value;
    }
  }
  for (const [file, counts] of Object.entries(baseline)) {
    for (const [metric, pinned] of Object.entries(counts)) if (!(current[file]?.[metric] >= 0)) better += pinned;
  }
  if (worse.length) {
    console.error('[check-theme-ratchet] theme-specific styling grew:');
    console.error(worse.join('\n'));
    console.error(
      '\nRead a token in the component sheet (var(--token, <base value>)) and set it in the theme palettes that differ;' +
        '\nkeep .theme-* rules in css/themes/. Re-pin deliberately with: npm run check:theme-ratchet -- --update'
    );
    process.exitCode = 1;
    return;
  }
  if (better) console.log(`[check-theme-ratchet] OK. Improvement: ${better} fewer since the baseline; tighten it with: npm run check:theme-ratchet -- --update`);
  else console.log('[check-theme-ratchet] OK.');
}

main();
