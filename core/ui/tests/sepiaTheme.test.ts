/**
 * Sepia's own theme: it exists, a fresh install opens in it, and the user's stored choice of
 * another theme is still honoured.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";

window.IPlugSendMsg = () => {};

const css = (name: string) => readFileSync(join(__dirname, "..", "css", "themes", name), "utf8");

/** Every `--token:` a theme's own selector block sets. */
function tokens(source: string, selector: string): Set<string> {
  const start = source.indexOf(`${selector} {`);
  const block = source.slice(start, source.indexOf("\n}", start));
  return new Set(Array.from(block.matchAll(/^\s*(--[\w-]+)\s*:/gm), (match) => match[1]));
}

describe("the Sepia theme", () => {
  it("is what a fresh install opens in", async () => {
    const { DEFAULT_THEME, themeSwitcher } = await import("../ts/theme-switcher.js");
    expect(DEFAULT_THEME).toBe("sepia");
    expect(themeSwitcher.getCurrentTheme()).toBe("sepia");
    expect(document.body.classList.contains("theme-sepia")).toBe(true);
    expect(themeSwitcher.getThemeDisplayName()).toBe("Sepia");
  });

  it("gives way to a stored choice, and an unknown one falls back to it", async () => {
    const { onTheme } = await import("../ts/messages/stateHandlers.js");
    const { themeSwitcher } = await import("../ts/theme-switcher.js");
    onTheme({ theme: "light" });
    expect(themeSwitcher.getCurrentTheme()).toBe("light");
    expect(document.body.classList.contains("theme-sepia")).toBe(false);
    onTheme({ theme: "no-such-theme" });
    expect(themeSwitcher.getCurrentTheme()).toBe("sepia");
    onTheme({ theme: "dark" });
    expect(themeSwitcher.getCurrentTheme()).toBe("dark");
  });

  it("sets every token the vintage theme it grew from sets, so nothing falls through to light", () => {
    const sepia = tokens(css("sepia.css"), ".theme-sepia");
    const missing = Array.from(tokens(css("classic.css"), ".theme-classic")).filter((token) => !sepia.has(token));
    expect(sepia.size).toBeGreaterThan(100);
    expect(missing).toEqual([]);
  });

  it("is linked from the page and offered in settings", () => {
    const template = readFileSync(join(__dirname, "..", "index.template.html"), "utf8");
    expect(template).toContain('href="css/themes/sepia.css"');
    expect(readFileSync(join(__dirname, "..", "ts", "settings", "appearance.ts"), "utf8")).toContain('value: "sepia"');
  });
});
