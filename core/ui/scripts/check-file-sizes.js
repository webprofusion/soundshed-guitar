#!/usr/bin/env node
/**
 * File-size budget for the UI source tree.
 *
 * A file over the budget is not a bug, but it is where bugs hide: the panel
 * builders and message switches that grew past a screenful are the ones nobody
 * can review. New files must come in under budget; the known offenders are
 * listed explicitly below and the list is only ever allowed to shrink.
 *
 *   node scripts/check-file-sizes.js            # verify
 *   node scripts/check-file-sizes.js --update   # re-pin the allowlist
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const ALLOWLIST = path.join(__dirname, 'file-size-allowlist.json');

const BUDGET = 800;

const TARGETS = [
  { dir: path.join(ROOT, 'ts'), ext: '.ts' },
  { dir: path.join(ROOT, 'css'), ext: '.css' },
  { dir: path.join(ROOT, 'ui-components'), ext: '.html' },
];

const toPosix = (p) => p.split(path.sep).join('/');

function listFiles(dir, ext, acc = []) {
  if (!fs.existsSync(dir)) return acc;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) listFiles(full, ext, acc);
    else if (entry.name.endsWith(ext)) acc.push(full);
  }
  return acc;
}

function measure() {
  const sizes = new Map();
  for (const { dir, ext } of TARGETS) {
    for (const file of listFiles(dir, ext)) {
      const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/).length;
      sizes.set(toPosix(path.relative(ROOT, file)), lines);
    }
  }
  return sizes;
}

function main() {
  const sizes = measure();
  const over = [...sizes.entries()].filter(([, lines]) => lines > BUDGET).sort((a, b) => b[1] - a[1]);

  if (process.argv.includes('--update')) {
    const allowed = Object.fromEntries(over);
    fs.writeFileSync(ALLOWLIST, `${JSON.stringify({ budget: BUDGET, allowed }, null, 2)}\n`, 'utf8');
    console.log(`[check-file-sizes] allowlist re-pinned: ${over.length} file(s) over ${BUDGET} lines.`);
    return;
  }

  if (!fs.existsSync(ALLOWLIST)) {
    console.error('[check-file-sizes] no allowlist found. Run: node scripts/check-file-sizes.js --update');
    process.exitCode = 1;
    return;
  }

  const { allowed } = JSON.parse(fs.readFileSync(ALLOWLIST, 'utf8'));
  const failures = [];
  const improvements = [];

  for (const [file, lines] of over) {
    const ceiling = allowed[file];
    if (ceiling === undefined) failures.push(`${file} — ${lines} lines, budget is ${BUDGET} (new file over budget)`);
    else if (lines > ceiling) failures.push(`${file} — grew to ${lines} lines, was ${ceiling}`);
  }

  for (const [file, ceiling] of Object.entries(allowed)) {
    const lines = sizes.get(file);
    if (lines === undefined) improvements.push(`${file} — gone (was ${ceiling})`);
    else if (lines < ceiling) improvements.push(`${file} — ${ceiling} to ${lines} lines`);
  }

  for (const note of improvements) console.log(`[check-file-sizes] improved: ${note}`);

  if (failures.length > 0) {
    console.error(`[check-file-sizes] FAIL — ${failures.length} file(s) over budget:`);
    for (const failure of failures) console.error(`  ${failure}`);
    console.error('\nSplit the file, or re-pin deliberately with: npm run check:sizes -- --update');
    process.exitCode = 1;
    return;
  }

  console.log(
    `[check-file-sizes] OK — ${sizes.size} files checked, ` +
      `${over.length} over the ${BUDGET}-line budget (all known).`
  );

  if (improvements.length > 0) {
    console.log('Allowlist can be tightened: npm run check:sizes -- --update');
  }
}

main();
