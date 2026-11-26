/**
 * Studio 1973's analog VU meter face: images/themes/studio-1973/vu-face.png.
 *
 * Draws the face as SVG (a lamp-lit cream card behind a black bezel, the -20..+3 VU scale with
 * its red zone, the red dBFS peak scale under it and the VU and PEAK legends) and has a headless
 * Chrome or Edge rasterise it at 4x, so the web UI and the native UI (Nano Q) draw the same
 * picture. The moving parts are not in it: each UI draws the VU needle, pivoting below the
 * window (PIVOT), and the red peak dot, which rides the peak scale's line (PEAK_R).
 *
 *   node tools/studio-1973-theme-assets/vu-face.mjs [--svg <file>]   (--svg also keeps the SVG)
 *
 * The geometry is in CSS pixels of the 66 x 48 face; the skin (css/themes/studio-1973-skin.css)
 * and the native gauge (nanoq/src/ui/widgets/Widgets.cpp, MeterBar) repeat PIVOT, NEEDLE_R,
 * SWEEP_DEG and PEAK_R, so change them together.
 */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '../..');
const out = path.join(repo, 'core/ui/images/themes/studio-1973/vu-face.png');
const interTtf = path.join(repo, 'core/ui/css/fonts/Inter-VariableFont_opsz,wght.ttf');

const W = 66;
const H = 48;
const SCALE = 4;
/** The VU needle's pivot, below the window as on the real thing. */
const PIVOT = { x: 33, y: 52 };
/** Half the sweep, either side of upright. */
const SWEEP_DEG = 42;
const ARC_R = 36;
const NEEDLE_R = 41;
/** The peak scale's line, under the VU arc: the red peak dot rides it. */
const PEAK_R = ARC_R - 4.5;

/** Needle position (0..1) of a level in VU: 0 VU is 1/1.413 of the way, +3 VU the end stop. */
const deflection = (vu) => Math.pow(10, vu / 20) / Math.SQRT2;
const angleOf = (d) => (-SWEEP_DEG + 2 * SWEEP_DEG * Math.min(1, Math.max(0, d))) * (Math.PI / 180);
const at = (r, a) => [PIVOT.x + r * Math.sin(a), PIVOT.y - r * Math.cos(a)];
/** The peak dot's angle for a sample peak in dBFS: the same sweep, full scale at the stop. */
const peakAngle = (dbfs) => angleOf(Math.pow(10, dbfs / 20));
const f = (n) => n.toFixed(3);

function arc(r, a0, a1) {
  const [x0, y0] = at(r, a0);
  const [x1, y1] = at(r, a1);
  return `M${f(x0)} ${f(y0)} A${r} ${r} 0 0 1 ${f(x1)} ${f(y1)}`;
}

function face() {
  const parts = [];
  const zero = angleOf(deflection(0));
  const lo = angleOf(0);
  const hi = angleOf(1);

  // The red zone: a band outside the arc from 0 VU to the stop.
  parts.push(
    `<path d="${arc(ARC_R + 1.6, zero, hi)}" stroke="#c3281c" stroke-width="3.2" fill="none"/>`,
  );
  // The scale line, black to 0 VU and red past it.
  parts.push(`<path d="${arc(ARC_R, lo, zero)}" stroke="#1b130c" stroke-width="0.55" fill="none"/>`);
  parts.push(`<path d="${arc(ARC_R, zero, hi)}" stroke="#b01e14" stroke-width="0.55" fill="none"/>`);

  const marks = [
    [-20, '20'],
    [-10, '10'],
    [-7, '7'],
    [-5, '5'],
    [-3, '3'],
    [-2, '2'],
    [-1, '1'],
    [0, '0'],
    [1, '1'],
    [2, '2'],
    [3, '3'],
  ];
  for (const [vu, label] of marks) {
    const a = angleOf(deflection(vu));
    const red = vu > 0;
    const major = vu === 0 || vu === -20 || vu === 3 || vu === -10;
    const [x0, y0] = at(ARC_R, a);
    const [x1, y1] = at(ARC_R + (major ? 4.2 : 3.2), a);
    parts.push(
      `<line x1="${f(x0)}" y1="${f(y0)}" x2="${f(x1)}" y2="${f(y1)}" stroke="${red ? '#b01e14' : '#1b130c'}" stroke-width="${major ? 0.75 : 0.55}"/>`,
    );
    const [tx, ty] = at(ARC_R + 7.2, a);
    parts.push(
      `<text x="${f(tx)}" y="${f(ty + 1.7)}" font-size="${vu === 0 ? 5 : 4.4}" fill="${red ? '#b01e14' : '#1b130c'}" text-anchor="middle" transform="rotate(${f((a * 180) / Math.PI)} ${f(tx)} ${f(ty)})">${label}</text>`,
    );
  }
  // Minor ticks between -20 and -10, and the half steps up top.
  for (const vu of [-15, -12, -8, -6, -4, -2.5, -1.5, -0.5, 0.5, 1.5, 2.5]) {
    const a = angleOf(deflection(vu));
    const [x0, y0] = at(ARC_R, a);
    const [x1, y1] = at(ARC_R + 1.8, a);
    parts.push(
      `<line x1="${f(x0)}" y1="${f(y0)}" x2="${f(x1)}" y2="${f(y1)}" stroke="${vu > 0 ? '#b01e14' : '#1b130c'}" stroke-width="0.4"/>`,
    );
  }
  // The peak scale under the arc, in red, for the red peak dot that rides its line: dBFS on
  // the same sweep law with full scale at the end stop (ts/vuMeter.ts, vuPeakDeflection), so
  // -3 dBFS sits under 0 VU, where the red zone starts. Ticks hang inside the line, so the dot
  // never hides a number.
  parts.push(`<path d="${arc(PEAK_R, peakAngle(-20), peakAngle(0))}" stroke="#b01e14" stroke-width="0.35" fill="none"/>`);
  for (const db of [-20, -15, -10, -8, -6, -5, -4, -3, -2, -1, 0]) {
    const labelled = [-20, -10, -6, -3, 0].includes(db);
    const a = peakAngle(db);
    const [x0, y0] = at(PEAK_R, a);
    const [x1, y1] = at(PEAK_R - (labelled ? 2.4 : 1.5), a);
    parts.push(
      `<line x1="${f(x0)}" y1="${f(y0)}" x2="${f(x1)}" y2="${f(y1)}" stroke="#b01e14" stroke-width="${labelled ? 0.45 : 0.35}"/>`,
    );
    if (labelled) {
      const [tx, ty] = at(PEAK_R - 5.2, a);
      parts.push(
        `<text x="${f(tx)}" y="${f(ty + 1.1)}" font-size="3.1" fill="#b01e14" text-anchor="middle" transform="rotate(${f((a * 180) / Math.PI)} ${f(tx)} ${f(ty)})">${db === 0 ? '0' : String(-db)}</text>`,
      );
    }
  }
  // The legend, and the tiny "-" and "+" either end.
  parts.push(
    `<text x="${PIVOT.x}" y="36.5" font-size="8.6" font-weight="800" letter-spacing="0.2" fill="#1b130c" text-anchor="middle">VU</text>`,
    `<text x="${PIVOT.x}" y="41.4" font-size="2.9" font-weight="700" letter-spacing="0.35" fill="#b01e14" text-anchor="middle">PEAK</text>`,
    `<text x="7.6" y="44.2" font-size="5.4" font-weight="700" fill="#1b130c" text-anchor="middle">−</text>`,
    `<text x="58.4" y="44.2" font-size="5.4" font-weight="700" fill="#b01e14" text-anchor="middle">+</text>`,
  );
  return parts.join('\n    ');
}

function svg(fontUrl) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W * SCALE}" height="${H * SCALE}" viewBox="0 0 ${W} ${H}">
  <defs>
    <style>
      @font-face { font-family: Face; src: url("${fontUrl}"); font-weight: 100 900; }
      text { font-family: Face, Arial, sans-serif; font-weight: 600; }
    </style>
    <!-- The lamp: warm in the middle and low, the card's corners in the shade of the bezel. -->
    <radialGradient id="lamp" cx="50%" cy="78%" r="78%">
      <stop offset="0" stop-color="#fff6da"/>
      <stop offset="0.45" stop-color="#fbe8b8"/>
      <stop offset="0.8" stop-color="#ecc985"/>
      <stop offset="1" stop-color="#c99a55"/>
    </radialGradient>
    <linearGradient id="bezel" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#3a2e24"/>
      <stop offset="0.12" stop-color="#15100b"/>
      <stop offset="0.9" stop-color="#0c0805"/>
      <stop offset="1" stop-color="#2a2018"/>
    </linearGradient>
    <linearGradient id="hood" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#000" stop-opacity="0.42"/>
      <stop offset="0.22" stop-color="#000" stop-opacity="0"/>
    </linearGradient>
    <filter id="grain" x="0" y="0" width="100%" height="100%">
      <feTurbulence type="fractalNoise" baseFrequency="1.6" numOctaves="2" seed="7"/>
      <feColorMatrix values="0 0 0 0 0.35  0 0 0 0 0.25  0 0 0 0 0.12  0 0 0 0.09 0"/>
      <feComposite in2="SourceGraphic" operator="in"/>
    </filter>
  </defs>
  <rect x="0" y="0" width="${W}" height="${H}" rx="3" fill="url(#bezel)"/>
  <rect x="0.35" y="0.35" width="${W - 0.7}" height="${H - 0.7}" rx="2.7" fill="none" stroke="rgb(255 236 205 / 0.22)" stroke-width="0.5"/>
  <rect x="2" y="2" width="${W - 4}" height="${H - 4}" rx="1.4" fill="url(#lamp)"/>
  <rect x="2" y="2" width="${W - 4}" height="${H - 4}" rx="1.4" fill="#fff" filter="url(#grain)"/>
  <g>
    ${face()}
  </g>
  <rect x="2" y="2" width="${W - 4}" height="${H - 4}" rx="1.4" fill="url(#hood)"/>
  <rect x="2" y="2" width="${W - 4}" height="${H - 4}" rx="1.4" fill="none" stroke="rgb(0 0 0 / 0.55)" stroke-width="0.6"/>
</svg>
`;
}

function findBrowser() {
  const found = [
    process.env.CHROME_PATH,
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
    '/usr/bin/google-chrome',
    '/usr/bin/chromium',
  ].find((p) => p && fs.existsSync(p));
  if (!found) throw new Error('No Chrome or Edge found: set CHROME_PATH');
  return found;
}

const keepSvg = process.argv.indexOf('--svg');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'vu-face-'));
try {
  const svgFile = path.join(work, 'vu-face.svg');
  const pngFile = path.join(work, 'vu-face.png');
  fs.copyFileSync(interTtf, path.join(work, 'face.ttf'));
  fs.writeFileSync(svgFile, svg('face.ttf'));
  if (keepSvg > 0) fs.writeFileSync(process.argv[keepSvg + 1], svg(interTtf.replace(/\\/g, '/')));
  // A page that is just the picture, so the screenshot is the picture.
  const html = path.join(work, 'face.html');
  fs.writeFileSync(
    html,
    `<!doctype html><style>html,body{margin:0;background:transparent}img{display:block}</style><img src="vu-face.svg">`,
  );
  execFileSync(
    findBrowser(),
    [
      '--headless',
      '--disable-gpu',
      '--hide-scrollbars',
      '--force-device-scale-factor=1',
      `--window-size=${W * SCALE},${H * SCALE}`,
      '--default-background-color=00000000',
      '--allow-file-access-from-files',
      `--user-data-dir=${path.join(work, 'profile')}`,
      `--screenshot=${pngFile}`,
      `file:///${html.replace(/\\/g, '/')}`,
    ],
    { stdio: 'ignore' },
  );
  fs.copyFileSync(pngFile, out);
  console.log(`wrote ${path.relative(repo, out)} (${W * SCALE}x${H * SCALE})`);
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
