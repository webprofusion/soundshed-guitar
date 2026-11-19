#!/usr/bin/env node
/**
 * Reads how each theme draws the effect editor's controls (the knob, the switch, the tabs, a
 * group's box, a control's label and value, the effect panel itself) from the web UI's own CSS,
 * and writes them for the native UI (Nano Q) into core/src/uiclient/ThemeControls.generated.h.
 *
 *   node tools/capture-native-controls.mjs           # rewrite the header
 *   node tools/capture-native-controls.mjs --check   # fail if the header is out of date
 *
 * The styles are the browser's own: a headless Chrome or Edge (CHROME_PATH, else the usual
 * places) lays out the effect panel's markup with every stylesheet the page links, under each
 * theme's body classes, and the computed styles are read back. So a theme's skin rules (a knob
 * picture, a pointer's size, a switch's slot) carry over as they are written, with no copy of
 * them kept by hand. Lengths are in CSS pixels at the web UI's sizes (a 38 px knob), and colours
 * as 0xAARRGGBB, every background flattened to a top and a bottom colour.
 *
 * --check (npm run verify, CI) reads the styles again and fails if the header differs, so a CSS
 * change that moves a control's look is caught, and one that doesn't is let through. Lengths are
 * kept to a hundredth of a pixel, under what layout rounding varies by. Requires Node 22+ (the
 * WebSocket global).
 */
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { linkedSheets } from './theme-css.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const UI = path.join(ROOT, 'core', 'ui');
const OUT = path.join(ROOT, 'core', 'src', 'uiclient', 'ThemeControls.generated.h');
const require = createRequire(import.meta.url);
const MANIFEST = require(path.join(UI, 'scripts', 'themes-manifest.js')).loadThemes();
const { applyThemeStylesheets } = require(path.join(UI, 'scripts', 'assemble-html.js'));
const SHEETS = linkedSheets(UI, applyThemeStylesheets);

const CHECK = process.argv.includes('--check');

// The effect panel's markup, inside the ancestors the web UI puts it in (so rules scoped by them
// apply): a plot's card, the tabs, and a group with a switch (on and off) and a knob.
const FIXTURE = `<!doctype html><html class="is-active"><head><meta charset="utf-8">
<base href="${pathToFileURL(UI).href}/">
${SHEETS.map((s) => `<link rel="stylesheet" href="css/${s}">`).join('\n')}
<style>*, *::before, *::after { transition: none !important; animation: none !important; }</style>
</head><body class="is-standalone"><div id="app" class="prevent-select"><main class="main-content">
<div id="panel-visualizer" class="tab-panel active"><div class="visualization-panel">
<div id="effect-visualization" class="effect-visualization has-selection has-equipment-image">
<section id="node-params-panel" class="node-params-panel visible"><div class="node-params-body">
<section class="default-effect-shell"><div class="default-effect-shell-content has-equipment-image">
<aside class="default-effect-shell-equipment-panel"></aside>
<div class="default-effect-shell-main">
<div class="eq-visualizer"><div class="eq-visualizer-header"><span>EQ Curve</span><span class="eq-visualizer-range">±18 dB</span></div><canvas class="eq-curve-canvas"></canvas></div>
<div class="default-effect-section default-effect-section-controls">
<div class="node-param-tabs"><button class="node-param-tab is-active">Main</button><button class="node-param-tab">Advanced</button></div>
<div class="node-param-tab-panels"><div class="node-param-tab-panel is-active"><div class="params-controls">
<div class="node-param-group-block"><div class="node-param-group-title">Input</div><div class="node-param-group-items">
<div class="node-param-group"><span class="node-param-label">Voice</span><label class="toggle-switch" id="sw-on"><input type="checkbox" checked><span class="toggle-slider"></span></label><span class="node-param-value">On</span></div>
<div class="node-param-group"><span class="node-param-label">Bright</span><label class="toggle-switch" id="sw-off"><input type="checkbox"><span class="toggle-slider"></span></label><span class="node-param-value">Off</span></div>
<div class="node-param-group"><span class="node-param-label">Gain</span><div class="knob node-param-knob" style="--knob-pct: 0.5"><div class="knob-indicator"></div></div><span class="node-param-value">0.50</span></div>
</div></div></div></div></div></div></div></div></section></div></section></div></div></div></main></div></body></html>`;

// Runs in the page: the computed styles of the fixture under the body classes given.
const READ = String.raw`(classes) => {
  document.body.className = 'is-standalone ' + classes.join(' ');
  const cs = (el, pseudo) => getComputedStyle(el, pseudo || null);
  const px = (v) => (v === 'auto' || v === 'none' || v === '' ? 0 : parseFloat(v) || 0);
  const colour = (text) => {
    text = (text || '').trim();
    let m = text.match(/^rgba?\(([\d.]+),\s*([\d.]+),\s*([\d.]+)(?:,\s*([\d.]+))?\)$/);
    if (m) return [+m[1], +m[2], +m[3], m[4] === undefined ? 1 : +m[4]];
    m = text.match(/^color\(srgb ([\d.e-]+) ([\d.e-]+) ([\d.e-]+)(?: \/ ([\d.]+))?\)$/);
    if (m) return [m[1] * 255, m[2] * 255, m[3] * 255, m[4] === undefined ? 1 : +m[4]];
    return [0, 0, 0, 0];
  };
  const COLOUR = /(?:rgba?|color)\([^)]*\)/g;
  const over = (top, under) => {
    const a = top[3] + under[3] * (1 - top[3]);
    if (a <= 0) return [0, 0, 0, 0];
    return [0, 1, 2].map((i) => (top[i] * top[3] + under[i] * under[3] * (1 - top[3])) / a).concat(a);
  };
  // Splits a computed list (backgrounds, shadows) at its top-level commas.
  const layers = (text) => {
    const out = []; let depth = 0, start = 0;
    for (let i = 0; i < text.length; i++) {
      if (text[i] === '(') depth++;
      else if (text[i] === ')') depth--;
      else if (text[i] === ',' && depth === 0) { out.push(text.slice(start, i).trim()); start = i + 1; }
    }
    out.push(text.slice(start).trim());
    return out.filter((s) => s && s !== 'none');
  };
  // A background as a top and a bottom colour: its gradients' first and last stops laid over its
  // colour, the topmost layer last. A radial gradient is kept as its stops, and a picture's url.
  const fill = (el, pseudo) => {
    const s = cs(el, pseudo);
    const base = colour(s.backgroundColor);
    let top = base, bottom = base, image = '', radial = null;
    const bg = layers(s.backgroundImage).reverse();
    for (const layer of bg) {
      const url = layer.match(/^url\("?([^")]+)"?\)/);
      if (url) { image = url[1]; continue; }
      const stops = [...layer.matchAll(new RegExp(COLOUR.source + '(?:\\s+([\\d.]+)%)?', 'g'))]
        .map((m) => ({ c: colour(m[0].replace(/\s+[\d.]+%$/, '')), at: m[1] === undefined ? null : +m[1] / 100 }));
      if (!stops.length) continue;
      if (layer.startsWith('radial-gradient')) {
        const at = layer.match(/at (?:([\d.]+)%|center) (?:([\d.]+)%|center)/);
        radial = { x: at && at[1] ? +at[1] / 100 : 0.5, y: at && at[2] ? +at[2] / 100 : 0.5, stops };
        top = over(stops[0].c, top); bottom = over(stops[stops.length - 1].c, bottom);
        continue;
      }
      top = over(stops[0].c, top); bottom = over(stops[stops.length - 1].c, bottom);
    }
    return { top, bottom, image, radial };
  };
  const shadows = (text) => layers(text).map((l) => {
    const c = colour((l.match(COLOUR) || [''])[0]);
    const n = l.replace(COLOUR, '').replace('inset', '').trim().split(/\s+/).map(px);
    return { colour: c, x: n[0] || 0, y: n[1] || 0, blur: n[2] || 0, spread: n[3] || 0, inset: /\binset\b/.test(l) };
  });
  const text = (el) => {
    const s = cs(el);
    return {
      colour: colour(s.color), size: px(s.fontSize), weight: +s.fontWeight,
      tracking: s.letterSpacing === 'normal' ? 0 : px(s.letterSpacing) / px(s.fontSize),
      upper: s.textTransform === 'uppercase', family: s.fontFamily.split(',')[0].replace(/["']/g, '').trim(),
    };
  };
  const box = (el, pseudo) => {
    const s = cs(el, pseudo);
    return { fill: fill(el, pseudo), border: px(s.borderTopWidth), borderColour: colour(s.borderTopColor),
      radius: px(s.borderTopLeftRadius), shadows: shadows(s.boxShadow) };
  };

  const knob = document.querySelector('.knob');
  const D = knob.getBoundingClientRect().width;
  const face = cs(knob, '::before'), tick = cs(knob, '::after');
  const ind = knob.querySelector('.knob-indicator'), is = cs(ind);
  const indFill = fill(ind);
  const indStops = [...is.backgroundImage.matchAll(new RegExp(COLOUR.source + '(?:\\s+([\\d.]+)%)?', 'g'))];
  // A pointer in two colours (a painted line that carries on engraved): where they change.
  let split = 1;
  for (let i = 1; i < indStops.length; i++) {
    if (indStops[i][1] !== undefined && indStops[i - 1][1] === indStops[i][1]) { split = +indStops[i][1] / 100; break; }
  }
  const pointer = {
    top: px(is.top), width: px(is.width), height: px(is.height), radius: px(is.borderTopLeftRadius),
    colour: split < 1 ? colour(indStops[0][0].replace(/\s+[\d.]+%$/, '')) : indFill.top,
    colourBelow: split < 1 ? indFill.bottom : indFill.top, split, shadows: shadows(is.boxShadow),
  };
  const switches = ['sw-off', 'sw-on'].map((id) => {
    const slider = document.querySelector('#' + id + ' .toggle-slider');
    const sw = document.getElementById(id).getBoundingClientRect();
    const thumb = cs(slider, '::before');
    return { width: sw.width, height: sw.height, slot: box(slider), thumb: box(slider, '::before'),
      thumbSize: px(thumb.width), thumbInset: px(thumb.left) };
  });
  const tabs = document.querySelectorAll('.node-param-tab');
  const shell = document.querySelector('.default-effect-shell');
  return {
    knob: {
      size: D, inset: px(face.top), face: box(knob, '::before'), pointer,
      tick: { colour: colour(tick.backgroundColor), width: px(tick.width), height: px(tick.height), bottom: px(tick.bottom) },
    },
    label: text(document.querySelector('.node-param-label')),
    value: text(document.querySelector('.node-param-value')),
    switchOff: switches[0], switchOn: switches[1],
    tab: text(tabs[1]), tabActive: text(tabs[0]), tabLine: colour(cs(tabs[0]).borderBottomColor),
    tabLineWidth: px(cs(tabs[0]).borderBottomWidth), tabRule: colour(cs(tabs[0].parentElement).borderBottomColor),
    group: box(document.querySelector('.node-param-group-block')),
    groupTitle: text(document.querySelector('.node-param-group-title')),
    shell: box(shell), section: box(document.querySelector('.default-effect-section-controls')),
    art: box(document.querySelector('.default-effect-shell-equipment-panel')),
    card: box(document.querySelector('.eq-visualizer')), cardPadding: px(cs(document.querySelector('.eq-visualizer')).paddingTop),
    cardTitle: text(document.querySelector('.eq-visualizer-header > span')),
    cardRange: text(document.querySelector('.eq-visualizer-range')),
    plot: box(document.querySelector('.eq-curve-canvas')),
  };
}`;

function findBrowser() {
  const candidates = [
    process.env.CHROME_PATH,
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/usr/bin/google-chrome',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ];
  const found = candidates.find((c) => c && fs.existsSync(c));
  if (!found) throw new Error('No Chrome or Edge found: set CHROME_PATH');
  return found;
}

async function capture() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'native-controls-'));
  const page = path.join(dir, 'fixture.html');
  fs.writeFileSync(page, FIXTURE);
  const browser = spawn(
    findBrowser(),
    [
      '--headless=new',
      '--remote-debugging-port=0',
      `--user-data-dir=${path.join(dir, 'profile')}`,
      '--allow-file-access-from-files',
      '--no-first-run',
      '--no-default-browser-check',
      '--disable-gpu',
      // CI's Linux runners give no user namespaces for Chrome's sandbox; this page is our own.
      ...(process.platform === 'linux' ? ['--no-sandbox'] : []),
      'about:blank',
    ],
    { stdio: ['ignore', 'ignore', 'pipe'] },
  );
  try {
    const wsBrowser = await new Promise((ok, fail) => {
      let err = '';
      const timer = setTimeout(() => fail(new Error(`the browser gave no debug address: ${err}`)), 20000);
      browser.stderr.on('data', (d) => {
        err += d;
        const m = err.match(/DevTools listening on (ws:\/\/\S+)/);
        if (m) {
          clearTimeout(timer);
          ok(m[1]);
        }
      });
      browser.on('exit', (code) => fail(new Error(`the browser exited (${code}): ${err}`)));
    });
    const port = new URL(wsBrowser).port;
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const target = targets.find((t) => t.type === 'page');
    const socket = new WebSocket(target.webSocketDebuggerUrl);
    let nextId = 1;
    const pending = new Map();
    const waiters = [];
    socket.addEventListener('message', (e) => {
      const msg = JSON.parse(e.data);
      if (msg.id && pending.has(msg.id)) {
        const w = pending.get(msg.id);
        pending.delete(msg.id);
        msg.error ? w.reject(new Error(JSON.stringify(msg.error))) : w.resolve(msg.result);
      } else if (msg.method) {
        for (const w of waiters.splice(0)) w(msg);
      }
    });
    await new Promise((ok, fail) => {
      socket.addEventListener('open', ok, { once: true });
      socket.addEventListener('error', fail, { once: true });
    });
    const send = (method, params = {}) =>
      new Promise((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve, reject });
        socket.send(JSON.stringify({ id, method, params }));
      });
    await send('Page.enable');
    await send('Emulation.setDeviceMetricsOverride', { width: 1704, height: 929, deviceScaleFactor: 1, mobile: false });
    const loaded = new Promise((ok) => {
      const wait = (msg) => (msg.method === 'Page.loadEventFired' ? ok() : waiters.push(wait));
      waiters.push(wait);
    });
    await send('Page.navigate', { url: pathToFileURL(page).href });
    await loaded;
    await send('Runtime.evaluate', { expression: 'document.fonts.ready', awaitPromise: true });
    const out = {};
    for (const theme of MANIFEST.THEMES) {
      const r = await send('Runtime.evaluate', {
        expression: `(${READ})(${JSON.stringify(theme.classes)})`,
        returnByValue: true,
      });
      if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 600));
      out[theme.id] = r.result.value;
    }
    socket.close();
    return out;
  } finally {
    browser.kill();
    // The profile can stay locked for a moment after the browser goes.
    setTimeout(() => fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 }), 500).unref();
  }
}

// ── The header ──

const hex = ([r, g, b, a]) => {
  const c = (v) => Math.max(0, Math.min(255, Math.round(v)));
  return `0x${[c(a * 255), c(r), c(g), c(b)].map((v) => v.toString(16).padStart(2, '0')).join('').toUpperCase()}`;
};
const num = (v) => {
  const s = (Math.round(v * 100) / 100).toString();
  return `${s.includes('.') ? s : `${s}.0`}f`;
};
const str = (s) => JSON.stringify(s ?? '');
/** A picture's path under core/ui (the native UI's asset root), or "" for none. */
const asset = (url) => {
  if (!url) return '""';
  const file = fileURLToPath(url);
  const rel = path.relative(UI, file).split(path.sep).join('/');
  if (rel.startsWith('..')) throw new Error(`picture outside core/ui: ${file}`);
  return str(rel);
};

/** The box-shadow layers a native box draws: an outer ring (spread, no blur) and the insets. */
function shadowFields(shadows) {
  const ring = shadows.find((s) => !s.inset && s.spread > 0 && s.blur === 0 && s.x === 0 && s.y === 0);
  const drop = shadows.find((s) => !s.inset && s !== ring);
  const hi = shadows.find((s) => s.inset && s.y > 0 && s.blur <= 1);
  const lo = shadows.find((s) => s.inset && s.y < 0);
  return [
    `${num(ring?.spread ?? 0)}, ${hex(ring?.colour ?? [0, 0, 0, 0])}`,
    `${hex(drop?.colour ?? [0, 0, 0, 0])}, ${num(drop?.y ?? 0)}, ${num(drop?.blur ?? 0)}`,
    `${hex(hi?.colour ?? [0, 0, 0, 0])}, ${hex(lo?.colour ?? [0, 0, 0, 0])}`,
  ].join(', ');
}

const boxInit = (b) =>
  `{${hex(b.fill.top)}, ${hex(b.fill.bottom)}, ${num(b.border)}, ${hex(b.borderColour)}, ${num(b.radius)}, ${shadowFields(b.shadows)}}`;
const textInit = (t) =>
  `{${hex(t.colour)}, ${num(t.size)}, ${t.weight}, ${num(t.tracking)}, ${t.upper}, ${str(t.family)}}`;

function themeInit(id, c) {
  const k = c.knob;
  const r = k.face.fill.radial;
  const stops = r ? r.stops : [];
  // A radial face: its centre, and its first and last stops (where the last one is reached).
  const radial = r
    ? `${num(r.x)}, ${num(r.y)}, ${hex(stops[0].c)}, ${hex(stops[stops.length - 1].c)}, ${num(stops[stops.length - 1].at ?? 1)}`
    : `0.5f, 0.5f, 0x00000000, 0x00000000, 1.0f`;
  const p = k.pointer;
  const sw = (s) => `{${num(s.width)}, ${num(s.height)}, ${boxInit(s.slot)}, ${boxInit(s.thumb)}, ${num(s.thumbSize)}, ${num(s.thumbInset)}}`;
  return [
    `    {${str(id)},`,
    `     {${num(k.size)}, ${num(k.inset)}, ${boxInit(k.face)}, ${asset(k.face.fill.image)}, ${radial},`,
    `      {${num(p.top)}, ${num(p.width)}, ${num(p.height)}, ${num(p.radius)}, ${hex(p.colour)}, ${hex(p.colourBelow)}, ${num(p.split)}, ${hex(p.shadows[0]?.colour ?? [0, 0, 0, 0])}, ${num(p.shadows[0]?.blur ?? 0)}},`,
    `      {${hex(k.tick.colour)}, ${num(k.tick.width)}, ${num(k.tick.height)}, ${num(k.tick.bottom)}}},`,
    `     ${textInit(c.label)},`,
    `     ${textInit(c.value)},`,
    `     ${sw(c.switchOff)},`,
    `     ${sw(c.switchOn)},`,
    `     {${textInit(c.tab)}, ${textInit(c.tabActive)}, ${hex(c.tabLine)}, ${num(c.tabLineWidth)}, ${hex(c.tabRule)}},`,
    `     ${boxInit(c.group)}, ${textInit(c.groupTitle)},`,
    `     ${boxInit(c.shell)}, ${boxInit(c.section)}, ${boxInit(c.art)},`,
    `     ${boxInit(c.card)}, ${num(c.cardPadding)}, ${textInit(c.cardTitle)}, ${textInit(c.cardRange)}, ${boxInit(c.plot)}},`,
  ].join('\n');
}

function header(captured) {
  return `#pragma once

// GENERATED by tools/capture-native-controls.mjs from the web UI's CSS (core/ui/css): do not edit.
//
// How each theme draws the effect editor's controls, as the browser computes them: lengths in CSS
// pixels at the web UI's sizes (a ${captured.dark.knob.size} px knob), colours 0xAARRGGBB, every
// background as a top and a bottom colour. A theme's look changes in its CSS; rerun the tool.

#include <cstdint>
#include <string_view>

namespace guitarfx::uiclient::theme
{
/// A filled box: its background (top to bottom), border, corner radius, and its box-shadow as a
/// native box draws one: a hard ring round it (a spread with no blur), a drop shadow, and an
/// inset line along its top and its bottom edge.
struct ControlBox
{
    std::uint32_t fillTop;
    std::uint32_t fillBottom;
    float border;
    std::uint32_t borderColour;
    float radius;
    float ring;
    std::uint32_t ringColour;
    std::uint32_t dropColour;
    float dropY;
    float dropBlur;
    std::uint32_t highlight;
    std::uint32_t shade;
};

/// A line of text: its ink, size, weight, letter spacing (in ems), capitals, and face.
struct ControlText
{
    std::uint32_t colour;
    float size;
    int weight;
    float tracking;
    bool upper;
    std::string_view family;
};

/// The pointer that turns: a bar from \`top\` down, about the knob's centre, in one colour or
/// two (\`colour\` above \`split\` of its length, \`colourBelow\` under it).
struct KnobPointer
{
    float top;
    float width;
    float height;
    float radius;
    std::uint32_t colour;
    std::uint32_t colourBelow;
    float split;
    std::uint32_t glow;
    float glowBlur;
};

/// The still mark under the knob, at six o'clock.
struct KnobTick
{
    std::uint32_t colour;
    float width;
    float height;
    float bottom;
};

/// The knob: its value ring (ThemeTokens' knobRing*) fills the \`inset\` between its edge and its
/// face; the face is a box, a picture (a path under the UI's assets) or a radial gradient.
struct KnobSkin
{
    float size;
    float inset;
    ControlBox face;
    std::string_view image;
    float radialX;
    float radialY;
    std::uint32_t radialInner;
    std::uint32_t radialOuter;
    float radialEnd;
    KnobPointer pointer;
    KnobTick tick;
};

/// A switch in one state: its slot and its thumb, the thumb's size and its inset from the slot.
struct SwitchSkin
{
    float width;
    float height;
    ControlBox slot;
    ControlBox thumb;
    float thumbSize;
    float thumbInset;
};

/// The effect editor's Main and Advanced tabs, the line under the chosen one, the rule under all.
struct TabSkin
{
    ControlText text;
    ControlText active;
    std::uint32_t line;
    float lineWidth;
    std::uint32_t rule;
};

struct ControlSkin
{
    std::string_view id;
    KnobSkin knob;
    ControlText label;
    ControlText value;
    SwitchSkin switchOff;
    SwitchSkin switchOn;
    TabSkin tabs;
    ControlBox group;
    ControlText groupTitle;
    ControlBox shell;
    ControlBox section;
    ControlBox art;
    /// A plot's card (.eq-visualizer: the EQ curve, a cabinet's or pickup's response): its box and
    /// padding, the title and range over the plot, and the plot's own well.
    ControlBox card;
    float cardPadding;
    ControlText cardTitle;
    ControlText cardRange;
    ControlBox plot;
};

// clang-format off
inline constexpr ControlSkin kControlSkins[] = {
${MANIFEST.THEMES.map((t) => themeInit(t.id, captured[t.id])).join('\n')}
};
// clang-format on

/// The theme's controls by id; the first theme's for one it doesn't know.
inline const ControlSkin& FindControlSkin(std::string_view id) noexcept
{
    for (const auto& skin : kControlSkins)
    {
        if (skin.id == id)
        {
            return skin;
        }
    }

    return kControlSkins[0];
}
} // namespace guitarfx::uiclient::theme
`;
}

const captured = await capture();
if (process.argv.includes('--json')) console.log(JSON.stringify(captured, null, 1));
const text = header(captured);
const rel = path.relative(ROOT, OUT);
if (CHECK) {
  const have = fs.existsSync(OUT) ? fs.readFileSync(OUT, 'utf8').replace(/\r\n/g, '\n') : '';
  if (have !== text) {
    console.error(`${rel} no longer matches the CSS: run node tools/capture-native-controls.mjs`);
    process.exit(1);
  }
  console.log(`${rel} is up to date.`);
} else {
  fs.writeFileSync(OUT, text);
  console.log(`Wrote ${rel} (${MANIFEST.THEMES.length} themes).`);
}
