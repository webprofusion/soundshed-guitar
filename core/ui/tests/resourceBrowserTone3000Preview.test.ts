import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ResourceBrowserModal } from "../ts/resourceBrowser.js";
import type { PreviewState, ResourceBrowserOptions } from "../ts/resourceBrowser/types.js";
import { uiState } from "../ts/state.js";

/// The modal's own preview state and entry points, reached past `private` for the test.
type PreviewInternals = {
  options: ResourceBrowserOptions | null;
  previewState: PreviewState | null;
  startPreview(toneId: string, modelId: string, modelUrl: string): Promise<void>;
  cancelPreview(restoreOriginal?: boolean): void;
};

describe("Tone3000 preview in the resource browser", () => {
  const originalFetch = globalThis.fetch;
  let sent: Array<Record<string, unknown>>;

  beforeEach(() => {
    sent = [];
    window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);
    uiState.tone3000Session = { accessToken: "session-token", refreshToken: "", expiresAt: Date.now() + 60_000 };
    uiState.appSettings = {
      ...uiState.appSettings,
      "tone3000.apiKey": "test-api-key",
      "tone3000.useSoundshedToneSearchApi": true,
    };
    globalThis.fetch = vi.fn(async () => ({
      ok: true,
      status: 200,
      arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer,
      text: async () => "",
      headers: new Headers({ "content-type": "application/octet-stream" }),
    }) as Response) as typeof fetch;
  });

  afterEach(() => {
    delete window.IPlugSendMsg;
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  function previewingModal(): PreviewInternals {
    const modal = new ResourceBrowserModal() as unknown as PreviewInternals;
    modal.options = { resourceType: "nam", nodeId: "amp", resourceIndex: 0, onSelect: () => {} };
    return modal;
  }

  // A cancel without restore deleted the first temp file and left the node on it, and the
  // engine then took that file for the node's original, so closing restored a missing file.
  it("starts the next preview without cancelling the one playing", async () => {
    const modal = previewingModal();

    await modal.startPreview("1", "a", "https://tone3000.test/models/a.nam");
    await modal.startPreview("1", "b", "https://tone3000.test/models/b.nam");

    expect(sent.map((message) => message.type)).toEqual(["previewRemoteResource", "previewRemoteResource"]);
    expect(sent[1]).toMatchObject({ nodeId: "amp", resourceIndex: 0, tempResourceId: "preview:tone3000:1:b" });
    expect(modal.previewState).toMatchObject({ active: true, modelId: "b" });
  });

  it("keeps the playing preview when the next download fails, so closing still restores", async () => {
    const modal = previewingModal();
    await modal.startPreview("1", "a", "https://tone3000.test/models/a.nam");
    globalThis.fetch = vi.fn(async () => ({ ok: false, status: 404 }) as Response) as typeof fetch;

    await modal.startPreview("1", "b", "https://tone3000.test/models/b.nam");
    modal.cancelPreview();

    expect(sent.map((message) => message.type)).toEqual(["previewRemoteResource", "cancelPreviewResource"]);
    expect(sent[1]).toMatchObject({ restoreOriginal: true });
  });
});
