import { existsSync, readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

import { DEFAULT_THEME, LEGACY_THEME_NAMES, THEMES } from "../ts/themes.js";

const uiRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const require = createRequire(import.meta.url);
const { applyThemeStylesheets, themeStylesheets, THEME_PALETTES_MARKER, THEME_VARIANTS_MARKER } = require(
  "../scripts/assemble-html.js",
) as {
  applyThemeStylesheets: (html: string) => string;
  themeStylesheets: () => { palettes: string[]; variants: string[] };
  THEME_PALETTES_MARKER: string;
  THEME_VARIANTS_MARKER: string;
};

const ids = THEMES.map((theme) => theme.id);

// The rules the rest of the app takes on trust from ts/themes.ts.
describe("the theme list", () => {
  it("names each theme once", () => {
    expect(new Set(ids).size).toBe(ids.length);
  });

  it("knows the default and every legacy name's theme", () => {
    expect(ids).toContain(DEFAULT_THEME);
    for (const theme of Object.values(LEGACY_THEME_NAMES)) {
      expect(ids).toContain(theme);
    }
  });

  it("builds each theme on a palette, keeping that palette's class first", () => {
    for (const theme of THEMES) {
      const base = THEMES.find((entry) => entry.id === theme.base);
      expect(base, theme.id).toBeDefined();
      expect(base!.base, `${theme.id}'s base is itself a variant`).toBe(base!.id);
      expect(theme.classes[0]).toBe(base!.classes[0]);
      expect(theme.classes.at(-1)).toBe(`theme-${theme.id}`);
    }
  });

  it("shares a palette's light or dark", () => {
    for (const theme of THEMES) {
      expect(theme.scheme).toBe(THEMES.find((entry) => entry.id === theme.base)!.scheme);
    }
  });

  it("lists stylesheets and flyout pictures that exist", () => {
    for (const theme of THEMES) {
      for (const sheet of [theme.palette, ...theme.skin]) {
        expect(existsSync(join(uiRoot, "css", sheet)), sheet).toBe(true);
      }
      expect(existsSync(join(uiRoot, "images", "themes", "thumbnails", `${theme.id}.jpg`)), theme.id).toBe(true);
    }
  });
});

/** The top-level blocks of a stylesheet: each one's selector and its declarations' property names. */
function topLevelBlocks(css: string): { selector: string; properties: string[] }[] {
  const text = css.replace(/\/\*[\s\S]*?\*\//g, "");
  const blocks: { selector: string; properties: string[] }[] = [];
  let at = 0;
  while (at < text.length) {
    const open = text.indexOf("{", at);
    if (open < 0) break;
    let depth = 1;
    let close = open + 1;
    for (; close < text.length && depth > 0; close++) {
      if (text[close] === "{") depth++;
      else if (text[close] === "}") depth--;
    }
    const body = text.slice(open + 1, close - 1);
    blocks.push({
      selector: text.slice(at, open).trim(),
      properties: body
        .split(";")
        .map((declaration) => declaration.split(":")[0].trim())
        .filter(Boolean),
    });
    at = close;
  }
  return blocks;
}

// The palette is what the native UI reads and what a new palette has to set; everything else is skin.
describe("a theme's palette sheet", () => {
  for (const theme of THEMES) {
    it(`${theme.id}'s holds nothing but tokens on its own class`, () => {
      const css = readFileSync(join(uiRoot, "css", theme.palette), "utf8");
      const ownClass = theme.classes.at(-1)!;
      const blocks = topLevelBlocks(css);
      expect(blocks.length).toBeGreaterThan(0);
      for (const block of blocks) {
        for (const selector of block.selector.split(",")) {
          expect(selector.trim(), `${theme.palette}: ${block.selector}`).toMatch(
            new RegExp(`^(body)?(\\.(${theme.classes.join("|")}))*\\.${ownClass}(\\.(${theme.classes.join("|")}))*$`),
          );
        }
        for (const property of block.properties) {
          expect(property, `${theme.palette}: ${block.selector}`).toMatch(/^--/);
        }
      }
    });
  }
});

describe("the themes' stylesheets in index.html", () => {
  it("links the palettes' sheets at one marker and the variants' at the other", () => {
    const html = applyThemeStylesheets(`a\n  ${THEME_PALETTES_MARKER}\n  b\n  ${THEME_VARIANTS_MARKER}\n`);
    const { palettes, variants } = themeStylesheets();
    expect(palettes).toEqual([
      "css/themes/light.css",
      "css/themes/light-skin.css",
      "css/themes/dark.css",
      "css/themes/dark-skin.css",
      "css/themes/classic.css",
      "css/themes/classic-skin.css",
    ]);
    expect(variants.length).toBeGreaterThan(0);
    const order = [...html.matchAll(/href="([^"]+)"/g)].map((match) => match[1]);
    expect(order).toEqual([...palettes, ...variants]);
    expect(html.indexOf(palettes.at(-1)!)).toBeLessThan(html.indexOf("b"));
    expect(html.indexOf("b")).toBeLessThan(html.indexOf(variants[0]));
  });
});
