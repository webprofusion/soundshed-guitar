#!/usr/bin/env node
/**
 * Every stylesheet must be reachable from the page.
 *
 * Stylesheets are pulled in by a hand-maintained list of <link> tags in
 * index.template.html, and CSS is not part of the CMake dependency glob. So a
 * new file that nobody links fails *silently* — the app builds, boots, and
 * renders one panel unstyled. This catches that, and the reverse: a <link>
 * pointing at a file that no longer exists.
 *
 *   node scripts/check-stylesheets.js
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const CSS_DIR = path.join(ROOT, 'css');
const TEMPLATE = path.join(ROOT, 'index.template.html');

const toPosix = (p) => p.split(path.sep).join('/');

function listCss(dir, acc = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) listCss(full, acc);
    else if (entry.name.endsWith('.css')) acc.push(full);
  }
  return acc;
}

/** Hrefs from <link rel="stylesheet" href="..."> in the template. */
function linkedStylesheets() {
  const html = fs.readFileSync(TEMPLATE, 'utf8');
  const hrefs = new Set();
  const re = /<link\b[^>]*rel=["']stylesheet["'][^>]*href=["']([^"']+)["']|<link\b[^>]*href=["']([^"']+)["'][^>]*rel=["']stylesheet["']/gi;
  let match;
  while ((match = re.exec(html))) hrefs.add(match[1] ?? match[2]);
  return hrefs;
}

/** Targets of @import inside a stylesheet, resolved relative to it. */
function importedFrom(file) {
  const css = fs.readFileSync(file, 'utf8');
  const targets = new Set();
  const re = /@import\s+(?:url\()?["']([^"']+)["']/g;
  let match;
  while ((match = re.exec(css))) {
    targets.add(toPosix(path.relative(ROOT, path.resolve(path.dirname(file), match[1]))));
  }
  return targets;
}

function main() {
  const files = listCss(CSS_DIR).map((f) => toPosix(path.relative(ROOT, f)));
  const linked = linkedStylesheets();

  // A stylesheet counts as reachable if it is linked directly, or @imported by
  // something already reachable. Resolve transitively.
  const reachable = new Set([...linked].filter((href) => files.includes(href)));
  let grew = true;
  while (grew) {
    grew = false;
    for (const file of [...reachable]) {
      for (const target of importedFrom(path.join(ROOT, file))) {
        if (files.includes(target) && !reachable.has(target)) {
          reachable.add(target);
          grew = true;
        }
      }
    }
  }

  const orphans = files.filter((f) => !reachable.has(f));
  const dangling = [...linked].filter((href) => href.startsWith('css/') && !files.includes(href));

  if (orphans.length === 0 && dangling.length === 0) {
    console.log(`[check-stylesheets] OK — all ${files.length} stylesheets are reachable from index.template.html.`);
    return;
  }

  if (orphans.length > 0) {
    console.error(`[check-stylesheets] FAIL — ${orphans.length} stylesheet(s) that nothing loads:`);
    for (const orphan of orphans) console.error(`  ${orphan}`);
    console.error('\nAdd a <link> in index.template.html, or @import it from a stylesheet that is already linked.');
  }

  if (dangling.length > 0) {
    console.error(`[check-stylesheets] FAIL — ${dangling.length} <link> tag(s) pointing at a missing file:`);
    for (const href of dangling) console.error(`  ${href}`);
  }

  process.exitCode = 1;
}

main();
