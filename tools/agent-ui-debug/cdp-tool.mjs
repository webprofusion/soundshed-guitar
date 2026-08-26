#!/usr/bin/env node
/**
 * cdp-tool.mjs — minimal Chrome DevTools Protocol client for driving the
 * running Standalone app's WebView2 UI over its remote-debugging port.
 *
 * Why this exists: agent "computer use" tools (browser automation) only
 * reach a real Chrome/Edge browser tab — they cannot see or click a native
 * Win32/WebView2 window. This talks to the *same* WebView2 instance the
 * live app is actually running (real backend, real DSP, real preset data)
 * via CDP, so you get genuine end-to-end verification instead of a static
 * code read. See ../../docs/agent-quickstart.md ("Live UI testing") for
 * the full workflow (launching with the debug port, feature flags, the
 * in-app debug-state snapshot for backend-truth checks, cleanup).
 *
 * Requires Node 22+ (uses the built-in global WebSocket).
 *
 * Usage:
 *   node cdp-tool.mjs <wsUrl> --eval "<js expression>"
 *   node cdp-tool.mjs <wsUrl> --screenshot out.png
 *   node cdp-tool.mjs <wsUrl> --drag x,y,dyPixels
 *   # Flags can be combined; order of effects is: drag -> eval -> screenshot.
 *   # Get <wsUrl> from: curl http://127.0.0.1:<port>/json/list
 *
 * Example — click a button then screenshot:
 *   node cdp-tool.mjs "$WS" --eval "document.getElementById('preset-chooser-label').click()" --screenshot after.png
 *
 * Example — drag a knob (real Input events, not JS-dispatched) then read its value:
 *   node cdp-tool.mjs "$WS" --drag 511,831,60 --eval "document.querySelector('.node-param-knob').dataset.value" --screenshot after.png
 *
 * Example — drag between two points (signal-chain node reordering, and anything
 * else pointer-driven; --drag only moves vertically):
 *   node cdp-tool.mjs "$WS" --drag-to 420,300,700,300 --screenshot after.png
 */

function parseArgs(argv) {
  const out = { wsUrl: argv[0], evalExpr: null, screenshotPath: null, drag: null, dragTo: null };
  for (let i = 1; i < argv.length; i++) {
    if (argv[i] === "--eval") out.evalExpr = argv[++i];
    else if (argv[i] === "--screenshot") out.screenshotPath = argv[++i];
    else if (argv[i] === "--drag") {
      const [x, y, dy] = argv[++i].split(",").map(Number);
      out.drag = { x, y, dy };
    }
    else if (argv[i] === "--drag-to") {
      const [x1, y1, x2, y2] = argv[++i].split(",").map(Number);
      out.dragTo = { x1, y1, x2, y2 };
    }
  }
  return out;
}

const { wsUrl, evalExpr, screenshotPath, drag, dragTo } = parseArgs(process.argv.slice(2));
if (!wsUrl) {
  console.error("Usage: node cdp-tool.mjs <wsUrl> [--drag x,y,dyPixels] [--drag-to x1,y1,x2,y2] [--eval \"<js>\"] [--screenshot out.png]");
  process.exit(1);
}

const ws = new WebSocket(wsUrl);
let id = 1;
const pending = new Map();

function send(method, params = {}) {
  const thisId = id++;
  return new Promise((resolve, reject) => {
    pending.set(thisId, { resolve, reject });
    ws.send(JSON.stringify({ id: thisId, method, params }));
  });
}

ws.addEventListener("message", (event) => {
  const msg = JSON.parse(event.data.toString());
  if (msg.id && pending.has(msg.id)) {
    const { resolve, reject } = pending.get(msg.id);
    pending.delete(msg.id);
    if (msg.error) reject(new Error(JSON.stringify(msg.error)));
    else resolve(msg.result);
  }
});

ws.addEventListener("error", (e) => {
  console.error("WS error:", e.message ?? e);
  process.exit(1);
});

async function dispatchDrag({ x, y, dy }) {
  await send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, buttons: 0 });
  await send("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", clickCount: 1, buttons: 1 });
  const steps = 10;
  for (let i = 1; i <= steps; i++) {
    const cy = y - (dy * i) / steps;
    await send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y: cy, button: "left", buttons: 1 });
  }
  await send("Input.dispatchMouseEvent", { type: "mouseReleased", x, y: y - dy, button: "left", clickCount: 1, buttons: 0 });
}

/**
 * Press, move in steps, release. Pointer-driven UI (signal-chain reordering)
 * needs the intermediate moves: a press followed by a single jump to the far
 * end never crosses the drag threshold in a way the page can track.
 */
async function dispatchDragTo({ x1, y1, x2, y2 }) {
  await send("Input.dispatchMouseEvent", { type: "mouseMoved", x: x1, y: y1, buttons: 0 });
  await send("Input.dispatchMouseEvent", { type: "mousePressed", x: x1, y: y1, button: "left", clickCount: 1, buttons: 1 });
  const steps = 16;
  for (let i = 1; i <= steps; i++) {
    await send("Input.dispatchMouseEvent", {
      type: "mouseMoved",
      x: x1 + ((x2 - x1) * i) / steps,
      y: y1 + ((y2 - y1) * i) / steps,
      button: "left",
      buttons: 1,
    });
  }
  await send("Input.dispatchMouseEvent", { type: "mouseReleased", x: x2, y: y2, button: "left", clickCount: 1, buttons: 0 });
}

ws.addEventListener("open", async () => {
  try {
    await send("Page.enable");
    await send("Runtime.enable");

    if (drag) {
      await dispatchDrag(drag);
    }

    if (dragTo) {
      await dispatchDragTo(dragTo);
    }

    if (evalExpr) {
      const result = await send("Runtime.evaluate", { expression: evalExpr, returnByValue: true, awaitPromise: true });
      if (result.exceptionDetails) {
        console.error("EVAL_EXCEPTION: " + JSON.stringify(result.exceptionDetails));
      } else {
        console.log("EVAL_RESULT: " + JSON.stringify(result.result));
      }
    }

    if (screenshotPath) {
      const shot = await send("Page.captureScreenshot", { format: "png" });
      const fs = await import("node:fs");
      fs.writeFileSync(screenshotPath, Buffer.from(shot.data, "base64"));
      console.error("SAVED " + screenshotPath);
    }

    ws.close();
    process.exit(0);
  } catch (err) {
    console.error("ERR " + err.message);
    process.exit(1);
  }
});
