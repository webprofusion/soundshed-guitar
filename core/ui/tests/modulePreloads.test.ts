/**
 * The modulepreload list scripts/assemble-html.js writes into index.html: every
 * module main.js reaches through static imports, and nothing it loads lazily.
 */
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";

const require = createRequire(import.meta.url);
const { applyModulePreloads, collectModuleGraph, staticImportSpecifiers, MODULE_PRELOADS_MARKER } = require("../scripts/assemble-html.js") as {
  applyModulePreloads: (html: string, modules?: string[]) => string;
  collectModuleGraph: (entry: string, root: string) => string[];
  staticImportSpecifiers: (source: string) => string[];
  MODULE_PRELOADS_MARKER: string;
};

describe("staticImportSpecifiers", () => {
  it("finds imports, side-effect imports and re-exports, across lines", () => {
    const source = [
      'import { a } from "./a.js";',
      "import {",
      "  b,",
      "  c,",
      '} from "./sub/bc.js";',
      'import "./side.js";',
      'export { d } from "../d.js";',
      'export * from "./e.js";',
      'import type_only from "./f.js"; export const g = 1;',
    ].join("\n");

    expect(staticImportSpecifiers(source)).toEqual(["./a.js", "./sub/bc.js", "./side.js", "../d.js", "./e.js", "./f.js"]);
  });

  it("leaves out dynamic imports, bare specifiers and strings that only look like paths", () => {
    const source = [
      'const lazy = () => import("./lazy.js");',
      'import JSZip from "jszip";',
      'export const path = "./not-a-module.js";',
      ' * import { x } from "./in-a-comment.js"',
    ].join("\n");

    expect(staticImportSpecifiers(source)).toEqual([]);
  });
});

describe("collectModuleGraph", () => {
  let root = "";

  afterEach(() => {
    if (root) rmSync(root, { recursive: true, force: true });
  });

  it("lists the entry first, then each module once, cycles included, lazy ones not", () => {
    root = mkdtempSync(join(tmpdir(), "module-preloads-"));
    mkdirSync(join(root, "dist", "sub"), { recursive: true });
    const write = (file: string, text: string) => writeFileSync(join(root, "dist", file), text);
    write("main.js", 'import { a } from "./a.js";\nimport "./sub/b.js";\nconst later = () => import("./lazy.js");\n');
    write("a.js", 'import { b } from "./sub/b.js";\nexport const a = b;\n');
    write("sub/b.js", 'import { a } from "../a.js";\nexport const b = 1;\n');
    write("lazy.js", "export const lazy = 1;\n");

    expect(collectModuleGraph(join(root, "dist", "main.js"), root)).toEqual(["dist/main.js", "dist/a.js", "dist/sub/b.js"]);
  });

  it("is empty when the entry has not been compiled", () => {
    root = mkdtempSync(join(tmpdir(), "module-preloads-"));
    expect(collectModuleGraph(join(root, "dist", "main.js"), root)).toEqual([]);
  });
});

describe("applyModulePreloads", () => {
  it("replaces the template's marker with a link per module", () => {
    const html = `<head>\n  ${MODULE_PRELOADS_MARKER}\n</head>`;
    expect(applyModulePreloads(html, ["dist/main.js", "dist/a.js"])).toBe(
      '<head>\n  <link rel="modulepreload" href="dist/main.js" />\n  <link rel="modulepreload" href="dist/a.js" />\n</head>',
    );
  });
});
