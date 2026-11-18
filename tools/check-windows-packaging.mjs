#!/usr/bin/env node
/**
 * Windows packaging gate: the VST3 bundle layout and the ARM64 / ARM64EC plugins.
 *
 * A Windows ARM host is one of two kinds of process, and loads only plugins built for its
 * own kind. REAPER's "ARM64" build is ARM64EC (its executable reports an x64 machine type
 * and carries ARM64EC metadata) and rejects a pure ARM64 plugin: the plugin is found, but
 * never registers, so it is missing from the plugin list (issue #62). A native ARM64 host
 * is the reverse. The arm64 installer therefore ships both, as one VST3 bundle with a
 * folder per architecture.
 *
 *   node tools/check-windows-packaging.mjs                 # source gate, needs no toolchain
 *   node tools/check-windows-packaging.mjs --bundle <dir>  # also inspect a built bundle
 *
 * The source gate reads installer.iss, build_windows.bat, build-installer.bat and the
 * WebView2 CMake files and fails when the ARM64EC plumbing is lost, or when a plugin starts
 * depending on a WebView2Loader.dll that no host's DLL search would find beside it.
 *
 * --bundle takes an installed or built `Soundshed Guitar.vst3` folder and checks every
 * Contents/<arch>-win binary: its machine type matches the folder, it exports
 * GetPluginFactory, and every DLL it imports is a Windows system DLL or sits beside it.
 * Use --require-arch to demand specific folders (default: the ones present, at least one).
 *   node tools/check-windows-packaging.mjs --bundle "C:/Program Files/Common Files/VST3/Soundshed Guitar.vst3" --require-arch arm64-win,arm64ec-win
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PRODUCT = 'Soundshed Guitar';

const failures = [];
const fail = (message) => failures.push(message);
const read = (relative) => fs.readFileSync(path.join(ROOT, relative), 'utf8');

// ---------------------------------------------------------------------------
// Source gate
// ---------------------------------------------------------------------------

/** Lines of `text` between the first `startRe` match and the next line matching `endRe`. */
function block(text, startRe, endRe) {
  const lines = text.split(/\r?\n/);
  const start = lines.findIndex((line) => startRe.test(line));
  if (start < 0) return null;
  const end = lines.findIndex((line, i) => i > start && endRe.test(line));
  return lines.slice(start + 1, end < 0 ? lines.length : end);
}

function checkInstaller() {
  const iss = read('juce/packaging/installer.iss');
  const lines = iss.split(/\r?\n/);

  // The Files entries for the ARM64EC build must sit inside the ARM64-only conditional,
  // so the x64, AVX and No-AVX installers never reference a missing builds-arm64ec tree.
  let depth = 0;
  let armDepth = 0;
  for (const line of lines) {
    if (/^#if\b/.test(line)) {
      depth += 1;
      if (/#if\s+TargetArch\s*==\s*"ARM64"/.test(line) && armDepth === 0) armDepth = depth;
    } else if (/^#endif\b/.test(line)) {
      if (depth === armDepth) armDepth = 0;
      depth -= 1;
    } else if (/builds-arm64ec/.test(line) && !line.startsWith(';') && armDepth === 0) {
      fail('installer.iss: a builds-arm64ec source sits outside #if TargetArch == "ARM64"');
    }
  }

  // The text of every #if TargetArch == "ARM64" block (the uninstall and the Files ones).
  const armText = [];
  let capture = false;
  for (const line of lines) {
    if (/^#if TargetArch == "ARM64"\s*$/.test(line)) capture = true;
    else if (capture && /^#endif/.test(line)) capture = false;
    else if (capture) armText.push(line);
  }
  const arm = armText.join('\n');

  const vst3Source = /Source:\s*"\.\.\\builds-arm64ec\\\{#ProjectName\}_artefacts\\Release\\VST3\\\{#ProductName\}\.vst3\\Contents\\arm64ec-win\\\*";\s*DestDir:\s*"\{code:GetVst3Dir\}\\Contents\\arm64ec-win"/;
  if (!vst3Source.test(arm)) {
    fail('installer.iss: the ARM64 installer must install builds-arm64ec ...\\Contents\\arm64ec-win\\* into {code:GetVst3Dir}\\Contents\\arm64ec-win');
  }
  if (!/Components:\s*vst3/.test(arm.split(/\r?\n/).find((l) => l.includes('Contents\\arm64ec-win')) ?? '')) {
    fail('installer.iss: the arm64ec-win VST3 entry must belong to the vst3 component');
  }
  if (!/DestName:\s*"\{#ProductName\} \(ARM64EC\)\.clap"/.test(arm)) {
    fail('installer.iss: the ARM64EC CLAP must install as "{#ProductName} (ARM64EC).clap" beside the ARM64 one');
  }
  if (!/Name:\s*"\{code:GetClapDir\}\\\{#ProductName\} \(ARM64EC\)\.clap"/.test(iss)) {
    fail('installer.iss: uninstall must delete the "(ARM64EC).clap" file');
  }

  // The native bundle is installed whole, which carries Contents/arm64-win (or x86_64-win).
  const nativeVst3 = /Source:\s*"\.\.\\Builds\\\{#ProjectName\}_artefacts\\Release\\VST3\\\{#ProductName\}\.vst3\\\*";\s*DestDir:\s*"\{code:GetVst3Dir\}"[^\r\n]*recursesubdirs/;
  if (!nativeVst3.test(iss)) {
    fail('installer.iss: the native VST3 bundle must be installed whole (recursesubdirs) into {code:GetVst3Dir}');
  }
  if (/^[^;\r\n]*WebView2Loader\.dll/im.test(iss)) {
    fail('installer.iss: WebView2Loader.dll must not be packaged; the loader is linked statically');
  }
}

function checkBuildScripts() {
  const bat = read('build_windows.bat');
  const arm = block(bat, /^if \/I not "%ARCH%"=="ARM64" goto :arm64ec_done/, /^:arm64ec_done/) ?? [];
  const armText = arm.join('\n');
  if (!/-A ARM64EC\b/.test(armText) || !/JUCE_BUILDS_ARM64EC/.test(armText)) {
    fail('build_windows.bat: the arm64 build must configure an ARM64EC tree in %JUCE_BUILDS_ARM64EC%');
  }
  for (const target of ['SoundshedGuitar_VST3', 'SoundshedGuitar_CLAP']) {
    if (!armText.includes(target)) fail(`build_windows.bat: the ARM64EC build must build ${target}`);
  }
  const installerStep = bat.indexOf('call "%INSTALLER_SCRIPT%"');
  const arm64ecDone = bat.indexOf(':arm64ec_done');
  if (arm64ecDone < 0 || installerStep < arm64ecDone) {
    fail('build_windows.bat: the ARM64EC build must finish before the installer is built');
  }
  if (!/set "JUCE_BUILDS_ARM64EC=%WORKSPACE_ROOT%juce\\builds-arm64ec"/.test(bat)) {
    fail('build_windows.bat: JUCE_BUILDS_ARM64EC must be juce\\builds-arm64ec (installer.iss reads it)');
  }

  const installer = read('juce/packaging/build-installer.bat');
  if (!/GUITARFX_WINDOWS_ARCH%"=="ARM64"[\s\S]*builds-arm64ec/.test(installer)) {
    fail('build-installer.bat: an arm64 installer must refuse to build without the ARM64EC plugins');
  }
}

function checkWebView2() {
  const cmake = read('juce/CMakeLists.txt');
  if (!/JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1/.test(cmake)) {
    fail('juce/CMakeLists.txt: WebView2 must be linked statically; a plugin cannot rely on WebView2Loader.dll beside it');
  }
  if (/^[^#\r\n]*WebView2Loader\.dll/im.test(cmake)) {
    fail('juce/CMakeLists.txt: do not copy or reference WebView2Loader.dll');
  }
  for (const arch of ['arm64ec-win', 'arm64-win']) {
    if (!cmake.includes(`"${arch}"`)) fail(`juce/CMakeLists.txt: GUITARFX_WINDOWS_PACKAGE_ARCH lost "${arch}"`);
  }
  const find = read('juce/cmake/FindWebView2.cmake');
  if (!/arm64ec"\)[\s\S]{0,400}set\(_arch "x64"\)/.test(find)) {
    fail('FindWebView2.cmake: ARM64EC must take the x64 static loader (the arm64 one is pure ARM64 objects)');
  }
  if (!/WebView2LoaderStatic\.lib/.test(find) || /WebView2Loader\.dll\.lib/.test(find)) {
    fail('FindWebView2.cmake: link WebView2LoaderStatic.lib only');
  }
}

// ---------------------------------------------------------------------------
// Bundle inspection (a minimal PE reader: headers, imports and exports)
// ---------------------------------------------------------------------------

const MACHINES = { 0x014c: 'x86', 0x8664: 'x64', 0xaa64: 'arm64', 0xa641: 'arm64ec' };
// Contents/<folder> -> machine types a binary there may carry. ARM64EC images are marked
// either ARM64EC (0xA641) or x64 (0x8664) depending on the linker, and carry hybrid metadata.
const FOLDER_MACHINES = {
  'x86-win': ['x86'],
  'x86_64-win': ['x64'],
  'arm64-win': ['arm64'],
  'arm64ec-win': ['arm64ec', 'x64'],
};

// DLLs every Windows install provides. A plugin may import these without shipping them.
const SYSTEM_DLL = /^(api-ms-win-.*|ext-ms-win-.*|kernel32|kernelbase|ntdll|user32|gdi32|gdi32full|advapi32|shell32|shlwapi|ole32|oleaut32|comdlg32|comctl32|combase|ws2_32|wsock32|winmm|version|imm32|rpcrt4|bcrypt|crypt32|setupapi|dwmapi|uxtheme|d3d11|d3d9|d3d10|d3d12|dxgi|d2d1|dwrite|dxguid|winspool\.drv|wininet|wldap32|iphlpapi|netapi32|opengl32|glu32|mmdevapi|avrt|msacm32|mfplat|mfreadwrite|mf|mfuuid|propsys|shcore|secur32|userenv|wtsapi32|psapi|normaliz|ucrtbase|msvcrt|msvcp_win|dbghelp|powrprof|cfgmgr32|dnsapi|mpr|usp10|windowscodecs|urlmon|d3dcompiler_47|oleacc|uiautomationcore|hid|cabinet|msimg32|wevtapi|gdiplus|winhttp|dcomp|dxcore|ncrypt|cryptsp|wintrust|bthprops\.cpl|webservices|xinput\d_\d|xinput9_1_0|dinput8|dsound|shfolder|mscoree|comsvcs|sensapi|credui|samcli|netutils|srvcli|wkscli|msi|clbcatq|normaliz)\.(dll|drv|cpl)$/i;
const CRT_DLL = /^(vcruntime\d+(_\d+)?|msvcp\d+(_\d+)?|concrt\d+|vccorlib\d+|vcomp\d+)\.dll$/i;

/** A DLL the plugin may import: a Windows system DLL (by name, or present in System32 here) or one beside it. */
function isAvailableDll(name, pluginDir) {
  if (SYSTEM_DLL.test(name) || CRT_DLL.test(name)) return true;
  if (fs.existsSync(path.join(pluginDir, name))) return true;
  const systemRoot = process.env.SystemRoot;
  return process.platform === 'win32' && !!systemRoot && fs.existsSync(path.join(systemRoot, 'System32', name));
}

function parsePe(file) {
  const b = fs.readFileSync(file);
  if (b.readUInt16LE(0) !== 0x5a4d) throw new Error('not a PE file (no MZ header)');
  const pe = b.readUInt32LE(0x3c);
  if (b.readUInt32LE(pe) !== 0x4550) throw new Error('not a PE file (no PE signature)');
  const machine = b.readUInt16LE(pe + 4);
  const sectionCount = b.readUInt16LE(pe + 6);
  const optSize = b.readUInt16LE(pe + 20);
  const opt = pe + 24;
  const magic = b.readUInt16LE(opt);
  const plus = magic === 0x20b;
  const dirBase = opt + (plus ? 112 : 96);
  const dir = (index) => ({
    rva: b.readUInt32LE(dirBase + index * 8),
    size: b.readUInt32LE(dirBase + index * 8 + 4),
  });
  const sections = [];
  for (let i = 0; i < sectionCount; i += 1) {
    const s = opt + optSize + i * 40;
    sections.push({
      va: b.readUInt32LE(s + 12),
      virtualSize: b.readUInt32LE(s + 8),
      raw: b.readUInt32LE(s + 20),
      rawSize: b.readUInt32LE(s + 16),
    });
  }
  const offset = (rva) => {
    for (const s of sections) {
      if (rva >= s.va && rva < s.va + Math.max(s.virtualSize, s.rawSize)) return rva - s.va + s.raw;
    }
    throw new Error(`RVA 0x${rva.toString(16)} is outside every section`);
  };
  const cstring = (at) => {
    let end = at;
    while (b[end] !== 0) end += 1;
    return b.toString('latin1', at, end);
  };

  const imports = [];
  const importDir = dir(1);
  if (importDir.rva) {
    for (let at = offset(importDir.rva); b.readUInt32LE(at + 12) !== 0; at += 20) {
      imports.push(cstring(offset(b.readUInt32LE(at + 12))));
    }
  }
  const delayImports = [];
  const delayDir = dir(13);
  if (delayDir.rva) {
    for (let at = offset(delayDir.rva); b.readUInt32LE(at + 4) !== 0; at += 32) {
      delayImports.push(cstring(offset(b.readUInt32LE(at + 4))));
    }
  }
  const exports = [];
  const exportDir = dir(0);
  if (exportDir.rva) {
    const at = offset(exportDir.rva);
    const names = b.readUInt32LE(at + 24);
    const namesRva = b.readUInt32LE(at + 32);
    for (let i = 0; i < names; i += 1) {
      exports.push(cstring(offset(b.readUInt32LE(offset(namesRva) + i * 4))));
    }
  }
  // IMAGE_LOAD_CONFIG_DIRECTORY64.CHPEMetadataPointer (offset 200) is set on ARM64EC and ARM64X
  // images and zero on plain x64 or ARM64 ones.
  let hybrid = false;
  const loadConfig = dir(10);
  if (plus && loadConfig.rva && b.readUInt32LE(offset(loadConfig.rva)) >= 208) {
    hybrid = b.readBigUInt64LE(offset(loadConfig.rva) + 200) !== 0n;
  }
  return { machine: MACHINES[machine] ?? `0x${machine.toString(16)}`, imports, delayImports, exports, hybrid };
}

function checkBundle(bundle, requiredArchs) {
  const contents = path.join(bundle, 'Contents');
  if (!fs.existsSync(contents)) {
    fail(`bundle: ${bundle} has no Contents folder; a VST3 bundle is <name>.vst3/Contents/<arch>-win/<name>.vst3`);
    return;
  }
  const folders = fs.readdirSync(contents).filter((n) => fs.statSync(path.join(contents, n)).isDirectory());
  const archFolders = folders.filter((n) => n in FOLDER_MACHINES);
  for (const wanted of requiredArchs) {
    if (!archFolders.includes(wanted)) fail(`bundle: Contents/${wanted} is missing`);
  }
  if (archFolders.length === 0) fail('bundle: no Contents/<arch>-win folder found');

  for (const folder of archFolders) {
    const dll = path.join(contents, folder, `${PRODUCT}.vst3`);
    const where = `Contents/${folder}/${PRODUCT}.vst3`;
    if (!fs.existsSync(dll)) {
      fail(`bundle: ${where} is missing`);
      continue;
    }
    let pe;
    try {
      pe = parsePe(dll);
    } catch (error) {
      fail(`bundle: ${where}: ${error.message}`);
      continue;
    }
    console.log(
      `  ${where}: machine ${pe.machine}${pe.hybrid ? ' (hybrid ARM64EC metadata)' : ''}, ${pe.imports.length} imports, ${pe.delayImports.length} delay imports`
    );
    if (!FOLDER_MACHINES[folder].includes(pe.machine)) {
      fail(`bundle: ${where} is ${pe.machine}; ${folder} must hold ${FOLDER_MACHINES[folder].join(' or ')}`);
    }
    // An x64-marked image in arm64ec-win is only loadable by an ARM64EC host if it is really
    // ARM64EC; plain x64 code there would load emulated, not in REAPER's ARM64EC process.
    if (folder === 'arm64ec-win' && !pe.hybrid) {
      fail(`bundle: ${where} has no ARM64EC (CHPE) metadata; it is a plain x64 build`);
    }
    if (!pe.exports.includes('GetPluginFactory')) fail(`bundle: ${where} does not export GetPluginFactory`);
    for (const dep of [...pe.imports, ...pe.delayImports]) {
      if (isAvailableDll(dep, path.join(contents, folder))) continue;
      fail(`bundle: ${where} imports ${dep}, which is neither a Windows system DLL nor beside the plugin`);
    }
    // The editor's UI is read from a resources folder beside the binary.
    if (!fs.existsSync(path.join(contents, folder, 'resources', 'ui'))) {
      fail(`bundle: Contents/${folder}/resources/ui is missing; the editor has no UI without it`);
    }
  }
}

// ---------------------------------------------------------------------------

const args = process.argv.slice(2);
const option = (name) => {
  const at = args.indexOf(name);
  return at >= 0 ? args[at + 1] : undefined;
};

checkInstaller();
checkBuildScripts();
checkWebView2();

const bundle = option('--bundle');
if (bundle) {
  console.log(`Inspecting ${bundle}`);
  const required = (option('--require-arch') ?? '').split(',').map((s) => s.trim()).filter(Boolean);
  checkBundle(path.resolve(bundle), required);
}

if (failures.length > 0) {
  console.error(`Windows packaging check failed (${failures.length}):`);
  for (const message of failures) console.error(`  - ${message}`);
  process.exit(1);
}
console.log('Windows packaging check passed.');
