#!/usr/bin/env node
/**
 * The fonts Soundshed Guitar Nano draws with: static Inter Regular, Medium and SemiBold, cut
 * from the web UI's variable Inter (core/ui/css/fonts) into juce/source/nativeui/theme/fonts.
 *
 * JUCE 8 cannot pick a variable font's weight - on Windows it draws Inter's first named
 * instance, Thin - so Nano needs one static file per weight. tools/nano-fonts/
 * make-inter-instances.cpp makes them with HarfBuzz's instancer, compiled here from the copy
 * of HarfBuzz inside juce/JUCE. That copy leaves out the subsetter's repacker, so the build
 * swaps it for a stub that stops the run if a table ever needs repacking.
 *
 * Run it after updating the variable font, then commit the three files:
 *
 *   node tools/gen-nano-fonts.mjs
 *
 * Needs a C++ compiler: MSVC on Windows (found through vswhere), c++ elsewhere.
 */

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const HARFBUZZ = path.join(ROOT, 'juce', 'JUCE', 'modules', 'juce_graphics', 'fonts', 'harfbuzz');
const TOOL = path.join(ROOT, 'tools', 'nano-fonts', 'make-inter-instances.cpp');
const FONTS = path.join(ROOT, 'core', 'ui', 'css', 'fonts');
const SOURCE = path.join(FONTS, 'Inter-VariableFont_opsz,wght.ttf');
const OUT = path.join(ROOT, 'juce', 'source', 'nativeui', 'theme', 'fonts');

function fail(message) {
  console.error(`gen-nano-fonts: ${message}`);
  process.exit(1);
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: 'inherit', ...options });

  if (result.status !== 0)
    fail(`${path.basename(command)} failed (${result.error?.message ?? `exit ${result.status}`})`);
}

/** hb-subset.cc with the repacker include replaced by a stub that ends the run. */
function writeSubsetWithoutRepacker(buildDir) {
  const include = '#include "hb-repacker.hh"';
  const source = fs.readFileSync(path.join(HARFBUZZ, 'hb-subset.cc'), 'utf8');

  if (!source.includes(include))
    fail(`hb-subset.cc no longer includes ${include}; update the stub`);

  const stub = [
    '#include <cstdio>',
    '#include <cstdlib>',
    'template <typename T>',
    'static inline hb_blob_t* hb_resolve_overflows (const T&, hb_tag_t tag, unsigned = 0, bool = false)',
    '{',
    '    std::fprintf (stderr, "%c%c%c%c overflows and needs the repacker, which JUCE\'s HarfBuzz leaves out\\n", HB_UNTAG (tag));',
    '    std::exit (3);',
    '}',
  ].join('\n');

  fs.writeFileSync(path.join(buildDir, 'hb-subset-norepack.cc'), source.replace(include, stub));

  // HarfBuzz's own amalgamation, less the repacker's files and with the stubbed subsetter.
  const units = fs
    .readFileSync(path.join(HARFBUZZ, 'harfbuzz-subset.cc'), 'utf8')
    .split(/\r?\n/)
    .filter((line) => line.startsWith('#include'))
    .filter((line) => !/graph\/|"hb-subset-repacker\.cc"|"hb-subset\.cc"/.test(line));
  units.push('#include "hb-subset-norepack.cc"');
  fs.writeFileSync(path.join(buildDir, 'unity.cc'), `${units.join('\n')}\n`);
}

function findVcvars() {
  const vswhere = path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');

  if (!fs.existsSync(vswhere))
    fail('vswhere.exe not found; install Visual Studio with the C++ tools');

  const found = spawnSync(vswhere, ['-latest', '-prerelease', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' });
  const vcvars = path.join(found.stdout.trim(), 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');

  if (!fs.existsSync(vcvars))
    fail('no Visual Studio with the C++ x64 tools found');

  return vcvars;
}

function build(buildDir) {
  const exe = path.join(buildDir, process.platform === 'win32' ? 'make-inter-instances.exe' : 'make-inter-instances');

  if (process.platform === 'win32') {
    const script = path.join(buildDir, 'build.bat');
    fs.writeFileSync(script, [
      '@echo off',
      `call "${findVcvars()}" >nul 2>&1`,
      `cl /nologo /std:c++17 /EHsc /O2 /DNDEBUG /I "${buildDir}" /I "${HARFBUZZ}" "${TOOL}" "${path.join(buildDir, 'unity.cc')}" /Fo"${buildDir}\\\\" /Fe"${exe}" >"${path.join(buildDir, 'build.log')}"`,
      'if errorlevel 1 (type "' + path.join(buildDir, 'build.log') + '" & exit /b 1)',
    ].join('\r\n'));
    run('cmd.exe', ['/d', '/c', script]);
  } else {
    run(process.env.CXX ?? 'c++', ['-std=c++17', '-O2', '-DNDEBUG', '-I', buildDir, '-I', HARFBUZZ, TOOL,
      path.join(buildDir, 'unity.cc'), '-o', exe]);
  }

  return exe;
}

if (!fs.existsSync(SOURCE))
  fail(`missing ${path.relative(ROOT, SOURCE)}`);

if (!fs.existsSync(path.join(HARFBUZZ, 'harfbuzz-subset.cc')))
  fail('juce/JUCE is not checked out');

const buildDir = fs.mkdtempSync(path.join(os.tmpdir(), 'nano-fonts-'));

try {
  writeSubsetWithoutRepacker(buildDir);
  const exe = build(buildDir);
  fs.mkdirSync(OUT, { recursive: true });
  run(exe, [SOURCE, OUT]);
  fs.copyFileSync(path.join(FONTS, 'OFL.txt'), path.join(OUT, 'OFL.txt'));
} finally {
  fs.rmSync(buildDir, { recursive: true, force: true });
}
