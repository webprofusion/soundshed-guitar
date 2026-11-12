#!/usr/bin/env node
/**
 * Soundshed Nano Q's app icons, from the one drawing nanoq/resources/icon/nanoq-icon.svg:
 *
 *   nanoq/resources/icon/AppIcon.xcassets   iOS and iPadOS: every size an iOS 12 app needs,
 *                                           full bleed and opaque (iOS rounds the corners)
 *   nanoq/resources/icon/NanoQ.icns         the macOS standalone app
 *   nanoq/resources/icon/NanoQ.ico          the Windows standalone app
 *
 * The desktop icons put the drawing in a rounded square with a shadow, on the macOS icon grid
 * (an 824 px square in 1024, as Apple's template); Windows takes the same picture.
 *
 * Run it after changing the drawing, then commit what it writes:
 *
 *   node tools/gen-nanoq-icons.mjs
 *
 * Needs macOS (iconutil, sips) and rsvg-convert (Homebrew's librsvg). The builds only read
 * the committed files, so no build machine needs either.
 */

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ICON_DIR = path.join(ROOT, 'nanoq', 'resources', 'icon');
const SOURCE = path.join(ICON_DIR, 'nanoq-icon.svg');
const CATALOG = path.join(ICON_DIR, 'AppIcon.xcassets');
const APPICONSET = path.join(CATALOG, 'AppIcon.appiconset');

function run(cmd, args) {
  const r = spawnSync(cmd, args, { encoding: 'utf8' });
  if (r.error) throw new Error(`${cmd}: ${r.error.message}`);
  if (r.status !== 0) throw new Error(`${cmd} ${args.join(' ')}\n${r.stderr}`);
  return r.stdout;
}

function render(svgFile, px, out) {
  run('rsvg-convert', ['-w', String(px), '-h', String(px), '-o', out, svgFile]);
}

// The drawing framed for the desktop: the macOS grid's rounded square, with a soft shadow,
// on a transparent 1024 canvas. The drawing goes in as a nested <svg>, unchanged.
function desktopSvg(source) {
  const inner = source.replace(/^[\s\S]*?<svg[^>]*>/, '').replace(/<\/svg>\s*$/, '');
  return `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <defs>
    <clipPath id="tile"><rect x="100" y="100" width="824" height="824" rx="185"/></clipPath>
    <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur stdDeviation="14"/>
    </filter>
  </defs>
  <rect x="100" y="112" width="824" height="824" rx="185" fill="#000" fill-opacity="0.45" filter="url(#shadow)"/>
  <g clip-path="url(#tile)">
    <svg x="100" y="100" width="824" height="824" viewBox="0 0 1024 1024">${inner}</svg>
  </g>
  <rect x="101" y="101" width="822" height="822" rx="184" fill="none" stroke="#fff" stroke-opacity="0.12" stroke-width="2"/>
</svg>
`;
}

// iOS 12 takes no single-size icon, so the catalog lists every slot (Xcode's iOS template
// before Xcode 14). Sizes are points; the file is the pixel size.
const IOS_SLOTS = [
  ['iphone', 20, 2], ['iphone', 20, 3], ['iphone', 29, 2], ['iphone', 29, 3],
  ['iphone', 40, 2], ['iphone', 40, 3], ['iphone', 60, 2], ['iphone', 60, 3],
  ['ipad', 20, 1], ['ipad', 20, 2], ['ipad', 29, 1], ['ipad', 29, 2],
  ['ipad', 40, 1], ['ipad', 40, 2], ['ipad', 76, 1], ['ipad', 76, 2], ['ipad', 83.5, 2],
  ['ios-marketing', 1024, 1],
];

function writeIos(tmp) {
  fs.rmSync(CATALOG, { recursive: true, force: true });
  fs.mkdirSync(APPICONSET, { recursive: true });
  const info = { author: 'xcode', version: 1 };
  fs.writeFileSync(path.join(CATALOG, 'Contents.json'), JSON.stringify({ info }, null, 2) + '\n');

  const images = [];
  const made = new Set();
  for (const [idiom, points, scale] of IOS_SLOTS) {
    const px = Math.round(points * scale);
    const filename = `icon-${px}.png`;
    if (!made.has(px)) {
      const out = path.join(APPICONSET, filename);
      render(SOURCE, px, out);
      if (idiom === 'ios-marketing') {
        // App Store Connect refuses a marketing icon with an alpha channel, and rsvg-convert
        // always writes one. A trip through JPEG (at full quality) drops it.
        const jpg = path.join(tmp, 'marketing.jpg');
        run('sips', ['-s', 'format', 'jpeg', '-s', 'formatOptions', '100', out, '--out', jpg]);
        run('sips', ['-s', 'format', 'png', jpg, '--out', out]);
      }
      made.add(px);
    }
    images.push({ filename, idiom, scale: `${scale}x`, size: `${points}x${points}` });
  }
  fs.writeFileSync(path.join(APPICONSET, 'Contents.json'), JSON.stringify({ images, info }, null, 2) + '\n');
  return made.size;
}

function writeIcns(desktop, tmp) {
  const iconset = path.join(tmp, 'NanoQ.iconset');
  fs.mkdirSync(iconset);
  for (const size of [16, 32, 128, 256, 512]) {
    render(desktop, size, path.join(iconset, `icon_${size}x${size}.png`));
    render(desktop, size * 2, path.join(iconset, `icon_${size}x${size}@2x.png`));
  }
  run('iconutil', ['-c', 'icns', '-o', path.join(ICON_DIR, 'NanoQ.icns'), iconset]);
}

// An .ico of PNG images, which Windows has read since Vista. 256 is written as 0 in the
// one-byte width and height.
function writeIco(desktop, tmp) {
  const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
  const pngs = sizes.map((size) => {
    const out = path.join(tmp, `ico-${size}.png`);
    render(desktop, size, out);
    return fs.readFileSync(out);
  });
  const header = Buffer.alloc(6 + 16 * sizes.length);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(sizes.length, 4);
  let offset = header.length;
  sizes.forEach((size, i) => {
    const entry = 6 + 16 * i;
    header.writeUInt8(size >= 256 ? 0 : size, entry);
    header.writeUInt8(size >= 256 ? 0 : size, entry + 1);
    header.writeUInt8(0, entry + 2);
    header.writeUInt8(0, entry + 3);
    header.writeUInt16LE(1, entry + 4);
    header.writeUInt16LE(32, entry + 6);
    header.writeUInt32LE(pngs[i].length, entry + 8);
    header.writeUInt32LE(offset, entry + 12);
    offset += pngs[i].length;
  });
  fs.writeFileSync(path.join(ICON_DIR, 'NanoQ.ico'), Buffer.concat([header, ...pngs]));
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'nanoq-icons-'));
try {
  const desktop = path.join(tmp, 'desktop.svg');
  fs.writeFileSync(desktop, desktopSvg(fs.readFileSync(SOURCE, 'utf8')));
  const iosCount = writeIos(tmp);
  writeIcns(desktop, tmp);
  writeIco(desktop, tmp);
  console.log(`Wrote ${path.relative(ROOT, CATALOG)} (${iosCount} images), NanoQ.icns and NanoQ.ico`);
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
