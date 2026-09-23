#!/usr/bin/env node
/**
 * The UI↔engine message protocol, checked against one list.
 *
 * Message types are plain strings on both sides: the engine routes them in
 * core/src/dispatcher/ (and in services' MessageHandlerRegistry registrations) and
 * sends replies built as JSON; the UI sends through bridge.postMessage and handles
 * replies through the MESSAGE_HANDLERS table in core/ui/ts/messages.ts. Nothing tied
 * the two together, so routes outlived the UI code that used them, replies went to
 * handlers that no longer existed, and the protocol doc fell behind.
 *
 * Soundshed Guitar Nano's native UI is a second client of the same protocol: it sends through
 * UiClient::Send("type", ...) (and its editors through sendUiMessage(processor, "type", ...))
 * and handles replies registered with On("type", ...), in core/src/uiclient and
 * juce/source/{nativeui,editor}. "The UI" below means both clients together.
 *
 * core/protocol/ui-messages.json is the list. This check reads what each side
 * actually does and fails when either disagrees with it:
 *
 *   - every type the UI sends is routed by the engine (else it is silently dropped),
 *   - every type the engine routes is sent by the UI, or is an engine-only request,
 *   - every type the engine sends is handled by the UI, or is a known unhandled send,
 *   - every type the UI handles is sent by the engine, or is a known unsent handler,
 *   - every type is documented in docs/user-interface.md, bar a pinned backlog that
 *     may only shrink.
 *
 * Each exception carries its reason. `--update` rewrites the two main lists from the
 * code and drops exceptions that no longer apply; a new exception is added with the
 * reason "UNEXPLAINED", which fails until someone writes why (or removes the drift).
 * Effect type GUIDs are checked too: core/src/dsp/EffectGuids.h and
 * core/ui/ts/effectGuids.ts must define the same names with the same values.
 *
 *   node tools/check-protocol.mjs            # verify
 *   node tools/check-protocol.mjs --update   # re-pin from the code (then review the diff)
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const MANIFEST = path.join(ROOT, 'core', 'protocol', 'ui-messages.json');
const UNEXPLAINED = 'UNEXPLAINED';

const rel = (file) => path.relative(ROOT, file).split(path.sep).join('/');
const stripComments = (text) => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/(^|[^:"])\/\/[^\n]*/g, '$1');

function walk(dir, pattern, acc = []) {
  if (!fs.existsSync(dir)) return acc;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, pattern, acc);
    else if (pattern.test(entry.name) && !entry.name.endsWith('.d.ts')) acc.push(full);
  }
  return acc;
}

/** Map of type -> first place it was seen, for every pattern match across the files. */
function collect(files, patterns) {
  const found = new Map();
  for (const file of files) {
    const text = stripComments(fs.readFileSync(file, 'utf8'));
    for (const pattern of patterns) {
      for (const match of text.matchAll(pattern)) {
        if (!found.has(match[1])) found.set(match[1], `${rel(file)}:${text.slice(0, match.index).split('\n').length}`);
      }
    }
  }
  return found;
}

function engineRoutes() {
  const dispatch = [...walk(path.join(ROOT, 'core/src/dispatcher'), /\.cpp$/), path.join(ROOT, 'core/src/MessageDispatcher.cpp')];
  const routes = collect(dispatch, [/\btype == "([^"]+)"/g]);
  // Services that answer their own messages.
  for (const [type, where] of collect(walk(path.join(ROOT, 'core/src'), /\.cpp$/), [/\bregistry\.Register\(\s*"([^"]+)"/g])) routes.set(type, where);
  // Read before the dispatcher sees them: the controller's bootstrap-error intercept and
  // the JUCE host's link opener.
  const intercepts = collect([path.join(ROOT, 'core/src/PluginController.cpp'), path.join(ROOT, 'juce/source/PluginProcessorAdapter.cpp')],
    [/value\("type", std::string\{\}\) == "([^"]+)"/g, /getProperty \(typeId\)\.toString\(\) == "([^"]+)"/g]);
  for (const [type, where] of intercepts) routes.set(type, where);
  return routes;
}

function engineSends() {
  const files = [...walk(path.join(ROOT, 'core/src'), /\.(cpp|h)$/), ...walk(path.join(ROOT, 'juce/source'), /\.(cpp|h)$/)];
  return collect(files, [
    /\{"type",\s*"([^"]+)"\}/g,
    /\["type"\]\s*=\s*"([^"]+)"/g,
    /\{"type":"([A-Za-z0-9_]+)"/g, // inside a raw string literal
    /\w+StateToUI\("([A-Za-z0-9_]+)"\)/g, // a sender that takes the type as its argument
  ]);
}

/** `type: "x"` in the object literal passed to postMessage(...). */
function uiSends() {
  const found = new Map();
  for (const file of walk(path.join(ROOT, 'core/ui/ts'), /\.ts$/)) {
    const text = stripComments(fs.readFileSync(file, 'utf8'));
    for (const call of text.matchAll(/\bpostMessage\s*\(/g)) {
      let depth = 0;
      let end = call.index + call[0].length - 1;
      for (; end < text.length; end += 1) {
        if (text[end] === '(') depth += 1;
        else if (text[end] === ')' && --depth === 0) break;
      }
      const type = text.slice(call.index, end).match(/\btype\s*:\s*["']([^"']+)["']/);
      if (type && !found.has(type[1])) found.set(type[1], `${rel(file)}:${text.slice(0, call.index).split('\n').length}`);
    }
  }
  return found;
}

function uiHandles() {
  const file = path.join(ROOT, 'core/ui/ts/messages.ts');
  const text = stripComments(fs.readFileSync(file, 'utf8'));
  const table = text.slice(text.indexOf('const MESSAGE_HANDLERS'), text.indexOf('};', text.indexOf('const MESSAGE_HANDLERS')));
  return new Map([...table.matchAll(/^\s*"([A-Za-z0-9_]+)"\s*:/gm)].map((m) => [m[1], rel(file)]));
}

/** Soundshed Guitar Nano's client: what it sends, and what it registers handlers for. */
function nativeClientFiles() {
  return [
    ...walk(path.join(ROOT, 'core/src/uiclient'), /\.cpp$/),
    ...walk(path.join(ROOT, 'juce/source/nativeui'), /\.cpp$/),
    ...walk(path.join(ROOT, 'juce/source/editor'), /\.cpp$/),
  ];
}

function nativeSends() {
  return collect(nativeClientFiles(), [/\bSend\(\s*"([A-Za-z0-9_]+)"/g, /\bsendUiMessage\s*\([^,]+,\s*"([A-Za-z0-9_]+)"/g]);
}

function nativeHandles() {
  return collect(walk(path.join(ROOT, 'core/src/uiclient'), /\.cpp$/), [/\bOn\(\s*"([A-Za-z0-9_]+)"/g]);
}

/** Both clients' entries in one map, the web UI's location first. */
function union(web, native) {
  const merged = new Map(web);
  for (const [type, where] of native) if (!merged.has(type)) merged.set(type, where);
  return merged;
}

function documentedTypes() {
  const documented = new Set();
  for (const line of fs.readFileSync(path.join(ROOT, 'docs/user-interface.md'), 'utf8').split('\n')) {
    if (!line.startsWith('| `')) continue;
    for (const m of line.split('|')[1].matchAll(/`([A-Za-z0-9_]+)`/g)) documented.add(m[1]);
  }
  return documented;
}

function effectGuidMismatches() {
  const header = fs.readFileSync(path.join(ROOT, 'core/src/dsp/EffectGuids.h'), 'utf8');
  const cpp = new Map([...header.matchAll(/constexpr const char\* (k\w+) = "([^"]+)";/g)].map((m) => [m[1], m[2]]));
  const ts = fs.readFileSync(path.join(ROOT, 'core/ui/ts/effectGuids.ts'), 'utf8');
  const block = ts.slice(ts.indexOf('export const EffectGuids = {'), ts.indexOf('} as const;'));
  const mirror = new Map([...block.matchAll(/(k\w+):\s*"([^"]+)"/g)].map((m) => [m[1], m[2]]));
  const problems = [];
  for (const name of new Set([...cpp.keys(), ...mirror.keys()])) {
    if (cpp.get(name) !== mirror.get(name)) problems.push(`${name}: EffectGuids.h ${cpp.get(name) ?? '(missing)'}, effectGuids.ts ${mirror.get(name) ?? '(missing)'}`);
  }
  return problems;
}

const sorted = (items) => [...items].sort((a, b) => a.localeCompare(b));

function main() {
  const routes = engineRoutes();
  const sends = engineSends();
  const uiSent = union(uiSends(), nativeSends());
  const handled = union(uiHandles(), nativeHandles());
  const documented = documentedTypes();

  const code = {
    uiToEngine: sorted([...uiSent.keys()].filter((t) => routes.has(t))),
    engineToUi: sorted([...sends.keys()].filter((t) => handled.has(t))),
    engineOnlyRequests: sorted([...routes.keys()].filter((t) => !uiSent.has(t))),
    uiSendsUnrouted: sorted([...uiSent.keys()].filter((t) => !routes.has(t))),
    sentButUnhandled: sorted([...sends.keys()].filter((t) => !handled.has(t))),
    handledButNeverSent: sorted([...handled.keys()].filter((t) => !sends.has(t))),
  };
  const allTypes = new Set([...code.uiToEngine, ...code.engineToUi, ...code.engineOnlyRequests]);

  if (process.argv.includes('--update')) {
    const previous = fs.existsSync(MANIFEST) ? JSON.parse(fs.readFileSync(MANIFEST, 'utf8')) : {};
    const reasons = (key, names) => Object.fromEntries(names.map((n) => [n, previous[key]?.[n] ?? UNEXPLAINED]));
    const manifest = {
      $comment: 'Every UI↔engine message type. Checked by tools/check-protocol.mjs; see docs/user-interface.md for payloads.',
      uiToEngine: code.uiToEngine,
      engineToUi: code.engineToUi,
      engineOnlyRequests: reasons('engineOnlyRequests', code.engineOnlyRequests),
      sentButUnhandled: reasons('sentButUnhandled', code.sentButUnhandled),
      handledButNeverSent: reasons('handledButNeverSent', code.handledButNeverSent),
      undocumented: sorted([...allTypes].filter((t) => !documented.has(t) && (previous.undocumented ?? [...allTypes]).includes(t))),
    };
    fs.mkdirSync(path.dirname(MANIFEST), { recursive: true });
    fs.writeFileSync(MANIFEST, `${JSON.stringify(manifest, null, 2)}\n`, 'utf8');
    console.log(`[check-protocol] manifest re-pinned: ${manifest.uiToEngine.length} UI→engine, ${manifest.engineToUi.length} engine→UI, ${manifest.undocumented.length} undocumented.`);
    return;
  }

  if (!fs.existsSync(MANIFEST)) {
    console.error('[check-protocol] no manifest. Run: node tools/check-protocol.mjs --update');
    process.exitCode = 1;
    return;
  }
  const manifest = JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
  const failures = [];
  const where = (type) => routes.get(type) ?? sends.get(type) ?? uiSent.get(type) ?? handled.get(type) ?? '';

  for (const type of code.uiSendsUnrouted) failures.push(`UI sends "${type}" (${uiSent.get(type)}) but the engine never routes it, so it is dropped`);

  const compareList = (label, fromCode, pinned) => {
    for (const type of fromCode) if (!pinned.includes(type)) failures.push(`${label}: "${type}" (${where(type)}) is not in the manifest`);
    for (const type of pinned) if (!fromCode.includes(type)) failures.push(`${label}: "${type}" is in the manifest but no longer in the code`);
  };
  compareList('UI→engine', code.uiToEngine, manifest.uiToEngine ?? []);
  compareList('engine→UI', code.engineToUi, manifest.engineToUi ?? []);

  const compareExceptions = (key, label, fromCode) => {
    const pinned = manifest[key] ?? {};
    for (const type of fromCode) {
      if (!(type in pinned)) failures.push(`${label}: "${type}" (${where(type)}) — fix it, or record why in ${key}`);
      else if (!pinned[type] || pinned[type] === UNEXPLAINED) failures.push(`${key}.${type} has no reason recorded`);
    }
    for (const type of Object.keys(pinned)) if (!fromCode.includes(type)) failures.push(`${key}: "${type}" no longer applies — remove it`);
  };
  compareExceptions('engineOnlyRequests', 'the engine routes a type the UI never sends', code.engineOnlyRequests);
  compareExceptions('sentButUnhandled', 'the engine sends a type the UI does not handle', code.sentButUnhandled);
  compareExceptions('handledButNeverSent', 'the UI handles a type the engine never sends', code.handledButNeverSent);

  const backlog = new Set(manifest.undocumented ?? []);
  for (const type of allTypes) if (!documented.has(type) && !backlog.has(type)) failures.push(`"${type}" is not documented in docs/user-interface.md`);
  const nowDocumented = [...backlog].filter((t) => documented.has(t) || !allTypes.has(t));

  for (const problem of effectGuidMismatches()) failures.push(`effect GUID ${problem}`);

  if (failures.length > 0) {
    console.error(`[check-protocol] FAIL — ${failures.length} problem(s):`);
    for (const failure of failures) console.error(`  ${failure}`);
    console.error('\nFix the code or the docs, or re-pin deliberately with: node tools/check-protocol.mjs --update');
    process.exitCode = 1;
    return;
  }

  console.log(`[check-protocol] OK — ${code.uiToEngine.length} UI→engine and ${code.engineToUi.length} engine→UI types agree on both sides; effect GUIDs match.`);
  if (nowDocumented.length > 0) console.log(`Improvement: ${nowDocumented.length} backlog type(s) now documented. Tighten with --update.`);
}

main();
