// @vitest-environment jsdom
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const apiFetch = vi.fn();
const deleteAsset = vi.fn();
const sendModeration = vi.fn();
const showConfirm = vi.fn();
const requestBrowseReload = vi.fn();
let admin = true;

vi.mock("../ts/toneSharingPanel/api.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  apiFetch,
  isToneSharingAdmin: () => admin,
}));
vi.mock("../ts/toneSharingPanel/assets.js", () => ({ deleteAsset, sendModeration }));
vi.mock("../ts/dialogs.js", () => ({ showConfirm }));
vi.mock("../ts/toneSharingPanel/refresh.js", () => ({ requestBrowseReload, setBrowseLoaders: vi.fn() }));

const { bindAdminEditModal, confirmAndDelete, openAdminEditModal, saveAdminEdit, setFeatured } = await import("../ts/toneSharingPanel/adminEdit.js");
const { renderFeedRows } = await import("../ts/toneSharingPanel/feed.js");
const { browseCollections, browseState } = await import("../ts/toneSharingPanel/state.js");

const modalMarkup = readFileSync(join(__dirname, "..", "ui-components", "modals", "tone-sharing-admin-edit-modal.html"), "utf8");

function patchBodies(path: string): unknown[] {
  return apiFetch.mock.calls
    .filter(([callPath, init]) => callPath === path && init?.method)
    .map(([, init]) => JSON.parse(init.body as string));
}

beforeEach(() => {
  admin = true;
  document.body.innerHTML = `${modalMarkup}<div id="tone-sharing-feed"></div><div id="tone-sharing-pack-view-modal" style="display: none;"></div>`;
  bindAdminEditModal();
});

afterEach(() => {
  apiFetch.mockReset();
  deleteAsset.mockReset();
  sendModeration.mockReset();
  showConfirm.mockReset();
  requestBrowseReload.mockReset();
  browseState.mode = "items";
  browseCollections.items = [];
});

describe("admin edit modal", () => {
  it("saves an item's details and tags, and features it when the box is ticked", async () => {
    apiFetch.mockImplementation(async (path: string, init?: RequestInit) => {
      if (path === "/items/itm-1" && !init) {
        return { item: { id: "itm-1", title: "Old", description: "desc", tags: ["Blues", "custom"], featured: false, moderationStatus: "approved" } };
      }
      return {};
    });
    await openAdminEditModal({ kind: "item", id: "itm-1" });

    const name = document.getElementById("tone-sharing-admin-edit-name") as HTMLInputElement;
    expect(name.value).toBe("Old");
    expect(document.getElementById("tone-sharing-admin-edit-status")?.textContent).toBe("");
    // A tag outside the standard list is kept selectable, so saving does not drop it.
    const custom = document.querySelector<HTMLButtonElement>('#tone-sharing-admin-edit-tags [data-tag="custom"]');
    expect(custom?.classList.contains("active")).toBe(true);
    document.querySelector<HTMLButtonElement>('#tone-sharing-admin-edit-tags [data-tag="Blues"]')?.click();

    name.value = "New title";
    (document.getElementById("tone-sharing-admin-edit-featured") as HTMLInputElement).checked = true;
    await saveAdminEdit();

    expect(patchBodies("/items/itm-1")).toEqual([{ title: "New title", description: "desc", tags: ["custom"] }]);
    expect(patchBodies("/items/itm-1/featured")).toEqual([{ featured: true }]);
    expect(requestBrowseReload).toHaveBeenCalledTimes(1);
    expect(document.getElementById("tone-sharing-admin-edit-modal")?.style.display).toBe("none");
  });

  it("removes unchecked presets from a pack and leaves featuring alone when unchanged", async () => {
    apiFetch.mockImplementation(async (path: string, init?: RequestInit) => {
      if (path === "/packs/pak-1" && !init) {
        return {
          pack: { id: "pak-1", title: "Pack", featured: true, moderationStatus: "approved" },
          items: [
            { itemId: "b", sortOrder: 1, title: "B", type: "preset" },
            { itemId: "a", sortOrder: 0, title: "A", type: "preset" },
          ],
        };
      }
      return {};
    });
    await openAdminEditModal({ kind: "pack", id: "pak-1" });
    expect(document.getElementById("tone-sharing-admin-edit-modal")?.dataset.kind).toBe("pack");
    (document.querySelector('[data-admin-pack-item-id="a"]') as HTMLInputElement).checked = false;
    await saveAdminEdit();

    expect(patchBodies("/packs/pak-1")).toEqual([{ title: "Pack", description: "" }]);
    expect(patchBodies("/packs/pak-1/items")).toEqual([{ itemIds: ["b"] }]);
    expect(patchBodies("/packs/pak-1/featured")).toEqual([]);
  });

  it("does not rewrite a pack's presets when none were unchecked", async () => {
    apiFetch.mockImplementation(async (path: string, init?: RequestInit) => (path === "/packs/pak-1" && !init
      ? { pack: { id: "pak-1", title: "Pack" }, items: [{ itemId: "a", sortOrder: 0, title: "A", type: "preset" }] }
      : {}));
    await openAdminEditModal({ kind: "pack", id: "pak-1" });
    await saveAdminEdit();
    expect(patchBodies("/packs/pak-1/items")).toEqual([]);
  });

  it("will not feature content that is not approved", async () => {
    apiFetch.mockResolvedValue({ item: { id: "itm-1", title: "Pending", moderationStatus: "pending_review" } });
    await openAdminEditModal({ kind: "item", id: "itm-1" });
    expect((document.getElementById("tone-sharing-admin-edit-featured") as HTMLInputElement).disabled).toBe(true);
  });

  it("deletes only after confirmation", async () => {
    showConfirm.mockResolvedValueOnce(false).mockResolvedValueOnce(true);
    expect(await confirmAndDelete({ kind: "pack", id: "pak-1" }, "Pack")).toBe(false);
    expect(deleteAsset).not.toHaveBeenCalled();
    expect(await confirmAndDelete({ kind: "pack", id: "pak-1" }, "Pack")).toBe(true);
    expect(deleteAsset).toHaveBeenCalledWith("pack", "pak-1");
  });

  it("keeps the cached page in step after a feature toggle", async () => {
    apiFetch.mockResolvedValue({});
    browseCollections.items = [{ id: "itm-1", title: "T", type: "preset", featured: false }];
    await setFeatured({ kind: "item", id: "itm-1" }, true);
    expect(browseCollections.items[0].featured).toBe(true);
  });
});

describe("admin card buttons", () => {
  const row = (moderationStatus?: string) => ({
    id: "r", slug: "r", title: "Row",
    items: [{ id: "itm-1", kind: "item" as const, title: "T", type: "preset", featured: true, moderationStatus }],
  });
  const actions = () => Array.from(document.querySelectorAll<HTMLButtonElement>("#tone-sharing-feed [data-action]")).map((b) => b.dataset.action);

  it("offers feature, edit and delete to admins while browsing", async () => {
    browseState.mode = "items";
    await renderFeedRows([row()]);
    expect(actions()).toEqual(expect.arrayContaining(["feature", "admin-edit", "admin-delete"]));
    expect(document.querySelector<HTMLButtonElement>('[data-action="feature"]')?.classList.contains("is-active")).toBe(true);
  });

  it("offers no feature toggle on pending content, and nothing to other users", async () => {
    browseState.mode = "review";
    await renderFeedRows([row("pending_review")]);
    expect(actions()).not.toContain("feature");
    expect(actions()).toContain("admin-edit");

    admin = false;
    browseState.mode = "items";
    await renderFeedRows([row()]);
    expect(actions()).not.toContain("feature");
    expect(actions()).not.toContain("admin-edit");
    expect(actions()).not.toContain("admin-delete");
  });

  it("keeps the owner's own tools in My Content without doubling them", async () => {
    browseState.mode = "mine";
    await renderFeedRows([row()]);
    expect(actions()).toContain("feature");
    expect(actions()).toContain("delete");
    expect(actions()).not.toContain("admin-delete");
  });
});

describe("admin moderation", () => {
  const creator = (requiresReview: boolean) => ({
    userId: "u1", email: "c@example.com", displayName: "Creator", handle: null,
    requiresReview, notes: null, setAt: requiresReview ? "2026-11-25 10:00:00" : null, setBy: requiresReview ? "admin" : null,
    items: { approved: 3, pending_review: 1 }, packs: {},
  });
  const visibleModerationButtons = () => Array.from(document.querySelectorAll<HTMLButtonElement>(".tone-sharing-admin-moderation-actions button"))
    .filter((button) => !button.hidden)
    .map((button) => button.id.replace("tone-sharing-admin-edit-", ""));
  const byId = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
  const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

  function serve(status: string, held = false, extra: (path: string, init?: RequestInit) => unknown = () => ({})) {
    apiFetch.mockImplementation(async (path: string, init?: RequestInit) => {
      if (path === "/items/itm-1" && !init) {
        return { item: { id: "itm-1", title: "Tone", creatorUserId: "u1", moderationStatus: status, moderationNote: { action: "note", notes: "Earlier note", createdAt: "2026-11-20 09:00:00" } } };
      }
      if (path === "/admin/creators/u1" && !init) {
        return { creator: creator(held) };
      }
      return extra(path, init);
    });
  }

  it("offers only the verdicts that fit the status", async () => {
    serve("approved");
    await openAdminEditModal({ kind: "item", id: "itm-1" });
    expect(visibleModerationButtons()).toEqual(["unpublish", "send-note"]);
    expect(byId("tone-sharing-admin-edit-last-note").textContent).toContain("Earlier note");

    serve("pending_review");
    await openAdminEditModal({ kind: "item", id: "itm-1" });
    expect(visibleModerationButtons()).toEqual(["reject", "approve", "send-note"]);
  });

  it("unpublishes with a note after confirmation, then closes and reloads", async () => {
    serve("approved");
    sendModeration.mockResolvedValue({ authorNotified: true });
    await openAdminEditModal({ kind: "item", id: "itm-1" });
    byId<HTMLTextAreaElement>("tone-sharing-admin-edit-note").value = "Not good enough to be listed.";

    showConfirm.mockResolvedValueOnce(false);
    byId<HTMLButtonElement>("tone-sharing-admin-edit-unpublish").click();
    await settle();
    expect(sendModeration).not.toHaveBeenCalled();

    showConfirm.mockResolvedValueOnce(true);
    byId<HTMLButtonElement>("tone-sharing-admin-edit-unpublish").click();
    await settle();
    await settle();
    expect(showConfirm.mock.calls[1][0]).toContain("emailed your note");
    expect(sendModeration).toHaveBeenCalledWith("item", "itm-1", "unpublish", { notes: "Not good enough to be listed.", notifyAuthor: true });
    expect(byId("tone-sharing-admin-edit-modal").style.display).toBe("none");
    expect(requestBrowseReload).toHaveBeenCalledTimes(1);
  });

  it("sends a note only when there is one, without emailing when unticked, and stays open", async () => {
    serve("approved");
    sendModeration.mockResolvedValue({ authorNotified: false });
    await openAdminEditModal({ kind: "item", id: "itm-1" });

    byId<HTMLButtonElement>("tone-sharing-admin-edit-send-note").click();
    await settle();
    expect(sendModeration).not.toHaveBeenCalled();
    expect(byId("tone-sharing-admin-edit-status").textContent).toBe("Write a note to send.");

    byId<HTMLTextAreaElement>("tone-sharing-admin-edit-note").value = "Please add a description.";
    byId<HTMLInputElement>("tone-sharing-admin-edit-notify").checked = false;
    byId<HTMLButtonElement>("tone-sharing-admin-edit-send-note").click();
    await settle();
    expect(sendModeration).toHaveBeenCalledWith("item", "itm-1", "note", { notes: "Please add a description.", notifyAuthor: false });
    expect(byId("tone-sharing-admin-edit-modal").style.display).toBe("flex");
    expect(byId<HTMLTextAreaElement>("tone-sharing-admin-edit-note").value).toBe("");
    expect(byId("tone-sharing-admin-edit-last-note").textContent).toContain("Please add a description.");
  });

  it("holds the creator for review, and puts the box back when that fails", async () => {
    let failPut = false;
    serve("approved", false, (path, init) => {
      if (path === "/admin/creators/u1" && init?.method === "PUT") {
        if (failPut) throw new Error("offline");
        return { creator: creator(JSON.parse(init.body as string).requiresReview) };
      }
      return {};
    });
    await openAdminEditModal({ kind: "item", id: "itm-1" });
    await settle();
    const hold = byId<HTMLInputElement>("tone-sharing-admin-edit-hold");
    expect(hold.checked).toBe(false);
    expect(hold.disabled).toBe(false);
    expect(byId("tone-sharing-admin-edit-hold-hint").textContent).toContain("goes live at once");
    expect(byId("tone-sharing-admin-edit-hold-hint").textContent).toContain("Presets: 3 live, 1 waiting");

    hold.checked = true;
    hold.dispatchEvent(new Event("change"));
    await settle();
    expect(patchBodies("/admin/creators/u1")).toEqual([{ requiresReview: true }]);
    expect(byId("tone-sharing-admin-edit-hold-hint").textContent).toContain("waits in the Review Queue");

    failPut = true;
    hold.checked = false;
    hold.dispatchEvent(new Event("change"));
    await settle();
    expect(hold.checked).toBe(true);
    expect(byId("tone-sharing-admin-edit-hold-hint").textContent).toContain("offline");
  });

  it("shows a moderator's note on the creator's own card, escaped", async () => {
    browseState.mode = "mine";
    await renderFeedRows([{
      id: "r", slug: "r", title: "Row",
      items: [{ id: "itm-1", kind: "item", title: "T", type: "preset", moderationStatus: "draft", moderationNote: { action: "unpublish", notes: "<b>clips</b>", createdAt: "" } }],
    }]);
    const note = document.querySelector(".tone-sharing-card-moderation-note");
    expect(note?.textContent).toBe("Moderator<b>clips</b>");
    expect(note?.querySelector("b")).toBeNull();
  });
});
