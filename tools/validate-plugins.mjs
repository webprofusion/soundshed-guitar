#!/usr/bin/env node
/**
 * Validate the built plugin formats with each format's own validator.
 *
 *   VST3, AU   pluginval (Tracktion) at strictness 10: host-compatibility tests
 *              covering parameter fuzzing, state round-trips, editor open/close
 *              and resize, processing across sample rates and block sizes, and
 *              bus layouts. AU is macOS only: pluginval hands it to auval, which
 *              looks components up by registered id, so the bundle is copied into
 *              ~/Library/Audio/Plug-Ins/Components first.
 *   CLAP       clap-validator (free-audio).
 *   LV2        lv2lint when it is installed (Linux); skipped otherwise.
 *   AAX, Standalone are never validated here: AAX needs Avid's harness and PACE
 *              signing, and the standalone is an app with no host contract.
 *
 * Usage:
 *   node tools/validate-plugins.mjs                     # Release, every format this OS builds
 *   node tools/validate-plugins.mjs --config Debug
 *   node tools/validate-plugins.mjs --formats VST3,CLAP # a missing format is then an error
 *   node tools/validate-plugins.mjs --strictness 8 --skip-gui-tests
 *   node tools/validate-plugins.mjs --artefacts <dir>   # default juce/builds/SoundshedGuitar_artefacts/<config>
 *   node tools/validate-plugins.mjs --artefacts nanoq/build/Release/products --product "Soundshed Nano Q"   # Nano Q
 *
 * The validators are pinned by version and sha256 below. A copy already on PATH
 * (or, on macOS, in /Applications) is used as is; otherwise the pinned release is
 * downloaded once into juce/builds/tools/ (override: --tools-dir, or the
 * SSG_VALIDATOR_TOOLS_DIR environment variable). pluginval's logs land in
 * juce/builds/validation/ so CI can upload them when a run fails.
 *
 * Each run gives the plugin a fresh, empty data root (juce/builds/validation-profile/):
 * the document store, presets and settings the core resolves from APPDATA on Windows
 * and HOME elsewhere (core/src/util/FileSystem.cpp). A local run so starts from what a
 * first-time user and a CI runner see, and cannot change the developer's own presets.
 * It is not a full sandbox. A few paths come from the OS rather than the environment
 * and still point at the real per-user folders: the factory preset and click-sound
 * lookups (read only), the editor's startup log, and the WebView2 cache. The macOS AU
 * keeps the real profile outright; see validationEnv.
 *
 * Exit status is 1 if any validated format fails.
 */

import { spawnSync, execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, cpSync, chmodSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const PLATFORM = process.platform; // win32 | darwin | linux

// ---------------------------------------------------------------------------
// Pinned validator releases. Bump the version and every hash together.
// ---------------------------------------------------------------------------
const TOOLS = {
  pluginval: {
    version: "v1.0.4",
    win32: {
      url: "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip",
      sha256: "c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab",
      bin: "pluginval.exe",
    },
    darwin: {
      url: "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_macOS.zip",
      sha256: "3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f",
      bin: "pluginval.app/Contents/MacOS/pluginval",
    },
    linux: {
      url: "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Linux.zip",
      sha256: "c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352",
      bin: "pluginval",
    },
  },
  "clap-validator": {
    version: "0.4.1",
    win32: {
      url: "https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-windows.zip",
      sha256: "d935c3af0a45c3911ea2e900f4aa5d6709dac82bb485f0c4ce28648ab2cd0c10",
      bin: "clap-validator.exe",
    },
    // The macOS and Linux zips wrap a .tar.gz that holds the actual binary: under binaries/
    // in the macOS tarball, at the root of the Linux one.
    darwin: {
      url: "https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-macos-universal.zip",
      sha256: "bbec8cd7d18274e549d5d8c12ece3cec54be966129388dd2e742b9957f2ba9f1",
      bin: "binaries/clap-validator",
      innerTarball: true,
    },
    linux: {
      url: "https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-ubuntu-22.04.zip",
      sha256: "49edadcfb407ea0dd946ce418300e853fbd2660fa4b0d00e4f19ff8eef24ad90",
      bin: "clap-validator",
      innerTarball: true,
    },
  },
};

// pluginval's default per-test timeout is 30 s; a cold prepare that loads NAM
// models and IRs can take longer than that on a busy CI runner.
const PLUGINVAL_TIMEOUT_MS = 120000;

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------
function parseArgs(argv) {
  const args = {
    config: "Release",
    formats: null,
    strictness: 10,
    skipGuiTests: false,
    artefacts: null,
    product: null,
    toolsDir: process.env.SSG_VALIDATOR_TOOLS_DIR || path.join(REPO_ROOT, "juce", "builds", "tools"),
    logsDir: path.join(REPO_ROOT, "juce", "builds", "validation"),
    // Outside logsDir: CI uploads logsDir on failure, and a profile is not a log.
    profileDir: path.join(REPO_ROOT, "juce", "builds", "validation-profile"),
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) fail(`${a} needs a value`);
      return argv[++i];
    };
    if (a === "--config") args.config = next();
    else if (a === "--formats") args.formats = next().split(",").map((f) => f.trim().toUpperCase()).filter(Boolean);
    else if (a === "--strictness") args.strictness = Number(next());
    else if (a === "--skip-gui-tests") args.skipGuiTests = true;
    else if (a === "--artefacts") args.artefacts = path.resolve(next());
    else if (a === "--product") args.product = next();
    else if (a === "--tools-dir") args.toolsDir = path.resolve(next());
    else if (a === "--logs-dir") args.logsDir = path.resolve(next());
    else if (a === "--help" || a === "-h") {
      console.log(readFileSync(fileURLToPath(import.meta.url), "utf8").split("*/")[0].replace(/^\/\*\*\n/, "").replace(/^ \* ?/gm, ""));
      process.exit(0);
    } else fail(`unknown argument ${a}`);
  }
  if (!Number.isInteger(args.strictness) || args.strictness < 1 || args.strictness > 10) fail("--strictness must be 1..10");
  if (!args.artefacts) args.artefacts = path.join(REPO_ROOT, "juce", "builds", "SoundshedGuitar_artefacts", args.config);
  return args;
}

function fail(message) {
  console.error(`[validate-plugins] ${message}`);
  process.exit(2);
}

function log(message) {
  console.log(`[validate-plugins] ${message}`);
}

function banner(title) {
  console.log("\n" + "=".repeat(72) + `\n${title}\n` + "=".repeat(72));
}

// The plugin's display name is the bundle name of every format.
function productName() {
  try {
    const cmake = readFileSync(path.join(REPO_ROOT, "juce", "CMakeLists.txt"), "utf8");
    const m = cmake.match(/^set\(PRODUCT_NAME "([^"]+)"\)/m);
    if (m) return m[1];
  } catch {
    // fall through to the default
  }
  return "Soundshed Guitar";
}

// ---------------------------------------------------------------------------
// Validator binaries
// ---------------------------------------------------------------------------
function onPath(name) {
  try {
    const out = execFileSync(PLATFORM === "win32" ? "where" : "which", [name], { stdio: ["ignore", "pipe", "ignore"] })
      .toString()
      .split(/\r?\n/)
      .map((s) => s.trim())
      .filter(Boolean);
    return out[0] || null;
  } catch {
    return null;
  }
}

function sha256(file) {
  return createHash("sha256").update(readFileSync(file)).digest("hex");
}

function run(cmd, cmdArgs, options = {}) {
  const result = spawnSync(cmd, cmdArgs, { stdio: "inherit", ...options });
  if (result.error) throw result.error;
  return result.status ?? 1;
}

function extractZip(zip, dest) {
  if (PLATFORM === "win32") {
    // Git Bash's tar is GNU tar, which cannot read zips; Expand-Archive always can.
    const status = run("powershell", [
      "-NoProfile",
      "-NonInteractive",
      "-Command",
      `Expand-Archive -LiteralPath '${zip}' -DestinationPath '${dest}' -Force`,
    ]);
    if (status !== 0) fail(`could not extract ${zip}`);
  } else if (run("unzip", ["-o", "-q", zip, "-d", dest]) !== 0) {
    fail(`could not extract ${zip}`);
  }
}

// Returns the path of a usable validator binary, downloading the pinned release
// when nothing suitable is already installed.
function ensureTool(name, toolsDir) {
  const spec = TOOLS[name][PLATFORM];
  if (!spec) fail(`${name} has no pinned release for ${PLATFORM}`);

  const installed = onPath(PLATFORM === "win32" ? `${name}.exe` : name);
  if (installed) {
    log(`${name}: using ${installed} (on PATH)`);
    return installed;
  }
  if (PLATFORM === "darwin" && name === "pluginval" && existsSync("/Applications/pluginval.app/Contents/MacOS/pluginval")) {
    log("pluginval: using /Applications/pluginval.app");
    return "/Applications/pluginval.app/Contents/MacOS/pluginval";
  }

  const dir = path.join(toolsDir, `${name}-${TOOLS[name].version}`);
  const bin = path.join(dir, spec.bin);
  if (existsSync(bin)) {
    log(`${name}: using cached ${bin}`);
    return bin;
  }

  log(`${name} ${TOOLS[name].version}: downloading ${spec.url}`);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const zip = path.join(dir, path.basename(spec.url));
  if (run("curl", ["-sSL", "--fail", "-o", zip, spec.url]) !== 0) fail(`download failed: ${spec.url}`);

  const actual = sha256(zip);
  if (actual !== spec.sha256) fail(`sha256 mismatch for ${path.basename(zip)}: expected ${spec.sha256}, got ${actual}`);

  extractZip(zip, dir);

  if (spec.innerTarball) {
    const tarball = readdirSync(dir).find((f) => f.endsWith(".tar.gz"));
    if (!tarball) fail(`no .tar.gz inside ${path.basename(zip)}`);
    if (run("tar", ["-xzf", path.join(dir, tarball), "-C", dir]) !== 0) fail(`could not extract ${tarball}`);
  }

  if (!existsSync(bin)) fail(`${name}: expected ${spec.bin} inside ${path.basename(zip)}, not found`);
  if (PLATFORM !== "win32") chmodSync(bin, 0o755);
  if (PLATFORM === "darwin") {
    // A freshly downloaded binary carries the quarantine attribute; drop it so the
    // validator can launch without a Gatekeeper prompt on a dev machine.
    spawnSync("xattr", ["-dr", "com.apple.quarantine", dir], { stdio: "ignore" });
  }
  log(`${name}: installed ${bin}`);
  return bin;
}

// ---------------------------------------------------------------------------
// Per-format validation. Each returns "PASS" | "FAIL" | "SKIP".
// ---------------------------------------------------------------------------
function validateWithPluginval(format, target, args) {
  const pluginval = ensureTool("pluginval", args.toolsDir);
  mkdirSync(args.logsDir, { recursive: true });
  banner(`pluginval: ${format} (${args.config}), strictness ${args.strictness}`);
  const pvArgs = [
    "--strictness-level",
    String(args.strictness),
    "--timeout-ms",
    String(PLUGINVAL_TIMEOUT_MS),
    "--output-dir",
    args.logsDir,
  ];
  if (args.skipGuiTests) pvArgs.push("--skip-gui-tests");
  pvArgs.push("--validate", target);
  return run(pluginval, pvArgs, { env: validationEnv(args, format) }) === 0 ? "PASS" : "FAIL";
}

function validateVst3(product, args) {
  const target = path.join(args.artefacts, "VST3", `${product}.vst3`);
  if (!existsSync(target)) return missing("VST3", target, args);
  return validateWithPluginval("VST3", target, args);
}

function validateAu(product, args) {
  if (PLATFORM !== "darwin") return skip("AU", "macOS only");
  const source = path.join(args.artefacts, "AU", `${product}.component`);
  if (!existsSync(source)) return missing("AU", source, args);

  // auval resolves components by registered id, not path, so the freshly built
  // bundle has to be where the registrar scans. Restarting the registrar makes it
  // pick the copy up immediately instead of on its own schedule.
  const componentsDir = path.join(os.homedir(), "Library", "Audio", "Plug-Ins", "Components");
  const installed = path.join(componentsDir, `${product}.component`);
  mkdirSync(componentsDir, { recursive: true });
  rmSync(installed, { recursive: true, force: true });
  cpSync(source, installed, { recursive: true });
  spawnSync("killall", ["-9", "AudioComponentRegistrar"], { stdio: "ignore" });
  log(`AU: installed a copy at ${installed} for auval`);
  return validateWithPluginval("AU", installed, args);
}

function validateClap(product, args) {
  const target = path.join(args.artefacts, "CLAP", `${product}.clap`);
  if (!existsSync(target)) return missing("CLAP", target, args);
  const validator = ensureTool("clap-validator", args.toolsDir);
  banner(`clap-validator: CLAP (${args.config})`);
  return run(validator, ["validate", target], { env: validationEnv(args, "CLAP") }) === 0 ? "PASS" : "FAIL";
}

function validateLv2(product, args) {
  const lv2lint = onPath("lv2lint");
  if (!lv2lint) return skip("LV2", "lv2lint is not installed");
  const target = path.join(args.artefacts, "LV2", `${product}.lv2`);
  if (!existsSync(target)) return missing("LV2", target, args);
  banner(`lv2lint: LV2 (${args.config})`);
  // lv2lint takes the plugin URI and finds the bundle through LV2_PATH.
  const uri = readLv2Uri();
  const env = { ...validationEnv(args, "LV2"), LV2_PATH: path.dirname(target) };
  return run(lv2lint, ["-s", "lv2_generate_ttl", uri], { env }) === 0 ? "PASS" : "FAIL";
}

function readLv2Uri() {
  try {
    const cmake = readFileSync(path.join(REPO_ROOT, "juce", "CMakeLists.txt"), "utf8");
    const m = cmake.match(/set\(GUITARFX_LV2_URI "([^"]+)"/);
    if (m) return m[1];
  } catch {
    // fall through
  }
  return "urn:soundshed:guitar";
}

// The environment a validator (and so the plugin it loads) runs with: the user-profile
// variable the core derives its data root from points at this run's empty profile.
function validationEnv(args, format) {
  // auval finds the AU through the per-user component registry. That lookup has not
  // been tried with a redirected HOME, so the AU keeps the real one.
  if (PLATFORM === "darwin" && format === "AU") return process.env;
  const key = PLATFORM === "win32" ? "APPDATA" : "HOME";
  return { ...process.env, [key]: args.profileDir };
}

function resetValidationProfile(args) {
  rmSync(args.profileDir, { recursive: true, force: true });
  mkdirSync(args.profileDir, { recursive: true });
  log(`plugin data root for this run: ${args.profileDir}`);
}

function skip(format, reason) {
  log(`SKIP ${format}: ${reason}`);
  return "SKIP";
}

// A format the caller asked for by name must exist; one that is merely part of
// this OS's default set may simply not have been built yet.
function missing(format, target, args) {
  if (args.formats) {
    log(`FAIL ${format}: not found at ${target} (build it first)`);
    return "FAIL";
  }
  return skip(format, `not built (${target})`);
}

// ---------------------------------------------------------------------------
function main() {
  const args = parseArgs(process.argv.slice(2));
  const product = args.product ?? productName();

  const defaultFormats = { win32: ["VST3", "CLAP"], darwin: ["VST3", "AU", "CLAP"], linux: ["VST3", "CLAP", "LV2"] }[PLATFORM];
  if (!defaultFormats) fail(`unsupported platform ${PLATFORM}`);
  const formats = args.formats ?? defaultFormats;

  const validators = { VST3: validateVst3, AU: validateAu, CLAP: validateClap, LV2: validateLv2 };
  for (const f of formats) if (!validators[f]) fail(`unknown format ${f}; choose from ${Object.keys(validators).join(", ")}`);

  log(`product "${product}", ${args.config} artefacts at ${args.artefacts}`);
  resetValidationProfile(args);
  const results = {};
  for (const f of formats) results[f] = validators[f](product, args);

  banner("Summary");
  for (const [f, r] of Object.entries(results)) console.log(`  ${r.padEnd(4)} ${f}`);
  const failed = Object.entries(results).filter(([, r]) => r === "FAIL").map(([f]) => f);
  if (failed.length > 0) {
    console.log(`\n[validate-plugins] FAILED: ${failed.join(", ")} (pluginval logs: ${args.logsDir})`);
    process.exit(1);
  }
  console.log("\n[validate-plugins] all validated formats passed");
}

main();
