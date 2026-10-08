#!/usr/bin/env node
/**
 * Simple HTML assembler for ui-components.
 * Replaces markers like:
 *   <!--#include:header-icon-bar.html-->
 *   <!--#include:ui-components/control-bar.html-->
 *   <!--#include:main-content/visualizer.html-->
 *
 * Usage:
 *   node scripts/assemble-html.js
 *
 * It reads index.template.html if present, else index.html as input.
 * Writes the result to index.html (in-place for the ui root, matching WebView expectations).
 *
 * Run automatically as part of `npm run build`.
 *
 * It also fills in the Content-Security-Policy: the template's
 * {{CSP_INLINE_SCRIPT_HASHES}} placeholder becomes a 'sha256-…' source for every
 * inline <script> that follows the policy, so an inline script can be edited
 * without touching the policy by hand.
 *
 * And it lists the compiled modules for the browser to fetch up front: the
 * template's <!--#modulepreloads--> marker becomes a <link rel="modulepreload">
 * for every module dist/main.js reaches through static imports. Run it after tsc,
 * as `npm run build` does; with no dist/main.js the marker is left empty.
 */

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const CSP_HASHES_PLACEHOLDER = '{{CSP_INLINE_SCRIPT_HASHES}}';
const MODULE_PRELOADS_MARKER = '<!--#modulepreloads-->';

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const COMPONENTS_DIR = path.join(ROOT, 'ui-components');
const MODULE_ENTRY = path.join(ROOT, 'dist', 'main.js');
const TEMPLATE_CANDIDATES = [
  path.join(ROOT, 'index.template.html'),
  path.join(ROOT, 'index.html'),
];
const OUTPUT = path.join(ROOT, 'index.html');

function findTemplate() {
  for (const p of TEMPLATE_CANDIDATES) {
    if (fs.existsSync(p)) return p;
  }
  throw new Error('No index.template.html or index.html found');
}

function resolveInclude(includePath) {
  // Allow both "foo.html" and "ui-components/foo.html" or "main-content/bar.html"
  const candidates = [
    path.join(COMPONENTS_DIR, includePath),
    path.join(COMPONENTS_DIR, path.basename(includePath)),
    path.join(ROOT, includePath),
  ];
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  return null;
}

function processIncludes(html, seen = new Set()) {
  const re = /<!--\s*#include:\s*([^\s>]+?)\s*-->/g;
  return html.replace(re, (match, includeSpec) => {
    const filePath = resolveInclude(includeSpec);
    if (!filePath) {
      console.warn(`[assemble-html] Warning: include not found: ${includeSpec}`);
      return match;
    }
    const abs = path.resolve(filePath);
    if (seen.has(abs)) {
      console.warn(`[assemble-html] Warning: circular include detected for ${includeSpec}`);
      return '';
    }
    seen.add(abs);
    let content = fs.readFileSync(filePath, 'utf8');
    // Recurse for nested includes
    content = processIncludes(content, seen);
    seen.delete(abs);
    return content;
  });
}

/**
 * The CSP hash source for an inline script's text. The browser hashes the text as
 * the HTML parser leaves it, which has turned every CRLF into LF; the working tree
 * is CRLF on Windows, so hashing the raw file would never match.
 */
function inlineScriptHash(scriptText) {
  const normalized = scriptText.replace(/\r\n?/g, '\n');
  return `'sha256-${crypto.createHash('sha256').update(normalized, 'utf8').digest('base64')}'`;
}

/** Replaces the policy's placeholder with the hashes of the inline scripts after it. */
function applyCspScriptHashes(html) {
  const at = html.indexOf(CSP_HASHES_PLACEHOLDER);
  if (at < 0) {
    return html;
  }
  const hashes = [];
  const inlineScript = /<script(\s[^>]*)?>([\s\S]*?)<\/script>/gi;
  inlineScript.lastIndex = at;
  for (let match = inlineScript.exec(html); match; match = inlineScript.exec(html)) {
    const attributes = match[1] || '';
    if (!/\ssrc\s*=/i.test(attributes)) {
      hashes.push(inlineScriptHash(match[2]));
    }
  }
  return html.slice(0, at) + hashes.join(' ') + html.slice(at + CSP_HASHES_PLACEHOLDER.length);
}

/**
 * The relative specifiers of a compiled module's static imports and re-exports:
 * `import … from "./x.js"`, `import "./x.js"` and `export … from "./x.js"`. A
 * dynamic `import("./x.js")` is left out, since whatever loads that way does so
 * on purpose, later.
 */
function staticImportSpecifiers(source) {
  const statement = /(?:^|[;\n}])\s*(?:import|export)\s*(?:[^'"]*?\sfrom\s*)?["'](\.{1,2}\/[^'"]+)["']/g;
  return Array.from(source.matchAll(statement), (match) => match[1]);
}

/**
 * Every module the entry module reaches through static imports, itself first, as
 * paths relative to `root` (the UI root). Without these the browser finds the graph one
 * level of imports at a time, and each level waits for the one before it to
 * arrive; in the app that is about seven round trips through the WebView's
 * resource handler on the message thread, at roughly 50 ms each.
 */
function collectModuleGraph(entry = MODULE_ENTRY, root = ROOT) {
  if (!fs.existsSync(entry)) {
    return [];
  }

  const seen = new Set([entry]);
  const order = [entry];
  for (let i = 0; i < order.length; i++) {
    const file = order[i];
    for (const specifier of staticImportSpecifiers(fs.readFileSync(file, 'utf8'))) {
      const resolved = path.resolve(path.dirname(file), specifier);
      if (!seen.has(resolved) && fs.existsSync(resolved)) {
        seen.add(resolved);
        order.push(resolved);
      }
    }
  }

  return order.map((file) => path.relative(root, file).split(path.sep).join('/'));
}

/** Replaces the template's marker with a modulepreload link per module. */
function applyModulePreloads(html, modules = collectModuleGraph()) {
  if (!html.includes(MODULE_PRELOADS_MARKER)) {
    return html;
  }
  if (modules.length === 0) {
    console.warn('[assemble-html] Warning: no dist/main.js to preload; run tsc first');
  }
  const links = modules.map((href) => `<link rel="modulepreload" href="${href}" />`).join('\n  ');
  return html.replace(MODULE_PRELOADS_MARKER, () => links);
}

/** The finished index.html text, from the template. */
function assembleHtml() {
  const inputPath = findTemplate();
  const template = fs.readFileSync(inputPath, 'utf8');
  return { inputPath, template, html: applyCspScriptHashes(applyModulePreloads(processIncludes(template))) };
}

function main() {
  const { inputPath, template, html } = assembleHtml();

  // If this was the template, or we did work, write output
  fs.writeFileSync(OUTPUT, html, 'utf8');

  console.log(`[assemble-html] Assembled ${path.basename(inputPath)} -> ${path.relative(ROOT, OUTPUT)} (${template.length} -> ${html.length} bytes)`);
}

module.exports = {
  assembleHtml,
  applyCspScriptHashes,
  applyModulePreloads,
  collectModuleGraph,
  inlineScriptHash,
  staticImportSpecifiers,
  CSP_HASHES_PLACEHOLDER,
  MODULE_PRELOADS_MARKER,
};

if (require.main === module) {
  main();
}
