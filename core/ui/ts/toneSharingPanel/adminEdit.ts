/**
 * Admin tools for anyone's published tones: editing a preset's or pack's details,
 * featuring it on the home page, deleting it, and moderating it: unpublishing,
 * rejecting or approving it with a note to its creator, and holding the creator so
 * what they publish next waits in the Review Queue.
 *
 * The server checks the admin role on every one of these calls; hiding the buttons
 * from other users is only so they are not offered something that would fail.
 */

import { showConfirm } from "../dialogs.js";
import { STANDARD_TAGS, renderTagChips } from "../presetTags.js";
import { escapeHtml } from "../utils.js";
import { apiFetch, isToneSharingAdmin } from "./api.js";
import { deleteAsset, sendModeration, type ModerationAction } from "./assets.js";
import { element, setText, setUploadStatus } from "./dom.js";
import { canFeature } from "./format.js";
import { buildPackImageVariants } from "./images.js";
import { uploadToneSharingAsset } from "./itemUpload.js";
import { clearPackDetail, viewPack } from "./packDetail.js";
import { requestBrowseReload } from "./refresh.js";
import { browseCollections, browseState } from "./state.js";
import type { ToneSharingCreatorModeration, ToneSharingItem, ToneSharingModerationNote, ToneSharingPack, ToneSharingPackDetails } from "./types.js";

type AdminTarget = { kind: "item" | "pack"; id: string };

type AdminEditSession = AdminTarget & {
  title: string;
  featured: boolean;
  approved: boolean;
  /** draft, pending_review, approved or rejected. */
  status: string;
  creatorUserId: string | null;
  /** The pack's presets as loaded, in order; empty for an item. */
  packItemIds: string[];
};

let session: AdminEditSession | null = null;
let saving = false;

export async function setFeatured(target: AdminTarget, featured: boolean): Promise<void> {
  const path = target.kind === "item" ? `/items/${target.id}/featured` : `/packs/${target.id}/featured`;
  await apiFetch(path, {
    method: "PATCH",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ featured }),
  });
  // Keep the cached page in step, so the next render of it shows the new state.
  const cached = target.kind === "item"
    ? browseCollections.items.find((entry) => entry.id === target.id)
    : browseCollections.packs.find((entry) => entry.id === target.id);
  if (cached) {
    cached.featured = featured;
  }
}

/** Asks first, then deletes; true when it was deleted. A deleted pack's open view is closed. */
export async function confirmAndDelete(target: AdminTarget, title: string): Promise<boolean> {
  const noun = target.kind === "item" ? "preset" : "pack";
  const consequence = target.kind === "item"
    ? "It is removed from every pack it is in, and its ratings and favourites go with it."
    : "The presets in it are kept; only the pack is removed.";
  const confirmed = await showConfirm(
    `Delete the ${noun} "${title}" from Tone Sharing for everyone?\n\n${consequence}\n\nThis cannot be undone.`,
    `Delete ${target.kind === "item" ? "Preset" : "Pack"}`,
  );
  if (!confirmed) {
    return false;
  }
  setUploadStatus(`Deleting ${noun}...`);
  await deleteAsset(target.kind, target.id);
  setUploadStatus(`${target.kind === "item" ? "Preset" : "Pack"} deleted.`);
  if (target.kind === "pack" && openPackViewId() === target.id) {
    clearPackDetail();
  }
  return true;
}

/** A message replaces the status line, an empty one clears it; none leaves it as it is. */
function setBusy(busy: boolean, message?: string): void {
  saving = busy;
  for (const id of ["tone-sharing-admin-edit-save", "tone-sharing-admin-edit-delete", "tone-sharing-admin-edit-cancel", "tone-sharing-admin-edit-close", ...MODERATION_BUTTON_IDS]) {
    const button = element<HTMLButtonElement>(id);
    if (button) button.disabled = busy;
  }
  if (message !== undefined) setText("tone-sharing-admin-edit-status", message);
}

const MODERATION_BUTTONS: Record<ModerationAction, string> = {
  unpublish: "tone-sharing-admin-edit-unpublish",
  reject: "tone-sharing-admin-edit-reject",
  approve: "tone-sharing-admin-edit-approve",
  note: "tone-sharing-admin-edit-send-note",
};
const MODERATION_BUTTON_IDS = Object.values(MODERATION_BUTTONS);

/** Which verdicts make sense from a status: only live content is unpublished, only waiting content rejected. */
export function moderationActionsFor(status: string): ModerationAction[] {
  switch (status) {
    case "approved":
      return ["unpublish", "note"];
    case "pending_review":
      return ["approve", "reject", "note"];
    default:
      return ["approve", "note"];
  }
}

function syncModerationButtons(): void {
  const offered = new Set(session ? moderationActionsFor(session.status) : []);
  for (const [action, id] of Object.entries(MODERATION_BUTTONS) as Array<[ModerationAction, string]>) {
    const button = element<HTMLButtonElement>(id);
    if (button) button.hidden = !offered.has(action);
  }
}

const NOTE_ACTION_LABELS: Record<string, string> = {
  approve: "approved",
  reject: "rejected",
  unpublish: "unpublished",
  note: "note",
};

/** A server timestamp ("2026-11-25 10:00:00", UTC) as a local date. */
function formatServerDate(value: string | null | undefined): string {
  if (!value) return "";
  const date = new Date(/[zZ]|[+-]\d\d:?\d\d$/.test(value) ? value : `${value.replace(" ", "T")}Z`);
  return Number.isNaN(date.getTime()) ? "" : date.toLocaleDateString();
}

function describeLastNote(note: ToneSharingModerationNote | null | undefined): string {
  if (!note?.notes) {
    return "";
  }
  const when = formatServerDate(note.createdAt);
  return `Last note (${NOTE_ACTION_LABELS[note.action] ?? note.action}${when ? `, ${when}` : ""}): ${note.notes}`;
}

function describeCounts(counts: Record<string, number>, noun: string): string {
  const parts = [
    ["approved", "live"],
    ["pending_review", "waiting"],
    ["draft", "draft"],
    ["rejected", "rejected"],
  ]
    .filter(([status]) => (counts[status] ?? 0) > 0)
    .map(([status, label]) => `${counts[status]} ${label}`);
  return parts.length ? `${noun}: ${parts.join(", ")}` : "";
}

function renderCreatorHold(creator: ToneSharingCreatorModeration | null, message?: string): void {
  const checkbox = element<HTMLInputElement>("tone-sharing-admin-edit-hold");
  if (checkbox) {
    checkbox.checked = Boolean(creator?.requiresReview);
    checkbox.disabled = !creator;
  }
  if (message !== undefined || !creator) {
    setText("tone-sharing-admin-edit-hold-hint", message ?? "");
    return;
  }
  const since = formatServerDate(creator.setAt);
  const state = creator.requiresReview
    ? `Everything they publish waits in the Review Queue${since ? ` (since ${since})` : ""}. What is already live stays live.`
    : "What they publish goes live at once.";
  const counts = [describeCounts(creator.items, "Presets"), describeCounts(creator.packs, "Packs")].filter(Boolean).join(" · ");
  setText("tone-sharing-admin-edit-hold-hint", counts ? `${state} ${counts}.` : state);
}

async function loadCreatorHold(target: AdminEditSession): Promise<void> {
  if (!target.creatorUserId) {
    renderCreatorHold(null, "The creator is not known for this tone.");
    return;
  }
  renderCreatorHold(null, "Loading creator...");
  try {
    const { creator } = await apiFetch<{ creator: ToneSharingCreatorModeration }>(`/admin/creators/${encodeURIComponent(target.creatorUserId)}`);
    if (session !== target) return;
    renderCreatorHold(creator);
  } catch (error) {
    if (session !== target) return;
    renderCreatorHold(null, `Creator review setting unavailable: ${(error as Error).message}`);
  }
}

async function setCreatorHold(requiresReview: boolean): Promise<void> {
  const current = session;
  const checkbox = element<HTMLInputElement>("tone-sharing-admin-edit-hold");
  if (!current?.creatorUserId || !checkbox) {
    return;
  }
  checkbox.disabled = true;
  setText("tone-sharing-admin-edit-hold-hint", requiresReview ? "Holding the creator for review..." : "Releasing the creator...");
  try {
    const { creator } = await apiFetch<{ creator: ToneSharingCreatorModeration }>(`/admin/creators/${encodeURIComponent(current.creatorUserId)}`, {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ requiresReview }),
    });
    if (session !== current) return;
    renderCreatorHold(creator);
    setUploadStatus(requiresReview ? "Creator held: what they publish now waits for review." : "Creator released: what they publish goes live at once.");
  } catch (error) {
    if (session !== current) return;
    checkbox.checked = !requiresReview;
    checkbox.disabled = false;
    setText("tone-sharing-admin-edit-hold-hint", `Could not change it: ${(error as Error).message}`);
  }
}

const MODERATION_DONE: Record<ModerationAction, string> = {
  approve: "Approved",
  reject: "Rejected",
  unpublish: "Unpublished",
  note: "Note sent",
};

const MODERATION_WORKING: Record<ModerationAction, string> = {
  approve: "Approving",
  reject: "Rejecting",
  unpublish: "Unpublishing",
  note: "Sending note",
};

async function applyModeration(action: ModerationAction): Promise<void> {
  if (!session || saving) {
    return;
  }
  const current = session;
  const noteInput = element<HTMLTextAreaElement>("tone-sharing-admin-edit-note");
  const notes = noteInput?.value.trim() ?? "";
  const notifyAuthor = element<HTMLInputElement>("tone-sharing-admin-edit-notify")?.checked ?? true;
  const noun = current.kind === "item" ? "preset" : "pack";
  const title = element<HTMLInputElement>("tone-sharing-admin-edit-name")?.value.trim() || current.title;

  if (action === "note" && !notes) {
    setText("tone-sharing-admin-edit-status", "Write a note to send.");
    noteInput?.focus();
    return;
  }
  if (action === "unpublish" || action === "reject") {
    const consequence = action === "unpublish"
      ? `It stops being listed or downloadable for anyone but its creator, leaves the featured rows and goes back to their drafts. If they publish it again it waits in the Review Queue.`
      : `It is not listed. If they publish it again it waits in the Review Queue.`;
    const telling = notifyAuthor
      ? (notes ? "The creator is emailed your note." : "The creator is emailed, without a note.")
      : (notes ? "The creator is not emailed; they see your note in My Content." : "The creator is not told.");
    const confirmed = await showConfirm(
      `${action === "unpublish" ? "Unpublish" : "Reject"} the ${noun} "${title}"?\n\n${consequence}\n\n${telling}`,
      `${action === "unpublish" ? "Unpublish" : "Reject"} ${current.kind === "item" ? "Preset" : "Pack"}`,
      { ok: action === "unpublish" ? "Unpublish" : "Reject" },
    );
    if (!confirmed || session !== current) {
      return;
    }
  }

  setBusy(true, `${MODERATION_WORKING[action]}...`);
  try {
    const { authorNotified } = await sendModeration(current.kind, current.id, action, { notes, notifyAuthor });
    const message = `${MODERATION_DONE[action]}.${authorNotified ? " The creator was emailed." : ""}`;
    setUploadStatus(`${current.kind === "item" ? "Preset" : "Pack"} "${title}": ${message}`);
    if (action === "note") {
      if (noteInput) noteInput.value = "";
      setText("tone-sharing-admin-edit-last-note", describeLastNote({ action, notes, createdAt: new Date().toISOString() }));
      setBusy(false, message);
      return;
    }
    setBusy(false);
    closeAdminEditModal(true);
    await refreshAfterAdminChange();
  } catch (error) {
    setBusy(false, `${MODERATION_WORKING[action]} failed: ${(error as Error).message}`);
  }
}

function describeStatus(target: ToneSharingItem | ToneSharingPack): string {
  const status = (target.moderationStatus ?? "approved").replace(/_/g, " ");
  const creator = target.creatorDisplayName || target.creatorProfileHandle || target.creatorEmail || target.creatorUserId || "unknown";
  return `Status: ${status} · Creator: ${creator}`;
}

function renderPackItems(items: ToneSharingPackDetails["items"]): void {
  const host = element<HTMLElement>("tone-sharing-admin-edit-items");
  if (!host) return;
  if (!items.length) {
    host.innerHTML = `<div class="tone-sharing-select-item">No presets in this pack.</div>`;
    return;
  }
  host.innerHTML = [...items]
    .sort((a, b) => a.sortOrder - b.sortOrder)
    .map((item) => `
        <label class="tone-sharing-select-item">
          <input type="checkbox" data-admin-pack-item-id="${escapeHtml(item.itemId)}" checked />
          <span>${escapeHtml(item.title)}</span>
        </label>`)
    .join("");
}

function syncFeaturedControl(): void {
  const checkbox = element<HTMLInputElement>("tone-sharing-admin-edit-featured");
  if (!checkbox || !session) return;
  checkbox.checked = session.featured;
  checkbox.disabled = !session.approved && !session.featured;
  setText("tone-sharing-admin-edit-featured-hint", session.approved ? "" : "Only approved content can be featured.");
}

export async function openAdminEditModal(target: AdminTarget): Promise<void> {
  const modal = element<HTMLElement>("tone-sharing-admin-edit-modal");
  if (!modal || !isToneSharingAdmin()) {
    return;
  }
  session = null;
  modal.dataset.kind = target.kind;
  setText("tone-sharing-admin-edit-title", target.kind === "item" ? "Edit Preset" : "Edit Pack");
  setText("tone-sharing-admin-edit-meta", "");
  const nameInput = element<HTMLInputElement>("tone-sharing-admin-edit-name");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-admin-edit-description");
  const imageInput = element<HTMLInputElement>("tone-sharing-admin-edit-image");
  if (nameInput) nameInput.value = "";
  if (descriptionInput) descriptionInput.value = "";
  if (imageInput) imageInput.value = "";
  const itemsHost = element<HTMLElement>("tone-sharing-admin-edit-items");
  if (itemsHost) itemsHost.innerHTML = "";
  const noteInput = element<HTMLTextAreaElement>("tone-sharing-admin-edit-note");
  if (noteInput) noteInput.value = "";
  const notifyCheckbox = element<HTMLInputElement>("tone-sharing-admin-edit-notify");
  if (notifyCheckbox) notifyCheckbox.checked = true;
  setText("tone-sharing-admin-edit-last-note", "");
  renderCreatorHold(null, "");
  syncModerationButtons();
  setBusy(true, "Loading...");
  modal.style.display = "flex";

  try {
    let record: ToneSharingItem | ToneSharingPack;
    let packItemIds: string[] = [];
    if (target.kind === "item") {
      record = (await apiFetch<{ item: ToneSharingItem }>(`/items/${target.id}`)).item;
      const tagsHost = element<HTMLElement>("tone-sharing-admin-edit-tags");
      if (tagsHost) {
        // Tags the standard list lacks stay selectable, so saving does not drop them.
        const current = (record as ToneSharingItem).tags ?? [];
        const all = [...STANDARD_TAGS, ...current.filter((tag) => !STANDARD_TAGS.includes(tag))];
        renderTagChips(tagsHost, all, "tone-sharing-tag-chip");
        tagsHost.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip").forEach((chip) => {
          chip.classList.toggle("active", current.includes(chip.dataset.tag ?? ""));
        });
      }
    } else {
      const details = await apiFetch<ToneSharingPackDetails>(`/packs/${target.id}`);
      record = details.pack;
      packItemIds = [...details.items].sort((a, b) => a.sortOrder - b.sortOrder).map((item) => item.itemId);
      renderPackItems(details.items);
    }

    session = {
      ...target,
      title: record.title,
      featured: Boolean(record.featured),
      approved: canFeature(record.moderationStatus),
      status: record.moderationStatus ?? "approved",
      creatorUserId: record.creatorUserId ?? null,
      packItemIds,
    };
    if (nameInput) nameInput.value = record.title;
    if (descriptionInput) descriptionInput.value = record.description ?? "";
    setText("tone-sharing-admin-edit-meta", describeStatus(record));
    setText("tone-sharing-admin-edit-last-note", describeLastNote(record.moderationNote));
    syncFeaturedControl();
    syncModerationButtons();
    setBusy(false, "");
    void loadCreatorHold(session);
  } catch (error) {
    setBusy(false, `Load failed: ${(error as Error).message}`);
    for (const id of ["tone-sharing-admin-edit-save", "tone-sharing-admin-edit-delete", ...MODERATION_BUTTON_IDS]) {
      const button = element<HTMLButtonElement>(id);
      if (button) button.disabled = true;
    }
  }
}

export function closeAdminEditModal(force = false): void {
  if (saving && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-admin-edit-modal");
  if (modal) {
    modal.style.display = "none";
  }
  session = null;
}

function openPackViewId(): string {
  const view = element<HTMLElement>("tone-sharing-pack-view-modal");
  return view && view.style.display === "flex" ? (view.dataset.packId ?? "") : "";
}

/** Reloads what is on screen after an admin change: the feed, and the pack view if it was open. */
export async function refreshAfterAdminChange(): Promise<void> {
  const packId = openPackViewId();
  await requestBrowseReload();
  if (packId) {
    try {
      await viewPack(packId);
    } catch {
      // Gone or no longer visible; the feed has already been redrawn.
    }
  }
}

export async function saveAdminEdit(): Promise<void> {
  if (!session || saving) {
    return;
  }
  const current = session;
  const title = element<HTMLInputElement>("tone-sharing-admin-edit-name")?.value.trim() ?? "";
  const description = element<HTMLTextAreaElement>("tone-sharing-admin-edit-description")?.value.trim() ?? "";
  if (!title) {
    setText("tone-sharing-admin-edit-status", "Title is required.");
    return;
  }
  const featured = element<HTMLInputElement>("tone-sharing-admin-edit-featured")?.checked ?? current.featured;

  setBusy(true, "Saving...");
  try {
    if (current.kind === "item") {
      const tags = Array.from(document.querySelectorAll<HTMLButtonElement>("#tone-sharing-admin-edit-tags .tone-sharing-tag-chip.active"))
        .map((chip) => chip.dataset.tag ?? "")
        .filter(Boolean);
      await apiFetch(`/items/${current.id}`, {
        method: "PATCH",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, tags }),
      });
    } else {
      const imageFile = element<HTMLInputElement>("tone-sharing-admin-edit-image")?.files?.[0] ?? null;
      let thumbnailAssetId: string | undefined;
      if (imageFile) {
        setText("tone-sharing-admin-edit-status", "Uploading image...");
        const variants = await buildPackImageVariants(imageFile);
        thumbnailAssetId = await uploadToneSharingAsset("thumbnail", variants.small.blob);
      }
      await apiFetch(`/packs/${current.id}`, {
        method: "PATCH",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, ...(thumbnailAssetId ? { thumbnailAssetId } : {}) }),
      });
      const keptItemIds = Array.from(document.querySelectorAll<HTMLInputElement>("#tone-sharing-admin-edit-items input[data-admin-pack-item-id]:checked"))
        .map((input) => input.dataset.adminPackItemId ?? "")
        .filter(Boolean);
      if (keptItemIds.join("\n") !== current.packItemIds.join("\n")) {
        await apiFetch(`/packs/${current.id}/items`, {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ itemIds: keptItemIds }),
        });
      }
    }

    if (featured !== current.featured) {
      await setFeatured(current, featured);
    }

    setUploadStatus(`${current.kind === "item" ? "Preset" : "Pack"} updated.`);
    setBusy(false);
    closeAdminEditModal(true);
    await refreshAfterAdminChange();
  } catch (error) {
    setBusy(false, `Save failed: ${(error as Error).message}`);
  }
}

async function deleteFromModal(): Promise<void> {
  if (!session || saving) {
    return;
  }
  const current = session;
  const title = element<HTMLInputElement>("tone-sharing-admin-edit-name")?.value.trim() || current.title;
  try {
    if (!(await confirmAndDelete(current, title))) {
      return;
    }
    closeAdminEditModal(true);
    await refreshAfterAdminChange();
  } catch (error) {
    setText("tone-sharing-admin-edit-status", `Delete failed: ${(error as Error).message}`);
  }
}

/** Whether a card in the current browse mode gets the admin buttons. */
export function showAdminCardActions(): boolean {
  return isToneSharingAdmin() && browseState.mode !== "installed";
}

export function bindAdminEditModal(): void {
  const tagsHost = element<HTMLElement>("tone-sharing-admin-edit-tags");
  tagsHost?.addEventListener("click", (event) => {
    const chip = (event.target as HTMLElement).closest<HTMLButtonElement>(".tone-sharing-tag-chip");
    chip?.classList.toggle("active");
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-save")?.addEventListener("click", () => {
    void saveAdminEdit();
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-delete")?.addEventListener("click", () => {
    void deleteFromModal();
  });
  for (const [action, id] of Object.entries(MODERATION_BUTTONS) as Array<[ModerationAction, string]>) {
    element<HTMLButtonElement>(id)?.addEventListener("click", () => {
      void applyModeration(action);
    });
  }
  element<HTMLInputElement>("tone-sharing-admin-edit-hold")?.addEventListener("change", (event) => {
    void setCreatorHold((event.currentTarget as HTMLInputElement).checked);
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-cancel")?.addEventListener("click", () => {
    closeAdminEditModal();
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-close")?.addEventListener("click", () => {
    closeAdminEditModal();
  });
  element<HTMLElement>("tone-sharing-admin-edit-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closeAdminEditModal();
    }
  });
}
