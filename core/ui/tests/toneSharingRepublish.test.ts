// @vitest-environment jsdom
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types.js";
import type { ToneSharingItem, ToneSharingPack } from "../ts/toneSharingPanel/types.js";

/**
 * Publishing a preset or pack again updates what is already on Tone Sharing instead of making a
 * second copy: the real publish and pack-save code, with the network and dialogs stubbed.
 */

const apiFetch = vi.fn();
const showConfirm = vi.fn();

vi.mock("../ts/toneSharingPanel/api.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  apiFetch,
  toneSharingFetch: async () => new Response(JSON.stringify({ ok: true }), { status: 200 }),
}));
vi.mock("../ts/dialogs.js", () => ({ showConfirm, showPrompt: vi.fn(), showAlert: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));
vi.mock("../ts/toneSharingPanel/consent.js", () => ({ ensurePublishConsent: async () => undefined }));
vi.mock("../ts/toneSharingPanel/shareLinks.js", () => ({ copyToneSharingShareLink: async () => "https://example.invalid/share" }));
vi.mock("../ts/toneSharingPanel/browse.js", () => ({ loadBrowse: vi.fn(), loadMine: vi.fn() }));
vi.mock("../ts/toneSharingPanel/hostActions.js", () => ({
  getToneSharingHostActions: () => ({
    buildToneSharingPresetArchiveBlobs: async () => ({ publicBlob: new Blob(["pub"]), privateBlob: new Blob(["priv"]) }),
  }),
}));

const { findPublishedItemForPreset } = await import("../ts/toneSharingPanel/itemUpload.js");
const { uploadAndPublishItem, openToneSharingPublishPresetModal } = await import("../ts/toneSharingPanel/publish.js");
const { savePack, openPackModal } = await import("../ts/toneSharingPanel/packEditor.js");
const { toneSharingState } = await import("../ts/toneSharingPanel/state.js");
const { uiState } = await import("../ts/state.js");

const modals = ["tone-sharing-publish-modal.html", "tone-sharing-pack-modal.html"]
  .map((file) => readFileSync(join(__dirname, "..", "ui-components", "modals", file), "utf8"))
  .join("");

function preset(id: string, name: string): Preset {
  return {
    id,
    name,
    graph: { nodes: [{ id: "in", type: "input" }, { id: "amp", type: "amp" }, { id: "out", type: "output" }], edges: [] },
  } as unknown as Preset;
}

const item = (id: string, title: string, extra: Partial<ToneSharingItem> = {}): ToneSharingItem => ({ id, title, type: "preset", moderationStatus: "approved", ...extra });

/** Answers the API: the user's items and packs, uploads, and records every write. */
function serve(options: { items?: ToneSharingItem[]; packs?: ToneSharingPack[] } = {}) {
  apiFetch.mockImplementation(async (path: string, init?: RequestInit) => {
    const method = init?.method ?? "GET";
    if (path === "/items/me/list") return { items: options.items ?? [] };
    if (path === "/packs/me/list") return { packs: options.packs ?? [] };
    if (path === "/uploads/init") return { uploadId: "up" };
    if (path === "/uploads/complete") return { assetId: "asset" };
    if (path === "/items" && method === "POST") return { item: item("itm-new", "New", { moderationStatus: "draft" }), updated: false };
    if (path.startsWith("/items/") && method === "PATCH") return { item: item(path.split("/")[2], "Updated") };
    if (path.endsWith("/publish")) return { moderationStatus: "approved" };
    if (path === "/packs" && method === "POST") return { pack: { id: "pak-new", title: "New" } };
    if (path.startsWith("/packs/") && !init) return { pack: { id: path.split("/")[2], title: "Blues" }, items: [] };
    return {};
  });
}

const writes = () => apiFetch.mock.calls
  .filter(([path, init]) => init?.method && !String(path).startsWith("/uploads"))
  .map(([path, init]) => `${init.method} ${path}`);
const bodyOf = (method: string, path: string) => JSON.parse(apiFetch.mock.calls.find(([p, init]) => p === path && init?.method === method)![1].body);
const status = () => document.getElementById("tone-sharing-publish-modal-status")?.textContent ?? document.getElementById("tone-sharing-upload-status")?.textContent;

beforeEach(() => {
  document.body.innerHTML = `${modals}<div id="tone-sharing-upload-status"></div><div id="tone-sharing-feed"></div>`;
  toneSharingState.user = { id: "u1", email: "u@example.com", role: "user" };
  toneSharingState.myItems = [];
  const active = preset("local-1", "Crunch");
  uiState.presetCache.set(active.id, active);
  uiState.activePresetId = active.id;
});

afterEach(() => {
  apiFetch.mockReset();
  showConfirm.mockReset();
  toneSharingState.user = null;
  uiState.activePresetId = null;
  uiState.presetCache.clear();
});

describe("finding the item a preset was published as", () => {
  it("prefers the item keyed to the preset, then an unkeyed one with its title, live first", () => {
    const keyed = item("i1", "Old Name", { sourcePresetId: "local-1" });
    expect(findPublishedItemForPreset(preset("local-1", "Crunch"), [item("i2", "Crunch"), keyed])).toEqual({ item: keyed, match: "preset" });
    const draft = item("i3", "crunch", { moderationStatus: "draft" });
    const live = item("i4", " Crunch ");
    expect(findPublishedItemForPreset(preset("local-2", "Crunch"), [draft, live])).toEqual({ item: live, match: "title" });
    // An item keyed to some other preset is never a guess for this one.
    expect(findPublishedItemForPreset(preset("local-2", "Crunch"), [item("i5", "Crunch", { sourcePresetId: "x" })])).toBeNull();
  });
});

describe("publishing a preset again", () => {
  it("creates a new item keyed to the preset when there is none", async () => {
    serve();
    await uploadAndPublishItem();
    expect(writes()).toEqual(["POST /items", "POST /items/itm-new/publish"]);
    expect(bodyOf("POST", "/items")).toMatchObject({ title: "Crunch", sourcePresetId: "local-1" });
    expect(showConfirm).not.toHaveBeenCalled();
  });

  it("updates the item published from this preset, without asking", async () => {
    serve({ items: [item("itm-1", "Crunch", { sourcePresetId: "local-1" })] });
    toneSharingState.myItems = [item("itm-1", "Crunch", { sourcePresetId: "local-1" })];
    openToneSharingPublishPresetModal();
    expect(document.getElementById("tone-sharing-upload-item")?.textContent).toBe("Update Published Preset");

    await uploadAndPublishItem();
    expect(writes()).toEqual(["PATCH /items/itm-1", "POST /items/itm-1/publish"]);
    expect(bodyOf("PATCH", "/items/itm-1")).toMatchObject({ payloadAssetId: "asset", privatePayloadAssetId: "asset", sourcePresetId: "local-1" });
    expect(showConfirm).not.toHaveBeenCalled();
    expect(status()).toContain("Published preset updated.");
  });

  it("asks before updating an older item that only shares the name, and publishes nothing if told no", async () => {
    serve({ items: [item("itm-old", "Crunch")] });
    showConfirm.mockResolvedValueOnce(false);
    await uploadAndPublishItem();
    expect(showConfirm).toHaveBeenCalledTimes(1);
    expect(writes()).toEqual([]);
    expect(status()).toBe("Not published.");

    showConfirm.mockResolvedValueOnce(true);
    await uploadAndPublishItem();
    expect(writes()).toEqual(["PATCH /items/itm-old", "POST /items/itm-old/publish"]);
  });
});

describe("saving a pack again", () => {
  function fillPack(title: string) {
    (document.getElementById("tone-sharing-pack-title") as HTMLInputElement).value = title;
    document.getElementById("tone-sharing-pack-items")!.innerHTML = `<input type="checkbox" data-pack-item-id="itm-1" checked />`;
  }

  it("updates the user's pack with the same title once they agree, instead of making another", async () => {
    serve({ packs: [{ id: "pak-1", title: "Blues", moderationStatus: "approved" }] });
    await openPackModal();
    fillPack(" blues ");
    showConfirm.mockResolvedValueOnce(true);
    await savePack(true);
    expect(writes()).toEqual(["PATCH /packs/pak-1", "POST /packs/pak-1/items", "POST /packs/pak-1/publish"]);
    expect(document.getElementById("tone-sharing-pack-status")?.textContent).toBe("Published pack updated.");
  });

  it("saves nothing when the user says no", async () => {
    serve({ packs: [{ id: "pak-1", title: "Blues", moderationStatus: "approved" }] });
    await openPackModal();
    fillPack("Blues");
    showConfirm.mockResolvedValueOnce(false);
    await savePack(true);
    expect(writes()).toEqual([]);
  });

  it("creates a pack when the title is new", async () => {
    serve({ packs: [{ id: "pak-1", title: "Blues", moderationStatus: "approved" }] });
    await openPackModal();
    fillPack("Rock");
    await savePack(false);
    expect(writes()).toEqual(["POST /packs", "POST /packs/pak-new/items"]);
    expect(showConfirm).not.toHaveBeenCalled();
  });
});
