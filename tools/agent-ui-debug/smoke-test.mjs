#!/usr/bin/env node
/**
 * smoke-test.mjs — boot the real Standalone app and assert the UI came up.
 *
 * This is the only check that catches the failure mode a module refactor
 * actually risks: an ES-module import cycle whose evaluation order flips, so a
 * module reads a binding from a peer that has not finished evaluating and
 * throws a TDZ ReferenceError at import time. `tsc` compiles it happily; the
 * app shows a blank window. Same for a render pass that dies half-way on a DOM
 * call, or a stylesheet nobody linked.
 *
 * It launches the app with WebView2's remote-debugging port, reloads the page
 * so a full bootstrap is observed from a clean slate with listeners already
 * attached, then asserts on what the boot produced.
 *
 * Requires Node 22+ (built-in global WebSocket).
 *
 *   node tools/agent-ui-debug/smoke-test.mjs
 *   node tools/agent-ui-debug/smoke-test.mjs --config Debug
 *   node tools/agent-ui-debug/smoke-test.mjs --attach 9333   # already running
 *   node tools/agent-ui-debug/smoke-test.mjs --keep-open     # leave it up
 */

import { spawn, execSync } from "node:child_process";
import { cpSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const UI_SRC = path.join(REPO_ROOT, "core", "ui");
const EXE_NAME = "Soundshed Guitar.exe";

/**
 * Runtime UI files, mirroring what the WebView actually loads. CMake's
 * post-build step copies all of core/ui (node_modules included, ~188 MB); this
 * copies only what matters so the smoke test can refresh in about a second.
 */
const RUNTIME_UI_ENTRIES = [
  "index.html", "dist", "css", "images", "data", "assets", "metronome", "demo", "presets",
];

/** console.error text matching these is tolerated — noise, not a boot failure. */
const IGNORED_CONSOLE_PATTERNS = [
  /favicon/i,
  /net::ERR_FILE_NOT_FOUND.*favicon/i,
];

/**
 * Failures that only ever come from the forced reload, not from the app.
 *
 * Page.reload tears down and re-creates the page while JUCE's custom resource
 * provider is still serving, so the parallel `fetch("data/*.json")` calls at
 * boot can be rejected at the transport layer ("Failed to fetch", as opposed to
 * a 404). A real user never hard-reloads, and the whole check is retried once
 * before this is reported — so a deterministic fault still fails, while this
 * race does not.
 */
const TRANSIENT_PATTERNS = [/Error loading .*: Failed to fetch/i];

const isTransient = (failure) => TRANSIENT_PATTERNS.some((pattern) => pattern.test(failure));

function parseArgs(argv) {
  const out = { port: 9333, config: "Release", exe: null, attach: false, keepOpen: false, sync: true, timeoutMs: 45000 };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--port") out.port = Number(argv[++i]);
    else if (argv[i] === "--config") out.config = argv[++i];
    else if (argv[i] === "--exe") out.exe = argv[++i];
    else if (argv[i] === "--attach") { out.attach = true; if (/^\d+$/.test(argv[i + 1] ?? "")) out.port = Number(argv[++i]); }
    else if (argv[i] === "--keep-open") out.keepOpen = true;
    else if (argv[i] === "--no-sync") out.sync = false;
    else if (argv[i] === "--timeout") out.timeoutMs = Number(argv[++i]);
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function resolveExe() {
  if (args.exe) return args.exe;
  const candidate = path.join(
    REPO_ROOT, "juce", "builds", "SoundshedGuitar_artefacts", args.config, "Standalone", EXE_NAME
  );
  if (!existsSync(candidate)) {
    throw new Error(
      `No Standalone build at:\n  ${candidate}\n` +
      `Build it first:\n  cmake --build juce/builds --config ${args.config} --target SoundshedGuitar_Standalone --parallel`
    );
  }
  return candidate;
}

async function findPageTarget(port, deadline) {
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/list`);
      const targets = await response.json();
      // Skip the initial about:blank the WebView shows before JUCE navigates
      // it to the app — reloading that would just give us a blank page.
      const page = targets.find(
        (t) => t.type === "page" && t.webSocketDebuggerUrl && t.url && t.url !== "about:blank"
      );
      if (page) return page;
    } catch {
      // Port not listening yet.
    }
    await sleep(400);
  }
  throw new Error(`No CDP page target on port ${port} within the timeout. Did the app start?`);
}

/** Minimal CDP client over the target's websocket. */
function connect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  const pending = new Map();
  const events = [];
  let nextId = 1;

  ws.addEventListener("message", (event) => {
    const message = JSON.parse(event.data.toString());
    if (message.id && pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) reject(new Error(JSON.stringify(message.error)));
      else resolve(message.result);
    } else if (message.method) {
      events.push(message);
    }
  });

  const ready = new Promise((resolve, reject) => {
    ws.addEventListener("open", resolve);
    ws.addEventListener("error", () => reject(new Error("CDP websocket failed to open")));
  });

  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      ws.send(JSON.stringify({ id, method, params }));
    });

  return { ws, send, events, ready };
}

async function evaluate(send, expression) {
  const result = await send("Runtime.evaluate", {
    expression,
    returnByValue: true,
    awaitPromise: true,
  });
  if (result.exceptionDetails) {
    throw new Error(`Evaluate threw: ${result.exceptionDetails.text} ${result.exceptionDetails.exception?.description ?? ""}`);
  }
  return result.result.value;
}

/**
 * Rebuild the UI and refresh the copy the exe actually loads. Without this the
 * smoke test verifies whatever the last CMake build deployed, not the source
 * that was just edited — which would make it worse than no test at all.
 */
function syncUi(exe) {
  console.log("[smoke] building UI bundle");
  execSync("npm run build", { cwd: UI_SRC, stdio: "inherit" });

  const destination = path.join(path.dirname(exe), "resources", "ui");
  if (!existsSync(destination)) {
    throw new Error(`No deployed UI at ${destination}. Build the Standalone target once so CMake creates it.`);
  }

  for (const entry of RUNTIME_UI_ENTRIES) {
    const from = path.join(UI_SRC, entry);
    if (!existsSync(from)) continue;
    cpSync(from, path.join(destination, entry), { recursive: true, force: true });
  }
  console.log(`[smoke] synced ${RUNTIME_UI_ENTRIES.length} runtime path(s) into the artefact`);
}

function killApp() {
  try {
    execSync(`taskkill /F /IM "${EXE_NAME}"`, { stdio: "ignore" });
  } catch {
    // Already gone.
  }
}

/** Boot assertions. Each returns null when it passes, or a failure string. */
const CHECKS = [
  {
    name: "bootstrap completed (splash removed)",
    expression: `(() => {
      const splash = document.getElementById('splash-screen');
      if (!splash) return null;
      const error = splash.querySelector('.splash-error');
      if (error) return 'startup failed: ' + error.textContent;
      return 'splash still on screen after timeout — bootstrap did not finish';
    })()`,
  },
  {
    name: "message bridge registered",
    expression: `typeof window.IPlugReceiveData === 'function' ? null : 'window.IPlugReceiveData was never registered — main.ts did not reach the end of bootstrap'`,
  },
  {
    name: "app shell rendered",
    expression: `(() => {
      const missing = ['app', 'signal-path-bar', 'signal-path-nodes', 'panel-visualizer', 'panel-settings']
        .filter((id) => !document.getElementById(id));
      if (missing.length) return 'missing DOM roots: ' + missing.join(', ');
      return document.querySelector('main.main-content') ? null : 'main.main-content missing';
    })()`,
  },
  {
    name: "active panel visible",
    expression: `(() => {
      const panel = document.getElementById('panel-visualizer');
      if (!panel) return 'panel-visualizer missing';
      const visible = panel.getClientRects().length > 0;
      return visible ? null : 'the default panel rendered but is not visible';
    })()`,
  },
  {
    name: "signal path rendered content",
    expression: `(() => {
      const host = document.getElementById('signal-path-nodes');
      if (!host) return 'signal-path-nodes missing';
      return host.children.length > 0 ? null : 'signal path rendered zero nodes — a render pass probably threw';
    })()`,
  },
  {
    name: "stylesheets all loaded",
    expression: `(() => {
      const links = Array.from(document.querySelectorAll('link[rel="stylesheet"]'));
      const broken = links.filter((link) => {
        try { return link.sheet === null; } catch { return true; }
      }).map((link) => link.getAttribute('href'));
      return broken.length ? 'stylesheet(s) failed to load: ' + broken.join(', ') : null;
    })()`,
  },
];

async function main() {
  let child = null;
  const failures = [];

  if (!args.attach) {
    const exe = resolveExe();
    if (args.sync) syncUi(exe);
    killApp();
    await sleep(500);
    console.log(`[smoke] launching ${path.relative(REPO_ROOT, exe)} with debug port ${args.port}`);
    // With --keep-open the app has to outlive this process, so detach it and
    // unref it; otherwise Windows tears the child down when we exit.
    child = spawn(exe, [], {
      env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${args.port}` },
      detached: args.keepOpen,
      stdio: "ignore",
    });
    if (args.keepOpen) child.unref();
    child.on("error", (error) => {
      console.error(`[smoke] failed to launch: ${error.message}`);
      process.exit(1);
    });
  } else {
    console.log(`[smoke] attaching to an app already running on port ${args.port}`);
  }

  const deadline = Date.now() + args.timeoutMs;
  const target = await findPageTarget(args.port, deadline);
  console.log(`[smoke] attached to "${target.title}"`);

  const { send, events, ready } = connect(target.webSocketDebuggerUrl);
  await ready;

  await send("Runtime.enable");
  await send("Log.enable");
  await send("Page.enable");

  /** One full reload-and-assert cycle. Returns the failures it found. */
  async function runCycle(attempt) {
    const found = [];
    const seenBefore = events.length;

    console.log(`[smoke] reloading to observe a clean bootstrap${attempt > 1 ? ` (attempt ${attempt})` : ""}`);
    await send("Page.reload", { ignoreCache: true });

    // Wait for the splash to be torn down, or for the timeout.
    let booted = false;
    const cycleDeadline = Date.now() + Math.min(args.timeoutMs, Math.max(5000, deadline - Date.now()));
    while (Date.now() < cycleDeadline) {
      await sleep(300);
      try {
        const state = await evaluate(send, `(() => {
          const splash = document.getElementById('splash-screen');
          if (!splash) return 'done';
          return splash.querySelector('.splash-error') ? 'failed' : 'waiting';
        })()`);
        if (state === "done" || state === "failed") { booted = true; break; }
      } catch {
        // Execution context is torn down mid-reload; keep polling.
      }
    }
    if (!booted) found.push("timed out waiting for bootstrap to settle");

    // Give any late async work (preset load, backend round-trips) a moment to throw.
    await sleep(1500);

    const passed = [];
    for (const check of CHECKS) {
      try {
        const failure = await evaluate(send, check.expression);
        if (failure) found.push(`${check.name}: ${failure}`);
        else passed.push(check.name);
      } catch (error) {
        found.push(`${check.name}: check itself threw — ${error.message}`);
      }
    }

    const fresh = events.slice(seenBefore);

    // Uncaught exceptions during boot are always fatal.
    for (const detail of fresh.filter((e) => e.method === "Runtime.exceptionThrown").map((e) => e.params.exceptionDetails)) {
      found.push(`uncaught exception during boot: ${(detail.exception?.description ?? detail.text).split("\n")[0]}`);
    }

    // console.error output is fatal unless explicitly ignored.
    const consoleErrors = fresh
      .filter((e) => e.method === "Runtime.consoleAPICalled" && e.params.type === "error")
      .map((e) => e.params.args.map((a) => a.value ?? a.description ?? "").join(" ").trim())
      .filter((text) => text && !IGNORED_CONSOLE_PATTERNS.some((pattern) => pattern.test(text)));
    for (const error of consoleErrors) found.push(`console.error during boot: ${error.split("\n")[0]}`);

    return { found, passed };
  }

  let cycle = await runCycle(1);

  // Retry once when every failure looks like the reload race rather than a
  // defect. A real fault reproduces; the race does not.
  if (cycle.found.length > 0 && cycle.found.every(isTransient)) {
    console.log(`[smoke] all ${cycle.found.length} failure(s) look like the reload race — retrying once`);
    cycle = await runCycle(2);
  }

  for (const name of cycle.passed) console.log(`[smoke]   ok  ${name}`);
  failures.push(...cycle.found);

  if (!args.keepOpen && !args.attach) {
    killApp();
  } else if (args.keepOpen) {
    console.log(`[smoke] leaving the app running (port ${args.port})`);
  }

  if (failures.length > 0) {
    console.error(`\n[smoke] FAIL — ${failures.length} problem(s):`);
    for (const failure of failures) console.error(`  ${failure}`);
    process.exit(1);
  }

  console.log(`\n[smoke] PASS — app booted clean, ${CHECKS.length} checks, no console errors.`);
  process.exit(0);
}

main().catch((error) => {
  console.error(`[smoke] ERROR — ${error.message}`);
  killApp();
  process.exit(1);
});
