#!/usr/bin/env node
/**
 * Opens the web UI against a real engine in headless Chromium, clicks through it, and takes
 * screenshots. No JUCE build needed.
 *
 *   node tools/sepia-ui/shot.js <out.png> [--preset <id>] [--theme <name>] [--width N] [--height N]
 *                               [--click <selector>]... [--shot <file.png>] [--eval <js>] [--wait ms]
 *                               [--trace]
 *
 * Steps run in the order given: --click clicks, --wait pauses, --shot saves a screenshot at that
 * point, --eval prints an expression's value. The final screenshot goes to <out.png>.
 *
 * It serves core/ui and bridges the page to `core/build/tests/SepiaUiReplay --serve`: what the
 * page sends (window.IPlugSendMsg) goes to a headless controller with the factory pack bundled,
 * and what the controller sends back arrives through window.IPlugReceiveData, as in the app.
 * Needs `npm run build` in core/ui and the SepiaUiReplay test target built.
 */
const { chromium } = require('playwright');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const readline = require('node:readline');

const ROOT = path.resolve(__dirname, '..', '..');
const UI = path.join(ROOT, 'core', 'ui');
const REPLAY = path.join(ROOT, 'core', 'build', 'tests', 'SepiaUiReplay');
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.svg': 'image/svg+xml', '.woff2': 'font/woff2', '.jpg': 'image/jpeg', '.webp': 'image/webp' };

function option(name, fallback) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : fallback;
}

(async () => {
  const out = process.argv[2];
  if (!out || out.startsWith('--')) { console.error('usage: shot.js <out.png> [options]'); process.exit(2); }

  const server = http.createServer((req, res) => {
    const file = path.join(UI, decodeURIComponent(req.url.split('?')[0]));
    if (!file.startsWith(UI) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));

  // No --preset: the engine starts as a fresh install does, on its configured startup preset.
  const preset = option('--preset');
  const engine = spawn(REPLAY, preset ? ['--serve', preset] : ['--serve'], { stdio: ['pipe', 'pipe', 'ignore'] });
  const pending = [];   // engine messages not yet delivered to the page
  let deliver = null;   // set once the page is up
  let settled = () => {};
  readline.createInterface({ input: engine.stdout }).on('line', (line) => {
    if (line === '.') { settled(); return; }
    if (deliver) { deliver(line); } else { pending.push(line); }
  });

  const browser = await chromium.launch();
  const page = await browser.newPage({
    viewport: { width: Number(option('--width', 1400)), height: Number(option('--height', 900)) },
    deviceScaleFactor: Number(option('--scale', 1)),
  });
  const errors = [];
  let sent = 0;
  let received = 0;
  page.on('pageerror', (e) => errors.push(String(e).split('\n')[0]));
  const trace = process.argv.includes('--trace');
  const typeOf = (text) => { try { return JSON.parse(text).type; } catch { return '?'; } };
  await page.exposeFunction('__sepiaSend', (message) => {
    sent += 1;
    if (trace) { console.log(`-> ${typeOf(message)}`); }
    engine.stdin.write(message.replace(/\n/g, ' ') + '\n');
  });
  // The UI hands IPlugSendMsg its message already serialised.
  await page.addInitScript(() => { window.IPlugSendMsg = (m) => window.__sepiaSend(typeof m === 'string' ? m : JSON.stringify(m)); });
  await page.goto(`http://127.0.0.1:${server.address().port}/index.html`);
  await page.waitForFunction(() => typeof window.IPlugReceiveData === 'function', null, { timeout: 15000 });
  deliver = (line) => {
    received += 1;
    if (trace) { console.log(`<- ${typeOf(line)} (${line.length})`); }
    page.evaluate((m) => window.IPlugReceiveData(m), line).catch((e) => console.error(`deliver: ${String(e).split('\n')[0]}`));
  };
  pending.splice(0).forEach(deliver);

  /** Lets the page and the engine finish talking: quiet for 400 ms. */
  const idle = async () => {
    let last = -1;
    while (last !== sent + received) { last = sent + received; await page.waitForTimeout(400); }
  };
  await idle();

  const theme = option('--theme');
  if (theme) {
    await page.evaluate((t) => { document.body.className = document.body.className.replace(/theme-\S+/g, '').trim() + ` theme-${t}`; }, theme);
  }

  for (let i = 3; i < process.argv.length; i += 1) {
    const [flag, value] = [process.argv[i], process.argv[i + 1]];
    if (flag === '--click') {
      await page.click(value, { timeout: 5000 }).catch((e) => console.error(`click ${value}: ${String(e).split('\n')[0]}`));
      await idle();
    } else if (flag === '--wait') {
      await page.waitForTimeout(Number(value));
    } else if (flag === '--shot') {
      await page.screenshot({ path: value });
      console.log(`${value}: saved`);
    } else if (flag === '--eval') {
      console.log(JSON.stringify(await page.evaluate(value)));
    }
  }

  await page.screenshot({ path: out });
  console.log(`${out}: ${sent} messages to the engine, ${received} back, ${errors.length} page error(s)`);
  errors.slice(0, 5).forEach((e) => console.log('  ' + e));
  await browser.close();
  engine.stdin.end();
  server.close();
})();
