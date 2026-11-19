import { beforeEach, describe, expect, it, vi } from "vitest";

// Persisting a theme goes through the bridge; these tests are about the classes on the body.
const postMessage = vi.fn();
vi.mock("../ts/bridge.js", () => ({ postMessage }));

async function loadThemeSwitcher() {
  vi.resetModules();
  return import("../ts/theme-switcher.js");
}

function themeClasses(): string[] {
  return [...document.body.classList].filter((name) => name.startsWith("theme-")).sort();
}

describe("themeSwitcher", () => {
  beforeEach(() => {
    document.body.className = "is-standalone";
    postMessage.mockClear();
  });

  it("starts on the dark theme and leaves other body classes alone", async () => {
    await loadThemeSwitcher();
    expect(themeClasses()).toEqual(["theme-dark"]);
    expect(document.body.classList.contains("is-standalone")).toBe(true);
  });

  it("gives each plain theme its one class", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    themeSwitcher.applyTheme("light");
    expect(themeClasses()).toEqual(["theme-light"]);
    themeSwitcher.applyTheme("classic");
    expect(themeClasses()).toEqual(["theme-classic"]);
  });

  // The studio themes are the light theme drawn as hardware, so the light rules have to keep matching.
  it("puts studio on top of the light theme", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    themeSwitcher.applyTheme("studio");
    expect(themeClasses()).toEqual(["theme-light", "theme-studio"]);
    expect(themeSwitcher.getCurrentTheme()).toBe("studio");
  });

  it("puts studio 2003 on top of the light theme, without the new studio's class", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    themeSwitcher.applyTheme("studio-2003");
    expect(themeClasses()).toEqual(["theme-light", "theme-studio-2003"]);
    themeSwitcher.applyTheme("studio");
    expect(themeClasses()).toEqual(["theme-light", "theme-studio"]);
  });

  it("drops both studio classes when leaving it", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    themeSwitcher.applyTheme("studio");
    themeSwitcher.applyTheme("dark");
    expect(themeClasses()).toEqual(["theme-dark"]);
    themeSwitcher.applyTheme("studio-2003");
    themeSwitcher.applyTheme("light");
    expect(themeClasses()).toEqual(["theme-light"]);
  });

  // Stage is the dark theme restyled, so the dark rules have to keep matching under it.
  it("puts stage on top of the dark theme, and drops its class when leaving it", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    themeSwitcher.applyTheme("stage");
    expect(themeClasses()).toEqual(["theme-dark", "theme-stage"]);
    expect(themeSwitcher.getCurrentTheme()).toBe("stage");
    themeSwitcher.applyTheme("dark");
    expect(themeClasses()).toEqual(["theme-dark"]);
  });

  it("puts eleven (high-gain) on top of the dark theme, without stage's class", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    themeSwitcher.applyTheme("stage");
    themeSwitcher.applyTheme("high-gain");
    expect(themeClasses()).toEqual(["theme-dark", "theme-high-gain"]);
    expect(themeSwitcher.getCurrentTheme()).toBe("high-gain");
    themeSwitcher.applyTheme("light");
    expect(themeClasses()).toEqual(["theme-light"]);
  });

  it("announces the change with the theme's own name", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    const seen: unknown[] = [];
    window.addEventListener("themeChanged", (event) => seen.push((event as CustomEvent).detail.theme), { once: true });
    themeSwitcher.applyTheme("studio");
    expect(seen).toEqual(["studio"]);
  });

  it("cycles through every theme the pickers list", async () => {
    const { themeSwitcher, THEME_OPTIONS } = await loadThemeSwitcher();
    themeSwitcher.applyTheme(THEME_OPTIONS[0].value);
    const visited = THEME_OPTIONS.map(() => {
      const current = themeSwitcher.getCurrentTheme();
      themeSwitcher.cycleTheme();
      return current;
    });
    expect(visited).toEqual(THEME_OPTIONS.map((option) => option.value));
    expect(themeSwitcher.getCurrentTheme()).toBe(THEME_OPTIONS[0].value);
  });

  it("names every theme", async () => {
    const { themeSwitcher } = await loadThemeSwitcher();
    expect(themeSwitcher.getThemeDisplayName("studio")).toBe("Studio");
    expect(themeSwitcher.getThemeDisplayName("studio-2003")).toBe("Studio 2003");
    expect(themeSwitcher.getThemeDisplayName("classic")).toBe("Vintage");
    expect(themeSwitcher.getThemeDisplayName("stage")).toBe("Stage");
    expect(themeSwitcher.getThemeDisplayName("high-gain")).toBe("Eleven");
  });
});

describe("normalizeThemeName", () => {
  it("keeps the themes this build has", async () => {
    const { normalizeThemeName } = await loadThemeSwitcher();
    for (const theme of ["light", "dark", "classic", "stage", "high-gain", "studio", "studio-2003"]) {
      expect(normalizeThemeName(theme)).toBe(theme);
    }
  });

  it("maps retired and unknown names to dark", async () => {
    const { normalizeThemeName } = await loadThemeSwitcher();
    expect(normalizeThemeName("gritty")).toBe("dark");
    expect(normalizeThemeName("default")).toBe("dark");
    expect(normalizeThemeName("neon")).toBe("dark");
    expect(normalizeThemeName(undefined)).toBe("dark");
    expect(normalizeThemeName(7)).toBe("dark");
  });
});
