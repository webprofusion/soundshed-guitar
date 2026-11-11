#!/usr/bin/env node
/**
 * Draws the pictures of Sepia's own pedals and renders them to
 * core/ui/images/equipment/pedals/sepia-<id>.png, where the effect panel shows them.
 *
 * Every pedal is the same wide stompbox drawn from one template: an enclosure in the pedal's
 * colour, its real control names under a row of knobs, a motif that says what it does, its
 * name, a lamp and a footswitch. Original artwork for this project; change a pedal here and re-run.
 *
 *   node tools/sepia-ui/render-pedals.js
 */
const { chromium } = require('playwright');
const path = require('node:path');

const OUT = path.resolve(__dirname, '..', '..', 'core', 'ui', 'images', 'equipment', 'pedals');
const W = 1200;
const H = 800;

/** Motifs: each draws inside the box x 110..590, y 400..720; svg() places that box on the pedal. */
const motifs = {
  reels: (ink) => `
    <g fill="none" stroke="${ink}" stroke-width="10">
      <circle cx="240" cy="560" r="92"/><circle cx="460" cy="560" r="92"/>
      <path d="M240 652 H460"/>
    </g>
    <g fill="${ink}">
      ${[240, 460].map((cx) => [0, 120, 240].map((a) => {
        const r = (a - 90) * Math.PI / 180;
        return `<circle cx="${(cx + 46 * Math.cos(r)).toFixed(1)}" cy="${(560 + 46 * Math.sin(r)).toFixed(1)}" r="17"/>`;
      }).join('')).join('')}
      <circle cx="240" cy="560" r="12"/><circle cx="460" cy="560" r="12"/>
    </g>`,
  steps: (ink) => {
    // A sine wave drawn as a staircase: what a bitcrusher does.
    let d = '';
    const n = 14;
    for (let i = 0; i < n; i += 1) {
      const x0 = 130 + (440 / n) * i;
      const x1 = 130 + (440 / n) * (i + 1);
      const y = 560 - 100 * Math.round(4 * Math.sin((i + 0.5) / n * 2 * Math.PI)) / 4;
      d += `${i === 0 ? 'M' : 'L'}${x0.toFixed(1)} ${y.toFixed(1)} L${x1.toFixed(1)} ${y.toFixed(1)} `;
    }
    return `<path d="${d}" fill="none" stroke="${ink}" stroke-width="12" stroke-linejoin="miter"/>`;
  },
  swell: (ink) => {
    // A note that dies away, and one that grows instead.
    const wave = (grow) => {
      let d = '';
      for (let i = 0; i <= 220; i += 1) {
        const t = i / 220;
        const amp = grow ? 20 + 80 * t : 100 * Math.exp(-3.2 * t);
        const x = 130 + 440 * t;
        const y = (grow ? 620 : 500) - amp * Math.sin(t * 2 * Math.PI * 9) * 0.55;
        d += `${i === 0 ? 'M' : 'L'}${x.toFixed(1)} ${y.toFixed(1)} `;
      }
      return d;
    };
    return `<path d="${wave(false)}" fill="none" stroke="${ink}" stroke-opacity="0.45" stroke-width="7"/>
            <path d="${wave(true)}" fill="none" stroke="${ink}" stroke-width="9"/>`;
  },
  record: (ink) => `
    <g fill="none" stroke="${ink}">
      <circle cx="350" cy="560" r="150" stroke-width="10"/>
      ${[128, 110, 92, 74].map((r) => `<circle cx="350" cy="560" r="${r}" stroke-width="3" stroke-opacity="0.6"/>`).join('')}
      <circle cx="350" cy="560" r="46" stroke-width="8"/>
    </g>
    <circle cx="350" cy="560" r="9" fill="${ink}"/>`,
  soundhole: (ink) => `
    <g fill="none" stroke="${ink}">
      <circle cx="350" cy="560" r="96" stroke-width="12"/>
      <circle cx="350" cy="560" r="122" stroke-width="4"/>
      <circle cx="350" cy="560" r="136" stroke-width="4" stroke-dasharray="6 10"/>
      ${[-55, -33, -11, 11, 33, 55].map((dx) => `<path d="M${350 + dx} 410 V710" stroke-width="${dx < 0 ? 5 : 3}" stroke-opacity="0.8"/>`).join('')}
    </g>`,
  repeats: (ink) => `
    <g fill="${ink}">
      ${[0, 1, 2, 3].map((i) => `<rect x="${140 + i * 108}" y="${500}" width="84" height="120" rx="10" fill-opacity="${1 - i * 0.22}"/>`).join('')}
    </g>
    <path d="M140 660 H560" stroke="${ink}" stroke-width="5" stroke-dasharray="4 14"/>`,
  mouth: (ink) => `
    <g fill="${ink}" font-family="Georgia, 'Times New Roman', serif" font-size="96" font-weight="700" text-anchor="middle">
      <text x="150" y="596" fill-opacity="0.45">U</text><text x="250" y="596" fill-opacity="0.6">O</text>
      <text x="350" y="596">A</text>
      <text x="450" y="596" fill-opacity="0.6">E</text><text x="550" y="596" fill-opacity="0.45">I</text>
    </g>
    <path d="M130 640 Q350 720 570 640" fill="none" stroke="${ink}" stroke-width="8"/>`,
};

const pedals = [
  { id: 'tape', name: 'TAPE', body: ['#e8dcc3', '#cdbb97'], ink: '#3a2a1c', knobs: ['DRIVE', 'WOW', 'FLUTTER', 'TONE'], motif: 'reels' },
  { id: 'bitcrusher', name: 'BITCRUSHER', body: ['#8fa07a', '#687a55'], ink: '#1d2416', knobs: ['BITS', 'DOWNSAMPLE', 'MIX'], motif: 'steps' },
  { id: 'sustainer', name: 'SUSTAINER', body: ['#a34a3a', '#7a3024'], ink: '#f3e2c8', knobs: ['AMOUNT', 'BLOOM', 'HOLD'], motif: 'swell' },
  { id: 'vinyl', name: 'VINYL', body: ['#2b2622', '#191512'], ink: '#d8a657', knobs: ['CRACKLE', 'DUST', 'WEAR'], motif: 'record' },
  { id: 'acoustic', name: 'ACOUSTIC', body: ['#c99a5b', '#a17638'], ink: '#2e1f10', knobs: ['BODY', 'TOP', 'LEVEL'], motif: 'soundhole' },
  { id: 'stutter', name: 'STUTTER', body: ['#4f8a8b', '#33686a'], ink: '#f0e6d2', knobs: ['CHANCE', 'REPEATS', 'REVERSE'], motif: 'repeats' },
  { id: 'vowel-filter', name: 'VOWEL', body: ['#d9a441', '#b07e22'], ink: '#2a1c0c', knobs: ['VOWEL', 'DEPTH', 'RESONANCE'], motif: 'mouth' },
];

function knob(cx, cy, label, ink, turn) {
  const a = (turn - 90) * Math.PI / 180;
  return `
    <circle cx="${cx}" cy="${cy + 7}" r="64" fill="#000" fill-opacity="0.28"/>
    <circle cx="${cx}" cy="${cy}" r="62" fill="url(#knob)" stroke="#0d0b09" stroke-width="3"/>
    <circle cx="${cx}" cy="${cy}" r="42" fill="#2a2420"/>
    <path d="M${cx} ${cy} L${(cx + 56 * Math.cos(a)).toFixed(1)} ${(cy + 56 * Math.sin(a)).toFixed(1)}" stroke="#efe6d6" stroke-width="8" stroke-linecap="round"/>
    <text x="${cx}" y="${cy + 108}" text-anchor="middle" font-family="Helvetica, Arial, sans-serif" font-size="${label.length > 8 ? 22 : 26}" font-weight="700" letter-spacing="2" fill="${ink}">${label}</text>`;
}

/**
 * A wide stompbox, 3:2, because the effect panel shows its picture in a landscape window and
 * crops whatever does not fit: knobs along the top, the motif on the right, the name and the
 * footswitch along the bottom.
 */
function svg(p) {
  const n = p.knobs.length;
  const span = n === 4 ? 170 : 200;
  const first = 150;
  const knobs = p.knobs.map((label, i) => knob(first + span * i, 190, label, p.ink, [-40, 25, -10, 50][i])).join('');
  const nameSize = p.name.length >= 10 ? 76 : p.name.length >= 8 ? 92 : 120;
  // The motifs are drawn in a 460 x 300 window centred at (350, 560); move that window to the right.
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">
  <defs>
    <linearGradient id="body" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${p.body[0]}"/><stop offset="1" stop-color="${p.body[1]}"/></linearGradient>
    <radialGradient id="knob" cx="35%" cy="30%" r="80%"><stop offset="0" stop-color="#6b625a"/><stop offset="1" stop-color="#1b1714"/></radialGradient>
    <radialGradient id="switch" cx="35%" cy="30%" r="80%"><stop offset="0" stop-color="#f4f1ea"/><stop offset="1" stop-color="#8a857c"/></radialGradient>
    <radialGradient id="lamp" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#ffe9a8"/><stop offset="0.5" stop-color="#f0a53a"/><stop offset="1" stop-color="#8a4a10"/></radialGradient>
  </defs>
  <!-- shadow, enclosure, bevel -->
  <rect x="34" y="46" width="${W - 68}" height="${H - 76}" rx="56" fill="#000" fill-opacity="0.35"/>
  <rect x="28" y="28" width="${W - 56}" height="${H - 68}" rx="56" fill="url(#body)" stroke="#0e0b08" stroke-width="5"/>
  <rect x="48" y="48" width="${W - 96}" height="${H - 108}" rx="40" fill="none" stroke="#fff" stroke-opacity="0.22" stroke-width="4"/>
  <!-- screws -->
  ${[[86, 84], [W - 86, 84], [86, H - 96], [W - 86, H - 96]].map(([x, y]) => `<circle cx="${x}" cy="${y}" r="14" fill="#b9b3a8" stroke="#3a352f" stroke-width="3"/><path d="M${x - 9} ${y} H${x + 9}" stroke="#3a352f" stroke-width="3"/>`).join('')}
  ${knobs}
  <!-- motif panel -->
  <rect x="${n === 4 ? 800 : 760}" y="96" width="${n === 4 ? 320 : 360}" height="320" rx="26" fill="#000" fill-opacity="0.10" stroke="${p.ink}" stroke-opacity="0.35" stroke-width="3"/>
  <g transform="translate(${n === 4 ? 20 : 0} 0)"><svg x="${n === 4 ? 790 : 770}" y="106" width="${n === 4 ? 320 : 340}" height="300" viewBox="110 400 480 320" preserveAspectRatio="xMidYMid meet">${motifs[p.motif](p.ink)}</svg></g>
  <!-- name -->
  <text x="100" y="${H - 190}" font-family="Georgia, 'Times New Roman', serif" font-size="${nameSize}" font-weight="700" letter-spacing="${p.name.length >= 8 ? 6 : 14}" fill="${p.ink}">${p.name}</text>
  <text x="104" y="${H - 138}" font-family="Georgia, 'Times New Roman', serif" font-size="30" letter-spacing="12" fill="${p.ink}" fill-opacity="0.7">SEPIA</text>
  <!-- lamp and footswitch -->
  <circle cx="${W - 330}" cy="${H - 220}" r="17" fill="url(#lamp)" stroke="#2a1a08" stroke-width="3"/>
  <circle cx="${W - 200}" cy="${H - 208}" r="74" fill="#000" fill-opacity="0.3"/>
  <circle cx="${W - 200}" cy="${H - 214}" r="70" fill="#55504a" stroke="#14110e" stroke-width="4"/>
  <circle cx="${W - 200}" cy="${H - 219}" r="50" fill="url(#switch)" stroke="#3b3731" stroke-width="3"/>
</svg>`;
}

(async () => {
  const browser = await chromium.launch();
  for (const pedal of pedals) {
    const page = await browser.newPage({ viewport: { width: W, height: H }, deviceScaleFactor: 1 });
    await page.setContent(`<html><body style="margin:0;background:transparent">${svg(pedal)}</body></html>`);
    const file = path.join(OUT, `sepia-${pedal.id}.png`);
    await page.screenshot({ path: file, omitBackground: true });
    await page.close();
    console.log(`wrote ${path.relative(path.resolve(__dirname, '..', '..'), file)}`);
  }
  await browser.close();
})();
