#!/usr/bin/env node
/**
 * Copies third-party browser bundles into dist/ so the WebView can load them
 * offline. Runs automatically as `npm run postbuild`.
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const NODE_MODULES = path.join(ROOT, 'node_modules');
const DIST = path.join(ROOT, 'dist');

const FILES = [
  ['jszip/dist/jszip.min.js', 'jszip.min.js'],
  // The Practice Tool's tab view loads alphaTab on first use (ts/practiceTool/tab/alphaTabLoader.ts),
  // so it lives under vendor/, which the editor's launch prefetch skips. Only the
  // Bravura web fonts ship: the player runs in external-media mode, so no soundfont.
  ['@coderline/alphatab/dist/alphaTab.min.js', 'vendor/alphatab/alphaTab.min.js'],
  ['@coderline/alphatab/LICENSE', 'vendor/alphatab/LICENSE'],
  ['@coderline/alphatab/dist/font/Bravura.woff2', 'vendor/alphatab/font/Bravura.woff2'],
  ['@coderline/alphatab/dist/font/Bravura.woff', 'vendor/alphatab/font/Bravura.woff'],
  ['@coderline/alphatab/dist/font/Bravura-OFL.txt', 'vendor/alphatab/font/Bravura-OFL.txt'],
];

let copied = 0;
const missing = [];

for (const [source, target] of FILES) {
  const from = path.join(NODE_MODULES, source);
  const to = path.join(DIST, target);
  if (!fs.existsSync(from)) {
    missing.push(source);
    continue;
  }
  fs.mkdirSync(path.dirname(to), { recursive: true });
  fs.copyFileSync(from, to);
  copied += 1;
}

console.log(`[copy-vendor] copied ${copied}/${FILES.length} vendor files into dist/`);

if (missing.length > 0) {
  console.error(`[copy-vendor] missing dependencies (run npm install): ${missing.join(', ')}`);
  process.exitCode = 1;
}
