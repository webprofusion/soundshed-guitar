/**
 * Reads ts/themes.ts, the list of themes, for the build scripts and tools.
 *
 * Node strips the file's type annotations itself (Node 22.18+). It warns that it had to guess the
 * file is an ES module, since package.json says no type; that one warning is dropped here.
 */

const path = require('path');

const MANIFEST = path.join(__dirname, '..', 'ts', 'themes.ts');

function loadThemes() {
  const emit = process.emitWarning;
  process.emitWarning = (warning, ...rest) => {
    const code = typeof rest[0] === 'object' ? rest[0]?.code : rest[1];
    if (code === 'MODULE_TYPELESS_PACKAGE_JSON') return;
    emit.call(process, warning, ...rest);
  };
  try {
    return require(MANIFEST);
  } finally {
    process.emitWarning = emit;
  }
}

module.exports = { loadThemes, MANIFEST };
