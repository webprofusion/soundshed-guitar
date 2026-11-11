#!/usr/bin/env node
/**
 * Screenshots the web UI with real engine state, without building the JUCE app.
 *
 *   node tools/sepia-ui/shot.js <out.png> [--preset <id>] [--theme <name>] [--width N] [--height N]
 *                               [--click <selector>]... [--eval <js>]
 *
 * It serves core/ui, opens it in headless Chromium, records the messages the UI sends at
 * startup, has core/build/tests/SepiaUiReplay answer them with a real controller (factory pack
 * bundled, one preset loaded), and replays the answers through window.IPlugReceiveData.
 * Needs `npm run build` in core/ui and the SepiaUiReplay test target built.
 */
const { chromium } = require('playwright');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..');
const UI = path.join(ROOT, 'core', 'ui');
const REPLAY = path.join(ROOT, 'core', 'build', 'tests', 'SepiaUiReplay');
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.svg': 'image/svg+xml', '.woff2': 'font/woff2', '.jpg': 'image/jpeg', '.webp': 'image/webp' };

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : fallback;
}
function args(name) {
  return process.argv.flatMap((v, i) => (v === name ? [process.argv[i + 1]] : []));
}

(async () => {
  const out = process.argv[2];
  if (!out) { console.error('usage: shot.js <out.png> [options]'); process.exit(2); }
  const server = http.createServer((req, res) => {
    const file = path.join(UI, decodeURIComponent(req.url.split('?')[0]));
    if (!file.startsWith(UI) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const url = `http://127.0.0.1:${server.address().port}/index.html`;
  const viewport = { width: Number(arg('--width', 1400)), height: Number(arg('--height', 900)) };
  const browser = await chromium.launch();

  const open = async () => {
    const page = await browser.newPage({ viewport, deviceScaleFactor: Number(arg('--scale', 1)) });
    const sent = [];
    await page.exposeFunction('__sepiaSent', (m) => sent.push(m));
    await page.addInitScript(() => { window.IPlugSendMsg = (m) => window.__sepiaSent(JSON.stringify(m)); });
    const errors = [];
    page.on('pageerror', (e) => errors.push(String(e)));
    await page.goto(url);
    await page.waitForTimeout(2500);
    return { page, sent, errors };
  };

  // Pass 1: what does the UI ask for?
  const first = await open();
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'sepia-ui-'));
  const requests = path.join(tmp, 'requests.json');
  const responses = path.join(tmp, 'responses.json');
  fs.writeFileSync(requests, `[${first.sent.join(',')}]`);
  await first.page.close();
  execFileSync(REPLAY, [requests, responses, arg('--preset', 'sepia-warm-clean-room')], { stdio: 'ignore' });

  // Pass 2: the same page, answered.
  const { page, errors } = await open();
  const messages = JSON.parse(fs.readFileSync(responses, 'utf8'));
  await page.evaluate((list) => { for (const m of list) { window.IPlugReceiveData(m); } }, messages);
  await page.waitForTimeout(1500);
  const theme = arg('--theme');
  if (theme) {
    await page.evaluate((t) => { document.body.className = document.body.className.replace(/theme-\S+/g, '').trim() + ` theme-${t}`; }, theme);
  }
  for (const selector of args('--click')) {
    await page.click(selector, { timeout: 5000 }).catch((e) => console.error(`click ${selector}: ${String(e).split('\n')[0]}`));
    await page.waitForTimeout(600);
  }
  const js = arg('--eval');
  if (js) { console.log(JSON.stringify(await page.evaluate(js))); }
  await page.waitForTimeout(500);
  await page.screenshot({ path: out });
  console.log(`${out}: ${messages.length} engine messages replayed, ${errors.length} page error(s)`);
  errors.slice(0, 5).forEach((e) => console.log('  ' + e.split('\n')[0]));
  await browser.close();
  server.close();
})();
