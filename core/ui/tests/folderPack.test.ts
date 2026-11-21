// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types.js";
import type { ToneSharingItem } from "../ts/toneSharingPanel/types.js";

const apiFetch = vi.fn();
const createToneSharingPresetItem = vi.fn();

vi.mock("../ts/toneSharingPanel/api.js", () => ({ apiFetch }));
vi.mock("../ts/toneSharingPanel/itemUpload.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  createToneSharingPresetItem,
}));

const { clearFolderPackSession, countFolderPackUploads, matchFolderPresetsToItems, renderFolderPackSelection, resolveFolderPackItemIds, startFolderPackSession } =
  await import("../ts/toneSharingPanel/folderPack.js");
const { toneSharingState } = await import("../ts/toneSharingPanel/state.js");

function preset(id: string, name: string, extra: Partial<Preset> = {}): Preset {
  return {
    id,
    name,
    graph: {
      nodes: [
        { id: "in", type: "input" },
        { id: "amp", type: "amp" },
        { id: "out", type: "output" },
      ],
      edges: [],
    },
    ...extra,
  } as unknown as Preset;
}

function item(id: string, title: string, moderationStatus?: string): ToneSharingItem {
  return { id, title, type: "preset", moderationStatus };
}

afterEach(() => {
  clearFolderPackSession();
  apiFetch.mockReset();
  createToneSharingPresetItem.mockReset();
  document.body.innerHTML = "";
});

describe("matching a folder's presets to the user's items", () => {
  it("links by title, preferring an approved item and never a rejected one", () => {
    const entries = matchFolderPresetsToItems(
      [preset("a", "Crunch"), preset("b", " Lead "), preset("c", "New Tone")],
      [item("i1", "crunch", "draft"), item("i2", "Crunch", "approved"), item("i3", "Lead", "rejected")],
    );
    expect(entries.map((entry) => entry.itemId)).toEqual(["i2", null, null]);
    expect(entries.every((entry) => entry.blockedReason === null)).toBe(true);
  });

  it("links a preset installed from the user's own item, and blocks one from someone else's", () => {
    const own = preset("a", "Mine", { toneSharingOrigin: { source: "toneSharingApi", itemId: "i1" } } as Partial<Preset>);
    const theirs = preset("b", "Theirs", { toneSharingOrigin: { source: "toneSharingApi", itemId: "other" } } as Partial<Preset>);
    const entries = matchFolderPresetsToItems([own, theirs], [item("i1", "Renamed", "approved")]);
    expect(entries[0]).toMatchObject({ itemId: "i1", blockedReason: null });
    expect(entries[1]).toMatchObject({ itemId: null, blockedReason: "From Tone Sharing" });
  });
});

describe("saving a folder pack", () => {
  it("uploads only the checked presets with no item, once, and returns every item id", async () => {
    document.body.innerHTML = `<div id="tone-sharing-pack-items"></div>`;
    apiFetch.mockResolvedValue({ items: [item("i1", "Crunch", "approved")] });
    const presets = [preset("a", "Crunch"), preset("b", "Lead"), preset("c", "Clean")];
    await startFolderPackSession("Gig", presets, async (id) => presets.find((p) => p.id === id)!);
    renderFolderPackSelection();
    document.querySelector<HTMLInputElement>('input[data-pack-preset-id="c"]')!.checked = false;
    expect(countFolderPackUploads()).toBe(1);

    createToneSharingPresetItem.mockResolvedValue(item("new-b", "Lead"));
    const progress = vi.fn();
    expect(await resolveFolderPackItemIds(progress)).toEqual(["i1", "new-b"]);
    expect(createToneSharingPresetItem).toHaveBeenCalledTimes(1);
    expect(createToneSharingPresetItem.mock.calls[0][1]).toMatchObject({ title: "Lead" });
    expect(toneSharingState.myItems.find((entry) => entry.id === "new-b")?.moderationStatus).toBe("draft");

    // A second save (draft, then publish) reuses the upload.
    expect(await resolveFolderPackItemIds(progress)).toEqual(["i1", "new-b"]);
    expect(createToneSharingPresetItem).toHaveBeenCalledTimes(1);
  });

  it("matches on the loaded preset, since the chooser's summary has no Tone Sharing origin", async () => {
    document.body.innerHTML = `<div id="tone-sharing-pack-items"></div>`;
    apiFetch.mockResolvedValue({ items: [] });
    const summary = { id: "a", name: "Installed" } as Preset;
    const full = preset("a", "Installed", { toneSharingOrigin: { source: "toneSharingApi", itemId: "someone-else" } } as Partial<Preset>);
    await startFolderPackSession("Gig", [summary], async () => full);
    renderFolderPackSelection();

    expect(document.querySelector<HTMLInputElement>('input[data-pack-preset-id="a"]')?.disabled).toBe(true);
    expect(countFolderPackUploads()).toBe(0);
  });

  it("refuses before uploading anything when a preset has an empty chain", async () => {
    document.body.innerHTML = `<div id="tone-sharing-pack-items"></div>`;
    apiFetch.mockResolvedValue({ items: [] });
    const empty = preset("b", "Empty", { graph: { nodes: [], edges: [] } } as Partial<Preset>);
    const presets = [preset("a", "Good"), empty];
    await startFolderPackSession("Gig", presets, async (id) => presets.find((p) => p.id === id)!);
    renderFolderPackSelection();

    await expect(resolveFolderPackItemIds(vi.fn())).rejects.toThrow(/Empty/);
    expect(createToneSharingPresetItem).not.toHaveBeenCalled();
  });

  it("refuses before uploading anything when a preset has a drive after its cab", async () => {
    document.body.innerHTML = `<div id="tone-sharing-pack-items"></div>`;
    apiFetch.mockResolvedValue({ items: [] });
    const { EffectGuids } = await import("../ts/effectGuids.js");
    const fizzy = preset("b", "Fizzy", {
      graph: {
        nodes: [
          { id: "in", type: "input" },
          { id: "cab", type: EffectGuids.kCabSimple },
          { id: "od", type: EffectGuids.kOverdrive },
          { id: "out", type: "output" },
        ],
        edges: [
          { from: "in", to: "cab" },
          { from: "cab", to: "od" },
          { from: "od", to: "out" },
        ],
      },
    } as unknown as Partial<Preset>);
    const presets = [preset("a", "Good"), fizzy];
    await startFolderPackSession("Gig", presets, async (id) => presets.find((p) => p.id === id)!);
    renderFolderPackSelection();

    await expect(resolveFolderPackItemIds(vi.fn())).rejects.toThrow(/"Fizzy" can't be published: .*after the cab/);
    expect(createToneSharingPresetItem).not.toHaveBeenCalled();
  });
});
