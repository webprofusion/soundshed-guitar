/**
 * Authoring a pack: the modal, the draft list it can be added to, and the save
 * that either keeps it a draft or publishes it.
 *
 * The modal also starts a pack from a preset folder; ./folderPack.ts owns that
 * list and the uploads it needs.
 */

import type { Preset } from "../types.js";
import { showConfirm } from "../dialogs.js";
import { apiFetch, ToneSharingApiError } from "./api.js";
import { loadBrowse, loadMine } from "./browse.js";
import { ensurePublishConsent } from "./consent.js";
import { element, setText, setUploadStatus } from "./dom.js";
import { clearFolderPackSession, countFolderPackUploads, getFolderPackSession, renderFolderPackSelection, resolveFolderPackItemIds, startFolderPackSession } from "./folderPack.js";
import { buildPackImageVariants } from "./images.js";
import { uploadToneSharingAsset } from "./itemUpload.js";
import { toneSharingState } from "./state.js";
import type { ToneSharingItem, ToneSharingPack, ToneSharingPackDetails } from "./types.js";
import { escapeHtml } from "../utils.js";

let publishPackInFlight = false;

let editingPackId: string | null = null;

export function setPublishPackBusy(busy: boolean, message?: string): void {
  publishPackInFlight = busy;
  const closeButton = element<HTMLButtonElement>("tone-sharing-pack-modal-close");
  const cancelButton = element<HTMLButtonElement>("tone-sharing-pack-modal-cancel");
  const saveDraftButton = element<HTMLButtonElement>("tone-sharing-save-pack-draft");
  const publishButton = element<HTMLButtonElement>("tone-sharing-create-pack");
  const titleInput = element<HTMLInputElement>("tone-sharing-pack-title");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-pack-description");
  const imageInput = element<HTMLInputElement>("tone-sharing-pack-image");
  const progress = element<HTMLElement>("tone-sharing-pack-progress");
  [closeButton, cancelButton, saveDraftButton, publishButton, titleInput, descriptionInput, imageInput].forEach((control) => {
    if (control) {
      control.disabled = busy;
    }
  });
  document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[type='checkbox']").forEach((checkbox) => {
    checkbox.disabled = busy || checkbox.dataset.packBlocked === "true";
  });
  if (progress) {
    progress.style.display = busy ? "flex" : "none";
    const label = progress.querySelector<HTMLElement>(".tone-sharing-progress-label");
    if (label) {
      label.textContent = message ?? "Working...";
    }
  }
}

/** Empties the form and sets the item list's heading for the kind of pack being made. */
function resetPackModal(itemsLabel: string, itemsHint: string): void {
  setPublishPackBusy(false, "Publishing pack...");
  const titleEl = element<HTMLInputElement>("tone-sharing-pack-title");
  const descEl = element<HTMLTextAreaElement>("tone-sharing-pack-description");
  const imageEl = element<HTMLInputElement>("tone-sharing-pack-image");
  if (titleEl) titleEl.value = "";
  if (descEl) descEl.value = "";
  if (imageEl) imageEl.value = "";
  setText("tone-sharing-pack-status", "");
  setText("tone-sharing-pack-items-label", itemsLabel);
  setText("tone-sharing-pack-items-hint", itemsHint);
}

export async function openPackModal(packId?: string, preCheckItemId?: string): Promise<void> {
  editingPackId = packId ?? null;
  clearFolderPackSession();
  const modal = element<HTMLElement>("tone-sharing-pack-modal");
  if (!modal) {
    return;
  }
  resetPackModal("My Published Presets", "Check presets to include in this pack");

  const titleEl = element<HTMLInputElement>("tone-sharing-pack-title");
  const descEl = element<HTMLTextAreaElement>("tone-sharing-pack-description");
  const titleHeader = element<HTMLElement>("tone-sharing-pack-modal-title");

  let checkedItemIds = new Set<string>(preCheckItemId ? [preCheckItemId] : []);

  if (packId) {
    if (titleHeader) titleHeader.textContent = "Edit Pack";
    setText("tone-sharing-pack-status", "Loading...");
    try {
      const details = await apiFetch<ToneSharingPackDetails>(`/packs/${packId}`);
      if (titleEl) titleEl.value = details.pack.title;
      if (descEl) descEl.value = details.pack.description ?? "";
      checkedItemIds = new Set(details.items.map((item) => item.itemId));
      if (preCheckItemId) {
        checkedItemIds.add(preCheckItemId);
      }
    } catch (error) {
      setText("tone-sharing-pack-status", `Load failed: ${(error as Error).message}`);
    }
  } else {
    if (titleHeader) titleHeader.textContent = "Create Pack";
  }

  renderPackItemSelection(toneSharingState.myItems, checkedItemIds);
  setText("tone-sharing-pack-status", "");
  modal.style.display = "flex";
}

/**
 * Opens the modal on a new pack made from a preset folder: titled after the
 * folder, listing its presets, all checked. `loadPreset` returns a preset in
 * full — graph and Tone Sharing origin — where the chooser's list has a summary.
 */
export async function openFolderPackModal(
  folderName: string,
  presets: Preset[],
  loadPreset: (presetId: string) => Promise<Preset>,
): Promise<void> {
  editingPackId = null;
  clearFolderPackSession();
  const modal = element<HTMLElement>("tone-sharing-pack-modal");
  if (!modal) {
    return;
  }
  resetPackModal(`Presets in ${folderName}`, "Presets not yet on Tone Sharing are uploaded as drafts");
  const titleEl = element<HTMLInputElement>("tone-sharing-pack-title");
  if (titleEl) titleEl.value = folderName;
  setText("tone-sharing-pack-modal-title", "Create Pack from Folder");
  const itemsHost = element<HTMLElement>("tone-sharing-pack-items");
  if (itemsHost) itemsHost.innerHTML = "";
  setText("tone-sharing-pack-status", "Loading your presets on Tone Sharing...");
  modal.style.display = "flex";

  try {
    await startFolderPackSession(folderName, presets, loadPreset);
    renderFolderPackSelection();
    setText("tone-sharing-pack-status", "");
  } catch (error) {
    setText("tone-sharing-pack-status", `Load failed: ${(error as Error).message}`);
  }
}

export function closePackModal(force = false): void {
  if (publishPackInFlight && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-pack-modal");
  if (modal) {
    modal.style.display = "none";
  }
  editingPackId = null;
  clearFolderPackSession();
}

export async function loadMyDraftPacksForSelect(): Promise<void> {
  const select = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  if (!select || !toneSharingState.user) {
    return;
  }
  try {
    const data = await apiFetch<{ packs: ToneSharingPack[] }>("/packs/me/list");
    const drafts = data.packs.filter((p) => p.moderationStatus === "draft");
    select.innerHTML =
      `<option value="">Or add to existing draft pack…</option>` +
      drafts.map((p) => `<option value="${escapeHtml(p.id)}">${escapeHtml(p.title)}</option>`).join("");
  } catch {
  }
}

/** The user's pack with this title (case and surrounding space ignored), as the server matches it. */
export async function findMyPackByTitle(title: string): Promise<ToneSharingPack | null> {
  const wanted = title.trim().toLowerCase();
  const data = await apiFetch<{ packs: ToneSharingPack[] }>("/packs/me/list");
  return data.packs.find((pack) => pack.title.trim().toLowerCase() === wanted) ?? null;
}

export async function addItemToExistingPack(packId: string, itemId: string): Promise<void> {
  const details = await apiFetch<ToneSharingPackDetails>(`/packs/${packId}`);
  const existingIds = details.items.map((item) => item.itemId);
  if (!existingIds.includes(itemId)) {
    existingIds.push(itemId);
  }
  await apiFetch(`/packs/${packId}/items`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ itemIds: existingIds })
  });
}

export async function savePack(publish: boolean): Promise<void> {
  if (publishPackInFlight) {
    return;
  }
  if (!toneSharingState.user) {
    setText("tone-sharing-pack-status", "Sign in first.");
    return;
  }

  const title = element<HTMLInputElement>("tone-sharing-pack-title")?.value.trim() ?? "";
  const description = element<HTMLTextAreaElement>("tone-sharing-pack-description")?.value.trim() ?? "";
  const imageFile = element<HTMLInputElement>("tone-sharing-pack-image")?.files?.[0] ?? null;

  if (!title) {
    setText("tone-sharing-pack-status", "Pack title is required.");
    return;
  }

  const folderPack = getFolderPackSession() !== null;
  const uploadCount = folderPack ? countFolderPackUploads() : 0;
  const checkedItemIds = Array.from(document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[data-pack-item-id]:checked"))
    .map((input) => input.dataset.packItemId ?? "")
    .filter(Boolean);
  const checkedPresetCount = document.querySelectorAll("#tone-sharing-pack-items input[data-pack-preset-id]:checked").length;

  if ((folderPack ? checkedPresetCount : checkedItemIds.length) === 0) {
    setText("tone-sharing-pack-status", "Select at least one preset.");
    return;
  }

  // A creator has one pack per title: making the same pack again updates the one already there.
  if (!editingPackId) {
    let sameTitle: ToneSharingPack | null = null;
    try {
      sameTitle = await findMyPackByTitle(title);
    } catch (error) {
      setText("tone-sharing-pack-status", `Could not check your packs: ${(error as Error).message}`);
      return;
    }
    if (sameTitle) {
      const update = await showConfirm(
        `You already have a pack called "${sameTitle.title}".\n\nUpdate it with these presets? Its link and downloads are kept, and presets you have not checked are taken out of it.\n\nTo make a separate pack, give it a different title.`,
        "Update Pack",
        { ok: "Update" },
      );
      if (!update) {
        setText("tone-sharing-pack-status", "Not saved.");
        return;
      }
      editingPackId = sameTitle.id;
    }
  }
  const updatingPack = editingPackId !== null;

  if (uploadCount > 0) {
    try {
      await ensurePublishConsent();
    } catch (error) {
      setText("tone-sharing-pack-status", `${publish ? "Publish" : "Save"} cancelled: ${(error as Error).message}`);
      return;
    }
  }

  // Uploading a folder's presets takes a while, so a draft save locks the form too.
  const busy = publish || uploadCount > 0;
  if (busy) {
    setPublishPackBusy(true, publish ? (editingPackId ? "Submitting pack for approval..." : "Publishing pack...") : "Saving pack...");
  }

  setText("tone-sharing-pack-status", editingPackId ? "Saving..." : "Creating pack...");

  try {
    const itemIds = folderPack
      ? await resolveFolderPackItemIds((message) => setText("tone-sharing-pack-status", message))
      : checkedItemIds;

    let thumbnailAssetId: string | undefined;
    if (imageFile) {
      const variants = await buildPackImageVariants(imageFile);
      thumbnailAssetId = await uploadToneSharingAsset("thumbnail", variants.small.blob);
    }

    let packId: string;
    if (editingPackId) {
      await apiFetch(`/packs/${editingPackId}`, {
        method: "PATCH",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, ...(thumbnailAssetId ? { thumbnailAssetId } : {}) })
      });
      packId = editingPackId;
    } else {
      let pack: { pack: ToneSharingPack };
      try {
        pack = await apiFetch<{ pack: ToneSharingPack }>("/packs", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ title, description, thumbnailAssetId })
        });
      } catch (error) {
        // Made since the check above (another device): save again and it is updated.
        if (error instanceof ToneSharingApiError && error.code === "PACK_EXISTS" && typeof error.details.packId === "string") {
          editingPackId = error.details.packId;
          throw new Error(`${error.message} Press ${publish ? "Publish" : "Save"} again to update it.`);
        }
        throw error;
      }
      packId = pack.pack.id;
      editingPackId = packId;
    }

    await apiFetch(`/packs/${packId}/items`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ itemIds })
    });

    if (publish) {
      // A draft item in the pack would never reach the moderators, so it goes with the pack.
      const draftIds = new Set(toneSharingState.myItems.filter((item) => item.moderationStatus === "draft").map((item) => item.id));
      for (const itemId of itemIds.filter((id) => draftIds.has(id))) {
        await apiFetch(`/items/${itemId}/publish`, { method: "POST" });
      }
      const published = await apiFetch<{ moderationStatus?: string }>(`/packs/${packId}/publish`, { method: "POST" });
      const live = (published?.moderationStatus ?? "approved") === "approved";
      const outcome = updatingPack
        ? (live ? "Published pack updated." : "Pack updated and submitted for review. It is listed once a moderator approves it.")
        : (live ? "Pack published." : "Pack submitted for review. It is listed once a moderator approves it.");
      setText("tone-sharing-pack-status", outcome);
      setUploadStatus(outcome);
      setPublishPackBusy(false, "Publishing pack...");
      closePackModal(true);
    } else {
      setText("tone-sharing-pack-status", "Draft saved.");
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-pack-status", `${publish ? "Publish" : "Save"} failed: ${(error as Error).message}`);
  } finally {
    if (busy) {
      setPublishPackBusy(false, "Publishing pack...");
    }
  }
}

export function renderPackItemSelection(items: ToneSharingItem[], checked = new Set<string>()): void {
  const host = element<HTMLElement>("tone-sharing-pack-items");
  if (!host) {
    return;
  }

  if (!items.length) {
    host.innerHTML = `<div class="tone-sharing-select-item">No published presets yet. Publish a preset first.</div>`;
    return;
  }

  host.innerHTML = items
    .map(
      (item) => `
        <label class="tone-sharing-select-item">
          <input type="checkbox" data-pack-item-id="${escapeHtml(item.id)}" ${checked.has(item.id) ? "checked" : ""} />
          <span>${escapeHtml(item.title)}</span>
        </label>
      `
    )
    .join("");
}
