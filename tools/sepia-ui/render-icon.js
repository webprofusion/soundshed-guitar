#!/usr/bin/env node
/** Renders tools/sepia-ui/icon.svg to the two PNGs the app and the installer use. */
const { chromium } = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const ROOT = path.resolve(__dirname, '..', '..');
(async () => {
  const svg = fs.readFileSync(path.join(__dirname, 'icon.svg'), 'utf8');
  const browser = await chromium.launch();
  for (const [size, file] of [[1024, 'core/ui/images/icon.png'], [128, 'core/ui/images/icon_sm.png'], [1024, 'juce/packaging/icon.png']]) {
    const page = await browser.newPage({ viewport: { width: size, height: size }, deviceScaleFactor: 1 });
    await page.setContent(`<html><body style="margin:0;background:transparent">${svg.replace('<svg ', `<svg width="${size}" height="${size}" `)}</body></html>`);
    await page.screenshot({ path: path.join(ROOT, file), omitBackground: true });
    await page.close();
    console.log(`wrote ${file} (${size}px)`);
  }
  await browser.close();
})();
