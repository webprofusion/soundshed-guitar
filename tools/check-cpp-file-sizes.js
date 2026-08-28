#!/usr/bin/env node
/**
 * File-size budget for the C++ source tree.
 *
 * A file over the budget is not a bug, but it is where bugs hide: the message
 * handlers and controller methods that grew past a screenful are the ones
 * nobody can review. New files must come in under budget; the known offenders
 * are listed explicitly below and the list is only ever allowed to shrink.
 *
 *   node tools/check-cpp-file-sizes.js            # verify
 *   node tools/check-cpp-file-sizes.js --update   # re-pin the allowlist
 *
 * This is the C++ counterpart of core/ui/scripts/check-file-sizes.js.
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const ALLOWLIST = path.join(__dirname, 'cpp-file-size-allowlist.json');

const BUDGET = 800;

// First-party sources only. Vendored trees (juce/JUCE, juce/ASIOSDK, build
// outputs, core/src/compat) are somebody else's problem.
const TARGETS = [
  path.join(ROOT, 'core', 'src'),
  path.join(ROOT, 'core', 'config'),
  path.join(ROOT, 'core', 'tests'),
  path.join(ROOT, 'juce', 'source'),
  path.join(ROOT, 'juce', 'tests'),
];

const EXCLUDED_DIRS = new Set(['build', 'builds', 'compat', 'node_modules', '_deps']);
const EXTENSIONS = ['.cpp', '.h', '.hpp'];

const toPosix = (p) => p.split(path.sep).join('/');

function listFiles(dir, acc = []) {
  if (!fs.existsSync(dir)) return acc;
  const entries = fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name));
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (!EXCLUDED_DIRS.has(entry.name)) listFiles(full, acc);
    } else if (EXTENSIONS.some((ext) => entry.name.endsWith(ext))) {
      acc.push(full);
    }
  }
  return acc;
}

function measure() {
  const sizes = new Map();
  for (const dir of TARGETS) {
    for (const file of listFiles(dir)) {
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
    console.log(`[check-cpp-file-sizes] allowlist re-pinned: ${over.length} file(s) over ${BUDGET} lines.`);
    return;
  }

  if (!fs.existsSync(ALLOWLIST)) {
    console.error('[check-cpp-file-sizes] no allowlist found. Run: node tools/check-cpp-file-sizes.js --update');
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

  for (const note of improvements) console.log(`[check-cpp-file-sizes] improved: ${note}`);

  if (failures.length > 0) {
    console.error(`[check-cpp-file-sizes] FAIL — ${failures.length} file(s) over budget:`);
    for (const failure of failures) console.error(`  ${failure}`);
    console.error('\nSplit the file, or re-pin deliberately with: node tools/check-cpp-file-sizes.js --update');
    process.exitCode = 1;
    return;
  }

  console.log(
    `[check-cpp-file-sizes] OK — ${sizes.size} files checked, ` +
      `${over.length} over the ${BUDGET}-line budget (all known).`
  );

  if (improvements.length > 0) {
    console.log('Allowlist can be tightened: node tools/check-cpp-file-sizes.js --update');
  }
}

main();
