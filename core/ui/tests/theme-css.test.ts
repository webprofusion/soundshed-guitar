// tools/theme-css.mjs resolves the themes' custom properties for the native UI's generated
// tokens (tools/gen-native-theme.mjs). It was checked against the browser's own resolution of
// every custom property under all seven themes; these pin the rules it follows.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { afterAll, describe, expect, it } from "vitest";

// @ts-expect-error -- a plain ES module with no type declarations
import * as themeCssModule from "../../../tools/theme-css.mjs";

interface ThemeCss {
  value: (themeId: string, prop: string) => string | undefined;
}
type Rgba = [number, number, number, number];
interface ThemeCssModule {
  parseColour: (value: string) => Rgba;
  toArgb: (colour: Rgba) => number;
  loadThemeCss: (cssDir: string, sheets: string[], themes: unknown[]) => ThemeCss;
}
const { parseColour, toArgb, loadThemeCss } = themeCssModule as ThemeCssModule;

const hex = (value: string) => toArgb(parseColour(value)).toString(16).padStart(8, "0");

describe("theme-css colours", () => {
  it("reads hex, rgb() and rgba() with commas or spaces", () => {
    expect(hex("#1e1e26")).toBe("ff1e1e26");
    expect(hex("#fff8")).toBe("88ffffff");
    expect(hex("rgba(255, 255, 255, 0.07)")).toBe("12ffffff");
    expect(hex("rgb(20 24 34 / 50%)")).toBe("80141822");
  });

  it("mixes as CSS Color 5 does: premultiplied, and scaled when the percentages sum under 100%", () => {
    expect(hex("color-mix(in srgb, #ff0000 50%, transparent)")).toBe("80ff0000");
    expect(hex("color-mix(in srgb, #ff0000 25%, #0000ff)")).toBe("ff4000bf");
    expect(hex("color-mix(in srgb, #ff0000 20%, #0000ff 20%)")).toBe("66800080");
  });

  it("takes a gradient by its first colour stop", () => {
    expect(hex("linear-gradient(180deg, #2b2c31 0%, #1c1d20 100%)")).toBe("ff2b2c31");
  });
});

describe("theme-css cascade", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "theme-css-"));
  afterAll(() => fs.rmSync(dir, { recursive: true, force: true }));
  fs.mkdirSync(path.join(dir, "themes"));
  fs.writeFileSync(
    path.join(dir, "variables.css"),
    ":root { --bg: #000000; --bg-derived: var(--bg); --ink: #111111; }"
  );
  fs.writeFileSync(path.join(dir, "themes", "dark.css"), ".theme-dark { --bg: #222222; --ink: var(--bg); }");
  fs.writeFileSync(path.join(dir, "themes", "stage.css"), ".theme-stage { --bg: #333333; }");
  fs.writeFileSync(
    path.join(dir, "panel.css"),
    ".panel { color: var(--panel-ink, var(--ink)); }\n.theme-stage { --panel-only: #444444; }"
  );
  const themes = [
    { id: "dark", base: "dark", classes: ["theme-dark"], palette: "themes/dark.css" },
    { id: "stage", base: "dark", classes: ["theme-dark", "theme-stage"], palette: "themes/stage.css" },
  ];
  const css = loadThemeCss(dir, ["variables.css", "themes/dark.css", "themes/stage.css", "panel.css"], themes);

  it("resolves a :root value at :root, not under the theme", () => {
    expect(css.value("dark", "--bg-derived")).toBe("#000000");
  });

  it("layers a variant over its base palette, resolving under the body", () => {
    expect(css.value("dark", "--ink")).toBe("#222222");
    expect(css.value("stage", "--ink")).toBe("#333333");
  });

  it("falls back to a component token's var() fallback, evaluated under the theme", () => {
    expect(css.value("stage", "--panel-ink")).toBe("#333333");
    expect(css.value("stage", "--panel-only")).toBe("#444444");
    expect(css.value("dark", "--panel-only")).toBeUndefined();
  });
});
