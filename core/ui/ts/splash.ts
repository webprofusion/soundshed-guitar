/**
 * Splash Screen Manager
 *
 * Displays a splash screen during app initialization and keeps it on-screen for
 * a minimum duration to avoid startup flashing.
 */

const MIN_SPLASH_MS = 3000;
const THEME_TRANSITION_MS = 260;
const FADE_OUT_MS = 300;

let splashShownAt = 0;

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    window.setTimeout(resolve, ms);
  });
}

/**
 * Hides the splash screen after ensuring a minimum on-screen time.
 * Before fade-out, it briefly transitions from dark startup styling to theme colors.
 */
export async function hideSplashScreen(): Promise<void> {
  const splash = document.getElementById("splash-screen");
  if (!splash) return;

  const elapsed = Math.max(0, performance.now() - splashShownAt);
  const remaining = Math.max(0, MIN_SPLASH_MS - elapsed);
  if (remaining > 0) {
    await delay(remaining);
  }

  // Switch splash palette from startup dark to the user's active theme.
  splash.classList.add("splash-theme-ready");
  await delay(THEME_TRANSITION_MS);

  // Fade-out and remove from DOM.
  splash.classList.add("splash-hidden");
  await delay(FADE_OUT_MS);
  splash.remove();
}

/** Initializes splash timing and ensures startup dark visual state. */
export function initSplashScreen(): void {
  const splash = document.getElementById("splash-screen");
  if (!splash) return;

  splashShownAt = performance.now();
  splash.classList.remove("splash-hidden", "splash-theme-ready");
  splash.style.display = "flex";
}
