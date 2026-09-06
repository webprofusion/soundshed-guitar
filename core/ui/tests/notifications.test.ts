import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

// notifications.ts grabs its element at import time, so the DOM has to exist
// before the module is loaded and the module has to be re-imported per test.
async function loadWithToastElement() {
  document.body.innerHTML = `<div id="notification-area" class="notification-toast"></div>`;
  vi.resetModules();
  const mod = await import("../ts/notifications.js");
  return { ...mod, toast: document.getElementById("notification-area") as HTMLElement };
}

describe("showNotification", () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
    document.body.innerHTML = "";
  });

  it("shows a bare message and auto-dismisses it", async () => {
    const { showNotification, toast } = await loadWithToastElement();
    showNotification("Layout saved");
    expect(toast.textContent).toBe("Layout saved");
    expect(toast.classList.contains("visible")).toBe(true);
    vi.advanceTimersByTime(4000);
    expect(toast.textContent).toBe("");
    expect(toast.classList.contains("visible")).toBe(false);
  });

  it("appends a detail string after a colon", async () => {
    const { showNotification, toast } = await loadWithToastElement();
    showNotification("Failed to apply preset", "disk full");
    expect(toast.textContent).toBe("Failed to apply preset: disk full");
  });

  it("does not render a severity word passed as the detail", async () => {
    const { showNotification, toast } = await loadWithToastElement();
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    showNotification("Layout saved", "success");
    expect(toast.textContent).toBe("Layout saved");
    expect(warn).toHaveBeenCalledTimes(1);
  });
});
