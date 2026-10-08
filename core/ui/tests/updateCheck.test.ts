/**
 * The update check, from the server's answer to what the user sees: the header's
 * update button, and the dialog it opens. Uses the real header and dialog markup.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

window.IPlugSendMsg = () => {};

const fragment = (path: string) => readFileSync(join(__dirname, "..", "ui-components", path), "utf8");

const originalFetch = globalThis.fetch;

function answer(latestVersion: string, downloadUrl = "https://guitar.soundshed.com/download") {
  return vi.fn(async (_url: RequestInfo | URL, _init?: RequestInit) => new Response(JSON.stringify({
    ok: true,
    data: {
      is_update_available: true,
      latest_version: latestVersion,
      download_url: downloadUrl,
      release_notes: "## What's new\n- Better tones",
    },
  }), { status: 200 }));
}

/** A fresh module graph each time: the check latches once per session. */
async function load(appSettings: Record<string, unknown> = { "app.instanceId": "test-instance" }) {
  vi.resetModules();
  const { uiState } = await import("../ts/state.js");
  const { triggerUpdateCheck } = await import("../ts/updateCheck.js");
  uiState.appSettings = appSettings as typeof uiState.appSettings;
  uiState.environment = { standalone: true, audioDeviceSettings: false, version: "1.4.0", os: "Windows", cpu: "x64" };
  return { uiState, triggerUpdateCheck };
}

const headerButton = () => document.getElementById("header-update-btn") as HTMLButtonElement;
const modal = () => document.getElementById("update-modal") as HTMLElement;

beforeEach(() => {
  vi.useFakeTimers();
  document.body.innerHTML = fragment("header-icon-bar.html") + fragment("modals/update-modal.html");
});

afterEach(() => {
  vi.useRealTimers();
  globalThis.fetch = originalFetch;
  document.body.innerHTML = "";
});

describe("update check", () => {
  it("shows the header button for a newer release, and it opens the dialog", async () => {
    const fetchMock = answer("1.5.0");
    globalThis.fetch = fetchMock;
    const { triggerUpdateCheck } = await load();

    expect(headerButton().hidden).toBe(true);
    triggerUpdateCheck();
    await vi.advanceTimersByTimeAsync(5000);
    await vi.waitFor(() => expect(headerButton().hidden).toBe(false));

    const body = JSON.parse(String(fetchMock.mock.calls[0]?.[1]?.body)) as Record<string, unknown>;
    expect(body).toMatchObject({ current_version: "1.4.0", os: "Windows", instance_id: "test-instance" });
    expect(document.getElementById("header-update-version")?.textContent).toBe("1.5.0");

    expect(modal().style.display).toBe("none");
    headerButton().click();
    expect(modal().style.display).toBe("flex");
    expect(document.getElementById("update-modal-version")?.textContent).toBe("1.5.0");
    expect(document.getElementById("update-modal-current")?.textContent).toBe("1.4.0");
    expect(document.getElementById("update-modal-notes")?.querySelector("h2")?.textContent).toBe("What's new");
    expect(document.getElementById("update-modal-download")?.getAttribute("href")).toBe("https://guitar.soundshed.com/download");

    (document.getElementById("update-modal-later") as HTMLButtonElement).click();
    expect(modal().style.display).toBe("none");
  });

  it("stays quiet when the server's latest is not newer", async () => {
    globalThis.fetch = answer("1.4.0");
    const { uiState, triggerUpdateCheck } = await load();

    triggerUpdateCheck();
    await vi.advanceTimersByTimeAsync(5000);

    expect(globalThis.fetch).toHaveBeenCalledTimes(1);
    expect(headerButton().hidden).toBe(true);
    expect(uiState.availableUpdate ?? null).toBeNull();
  });

  it("never links the download to anything but http(s)", async () => {
    globalThis.fetch = answer("1.5.0", "javascript:alert(1)");
    const { uiState, triggerUpdateCheck } = await load();

    triggerUpdateCheck();
    await vi.advanceTimersByTimeAsync(5000);
    await vi.waitFor(() => expect(headerButton().hidden).toBe(false));

    expect(uiState.availableUpdate?.downloadUrl).toBe("");
    headerButton().click();
    const download = document.getElementById("update-modal-download") as HTMLAnchorElement;
    expect(download.getAttribute("href")).toBe("#");
    expect(download.style.display).toBe("none");
  });

  it("checks once the setting is turned back on, not only at the next launch", async () => {
    globalThis.fetch = answer("1.5.0");
    const { uiState, triggerUpdateCheck } = await load({ "app.instanceId": "test-instance", "app.updateCheckEnabled": false });

    triggerUpdateCheck();
    await vi.advanceTimersByTimeAsync(5000);
    expect(globalThis.fetch).not.toHaveBeenCalled();

    uiState.appSettings["app.updateCheckEnabled"] = true;
    triggerUpdateCheck();
    await vi.advanceTimersByTimeAsync(5000);
    expect(globalThis.fetch).toHaveBeenCalledTimes(1);
  });
});
