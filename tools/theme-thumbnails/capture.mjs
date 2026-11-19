#!/usr/bin/env node
/**
 * Captures the theme flyout's pictures (core/ui/images/themes/thumbnails/<theme>.jpg) from the
 * running Standalone app, one per theme in THEME_OPTIONS.
 *
 * Launch the app with WebView2's debug port (tools/agent-ui-debug/README.md), set it up as the
 * pictures should show it (a preset with a few effects, an effect open), then:
 *
 *   node tools/theme-thumbnails/capture.mjs <webSocketDebuggerUrl> [--out <dir>] [--size 320] [--crop 560]
 *
 * Each theme is applied without being saved, the page is laid out at 1280x800 and 100% zoom, its
 * top-left 560x560 square (the meter, the IN, gate and transpose knobs, the first effects in the
 * chain and the top of the open effect) is
 * shot and scaled to the size asked for, and the theme and zoom the app had are put back. A
 * square of the page rather than all of it, so a tile shows the theme's detail at a size that
 * reads. Use a preset whose names are our own (a factory preset of built-in effects), since the
 * pictures ship. The player's own effect-panel background, when one is set, is lifted for the
 * shots so each picture shows the theme's own.
 * Requires Node 22+ (the WebSocket global).
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const wsUrl = args[0];
const option = (name, fallback) => {
  const index = args.indexOf(name);
  return index > 0 ? args[index + 1] : fallback;
};
const outDir = resolve(option("--out", join(here, "..", "..", "core", "ui", "images", "themes", "thumbnails")));
const size = Number(option("--size", "320"));
const VIEW = { width: 1280, height: 800 };
const crop = Number(option("--crop", "560"));
const CROP = { x: 0, y: 0, width: crop, height: crop };

if (!wsUrl || !wsUrl.startsWith("ws")) {
  console.error("Usage: node capture.mjs <webSocketDebuggerUrl> [--out <dir>] [--size 320] [--crop 560]");
  process.exit(2);
}

const socket = new WebSocket(wsUrl);
let nextId = 1;
const pending = new Map();
socket.addEventListener("message", (event) => {
  const message = JSON.parse(event.data);
  const waiter = pending.get(message.id);
  if (waiter) {
    pending.delete(message.id);
    message.error ? waiter.reject(new Error(JSON.stringify(message.error))) : waiter.resolve(message.result);
  }
});
await new Promise((ok, fail) => {
  socket.addEventListener("open", ok, { once: true });
  socket.addEventListener("error", fail, { once: true });
});

function send(method, params = {}) {
  const id = nextId++;
  socket.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}

async function evaluate(expression) {
  const result = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  if (result.exceptionDetails) {
    throw new Error(result.exceptionDetails.exception?.description ?? result.exceptionDetails.text);
  }
  return result.result.value;
}

const sleep = (ms) => new Promise((ok) => setTimeout(ok, ms));

const state = await evaluate(`(async () => {
  const { THEME_OPTIONS, themeSwitcher } = await import("./dist/theme-switcher.js");
  document.getElementById("theme-flyout")?.setAttribute("hidden", "");
  const customBg = document.body.classList.contains("has-custom-visualizer-bg");
  document.body.classList.remove("has-custom-visualizer-bg");
  // Shot at 100%, whatever the player's zoom; put back after, and never saved.
  const zoom = document.body.style.zoom;
  document.body.style.zoom = "1";
  return { themes: THEME_OPTIONS.map((o) => o.value), current: themeSwitcher.getCurrentTheme(), customBg, zoom };
})()`);

mkdirSync(outDir, { recursive: true });
await send("Emulation.setDeviceMetricsOverride", { ...VIEW, deviceScaleFactor: 1, mobile: false });
await sleep(1500);

try {
  for (const theme of state.themes) {
    await evaluate(`(async () => {
      const { themeSwitcher } = await import("./dist/theme-switcher.js");
      themeSwitcher.applyTheme(${JSON.stringify(theme)});
      if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    })()`);
    // Long enough for a theme's backdrop and knob images to load and its transitions to finish.
    await sleep(1200);
    const shot = await send("Page.captureScreenshot", {
      format: "jpeg",
      quality: 82,
      clip: { ...CROP, scale: size / CROP.width },
    });
    const file = join(outDir, `${theme}.jpg`);
    writeFileSync(file, Buffer.from(shot.data, "base64"));
    console.log(`${theme}: ${file}`);
  }
} finally {
  await evaluate(`(async () => {
    const { themeSwitcher } = await import("./dist/theme-switcher.js");
    themeSwitcher.applyTheme(${JSON.stringify(state.current)});
    if (${state.customBg}) document.body.classList.add("has-custom-visualizer-bg");
    document.body.style.zoom = ${JSON.stringify(state.zoom)};
  })()`);
  // Cleared on this connection: an override outlives the socket that set it.
  await send("Emulation.clearDeviceMetricsOverride");
  socket.close();
}
