#!/usr/bin/env node
/**
 * The fonts Soundshed Nano Q draws with: static cuts of the web UI's variable fonts
 * (core/ui/css/fonts) into core/nativeui/fonts - Inter, which every theme uses, and the
 * theme fonts (Oswald, Archivo) at the weights their themes set.
 *
 * They began with the JUCE Nano (since removed): JUCE 8 cannot pick a variable font's
 * weight - on Windows it draws Inter's first named instance, Thin - so it needed one static
 * file per weight. Nano Q draws with the same files. tools/nano-fonts/make-font-instance.cpp
 * makes each with HarfBuzz's instancer, compiled here from the copy of HarfBuzz inside
 * juce/JUCE. That copy leaves out the subsetter's repacker, so the build swaps it for a stub
 * that stops the run if a table ever needs repacking. The theme fonts are kept as WOFF2,
 * which tools/nano-fonts/woff2.mjs unpacks first.
 *
 * Run it after updating a variable font or a theme's weights, then commit the files:
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

import { renameFamily } from './nano-fonts/sfnt.mjs';
import { decodeWoff2 } from './nano-fonts/woff2.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const HARFBUZZ = path.join(ROOT, 'juce', 'JUCE', 'modules', 'juce_graphics', 'fonts', 'harfbuzz');
const TOOL = path.join(ROOT, 'tools', 'nano-fonts', 'make-font-instance.cpp');
const FONTS = path.join(ROOT, 'core', 'ui', 'css', 'fonts');
const OUT = path.join(ROOT, 'core', 'nativeui', 'fonts');

/**
 * Each variable font, its licence, the cuts made from it (file name and axis settings) and,
 * where the file names it otherwise, the family name the cuts take. Nano
 * Q asks for a family and weight, and the platform picks the nearest cut, so a theme font needs
 * only the weights its theme draws with (its skin sheet's font-weight rules, and the weights
 * Nano Q's three steps map to: a theme whose body text is 500 draws 500, 600 and 700).
 */
const FAMILIES = [
  {
    source: 'Inter-VariableFont_opsz,wght.ttf',
    licence: 'OFL.txt',
    // Inter's text optical size: Nano Q's text runs 10-20 px, where the display cut is too tight.
    cuts: [
      ['Inter-Regular.ttf', 'wght=400', 'opsz=14'],
      ['Inter-Medium.ttf', 'wght=500', 'opsz=14'],
      ['Inter-SemiBold.ttf', 'wght=600', 'opsz=14'],
    ],
  },
  {
    // Eleven's text and titles. Its axis stops at 700.
    source: 'Oswald-VariableFont_wght.latin.woff2',
    licence: 'Oswald-OFL.txt',
    cuts: [
      ['Oswald-Medium.ttf', 'wght=500'],
      ['Oswald-SemiBold.ttf', 'wght=600'],
      ['Oswald-Bold.ttf', 'wght=700'],
    ],
  },
  {
    // Stage's text, with its names and legends at 800. The subset calls its family "Archivo
    // SemiBold", after its default instance.
    source: 'Archivo-VariableFont_wght.latin.woff2',
    family: 'Archivo',
    licence: 'Archivo-OFL.txt',
    cuts: [
      ['Archivo-Medium.ttf', 'wght=500'],
      ['Archivo-SemiBold.ttf', 'wght=600'],
      ['Archivo-Bold.ttf', 'wght=700'],
      ['Archivo-ExtraBold.ttf', 'wght=800'],
    ],
  },
];

function fail(message) {
  console.error(`gen-nano-fonts: ${message}`);
  process.exit(1);
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: 'inherit', ...options });

  if (result.status !== 0)
    fail(`${path.basename(command)} failed (${result.error?.message ?? `exit ${result.status}`})`);
}

/**
 * HarfBuzz's own amalgamation less the repacker, which JUCE's copy leaves out (no graph/
 * folder): its header's include guard is defined up front, with a stand-in for the one
 * function the subsetter calls from it that ends the run instead.
 */
function writeSubsetWithoutRepacker(buildDir) {
  if (!fs.readFileSync(path.join(HARFBUZZ, 'hb-repacker.hh'), 'utf8').includes('#define HB_REPACKER_HH'))
    fail('hb-repacker.hh no longer has the HB_REPACKER_HH guard; update the stand-in');

  const stub = [
    '#include "hb.hh"',
    '#include <cstdio>',
    '#include <cstdlib>',
    '#define HB_REPACKER_HH',
    'template <typename T>',
    'static inline hb_blob_t* hb_resolve_overflows (const T&, hb_tag_t tag, unsigned = 0, bool = false)',
    '{',
    '    std::fprintf (stderr, "%c%c%c%c overflows and needs the repacker, which JUCE\'s HarfBuzz leaves out\\n", HB_UNTAG (tag));',
    '    std::exit (3);',
    '}',
  ];

  const units = fs
    .readFileSync(path.join(HARFBUZZ, 'harfbuzz-subset.cc'), 'utf8')
    .split(/\r?\n/)
    .filter((line) => line.startsWith('#include'))
    .filter((line) => !/graph\/|"hb-subset-repacker\.cc"/.test(line));
  fs.writeFileSync(path.join(buildDir, 'unity.cc'), `${[...stub, ...units].join('\n')}\n`);
}

/** Copies a text file unless the copy already has its text (line endings aside, which Git sets). */
function copyText(from, to) {
  const text = (file) => fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');

  if (!fs.existsSync(to) || text(from) !== text(to))
    fs.copyFileSync(from, to);
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
  const exe = path.join(buildDir, process.platform === 'win32' ? 'make-font-instance.exe' : 'make-font-instance');

  if (process.platform === 'win32') {
    const script = path.join(buildDir, 'build.bat');
    fs.writeFileSync(script, [
      '@echo off',
      `call "${findVcvars()}" >nul 2>&1`,
      `cl /nologo /std:c++17 /EHsc /bigobj /O2 /DNDEBUG /I "${buildDir}" /I "${HARFBUZZ}" "${TOOL}" "${path.join(buildDir, 'unity.cc')}" /Fo"${buildDir}\\\\" /Fe"${exe}" >"${path.join(buildDir, 'build.log')}"`,
      'if errorlevel 1 (type "' + path.join(buildDir, 'build.log') + '" & exit /b 1)',
    ].join('\r\n'));
    run('cmd.exe', ['/d', '/c', script]);
  } else {
    run(process.env.CXX ?? 'c++', ['-std=c++17', '-O2', '-DNDEBUG', '-I', buildDir, '-I', HARFBUZZ, TOOL,
      path.join(buildDir, 'unity.cc'), '-o', exe]);
  }

  return exe;
}

for (const family of FAMILIES) {
  if (!fs.existsSync(path.join(FONTS, family.source)))
    fail(`missing ${path.relative(ROOT, path.join(FONTS, family.source))}`);
}

if (!fs.existsSync(path.join(HARFBUZZ, 'harfbuzz-subset.cc')))
  fail('juce/JUCE is not checked out');

const buildDir = fs.mkdtempSync(path.join(os.tmpdir(), 'nano-fonts-'));

try {
  writeSubsetWithoutRepacker(buildDir);
  const exe = build(buildDir);
  fs.mkdirSync(OUT, { recursive: true });

  for (const family of FAMILIES) {
    let source = path.join(FONTS, family.source);

    if (source.endsWith('.woff2')) {
      const unpacked = path.join(buildDir, path.basename(source, '.woff2') + '.ttf');
      fs.writeFileSync(unpacked, decodeWoff2(fs.readFileSync(source)));
      source = unpacked;
    }

    for (const [fileName, ...axes] of family.cuts) {
      const cut = path.join(OUT, fileName);
      run(exe, [source, cut, ...axes]);

      if (family.family)
        fs.writeFileSync(cut, renameFamily(fs.readFileSync(cut), family.family));
    }

    copyText(path.join(FONTS, family.licence), path.join(OUT, family.licence));
  }
} finally {
  fs.rmSync(buildDir, { recursive: true, force: true });
}
