/**
 * Publishing the active preset as a community item: the modal, the tag picker,
 * the consent gate, and the upload itself.
 */

import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { summarizeSignalChainIssues, validatePresetSignalChain } from "../signalPath/chainValidation.js";
import { uiState } from "../state.js";
import { apiFetch } from "./api.js";
import { loadBrowse, loadMine } from "./browse.js";
import { ensurePublishConsent } from "./consent.js";
import { element, setUploadStatus } from "./dom.js";
import { createToneSharingPresetItem, findPublishedItemForPreset, hasPublishableSignalChain, updateToneSharingPresetItem, type PublishedItemMatch } from "./itemUpload.js";
import { addItemToExistingPack, loadMyDraftPacksForSelect, openPackModal } from "./packEditor.js";
import { getPresetToneSharingOrigin } from "./presetLinks.js";
import { copyToneSharingShareLink } from "./shareLinks.js";
import { toneSharingState } from "./state.js";
import type { ToneSharingItem } from "./types.js";

const PUBLISH_BUTTON_LABEL = "Upload + Publish";
const UPDATE_BUTTON_LABEL = "Update Published Preset";

let publishItemInFlight = false;

export function setPublishItemBusy(busy: boolean, message?: string): void {
  publishItemInFlight = busy;
  const closeButton = element<HTMLButtonElement>("tone-sharing-publish-modal-close");
  const cancelButton = element<HTMLButtonElement>("tone-sharing-publish-modal-cancel");
  const submitButton = element<HTMLButtonElement>("tone-sharing-upload-item");
  const progress = element<HTMLElement>("tone-sharing-publish-progress");
  const titleInput = element<HTMLInputElement>("tone-sharing-item-title");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-item-description");
  const packSelect = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  const newPackButton = element<HTMLButtonElement>("tone-sharing-add-to-new-pack");
  document.querySelectorAll<HTMLButtonElement>("#tone-sharing-tags-picker .tone-sharing-tag-chip").forEach((button) => {
    button.disabled = busy;
  });
  [closeButton, cancelButton, submitButton, titleInput, descriptionInput, packSelect, newPackButton].forEach((control) => {
    if (control) {
      control.disabled = busy;
    }
  });
  if (progress) {
    progress.style.display = busy ? "flex" : "none";
    const label = progress.querySelector<HTMLElement>(".tone-sharing-progress-label");
    if (label) {
      label.textContent = message ?? "Working...";
    }
  }
}

export function openToneSharingPublishPresetModal(defaultTitle?: string, defaultDescription?: string): void {
  const modal = element<HTMLElement>("tone-sharing-publish-modal");
  if (!modal) {
    return;
  }

  const titleInput = element<HTMLInputElement>("tone-sharing-item-title");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-item-description");

  // Pre-fill from active preset if not overridden
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  const resolvedTitle = defaultTitle ?? activePreset?.name ?? "";
  const resolvedDescription = defaultDescription ?? activePreset?.description ?? "";

  if (titleInput) {
    titleInput.value = resolvedTitle;
  }
  if (descriptionInput) {
    descriptionInput.value = resolvedDescription;
  }
  setToneSharingTagsPickerValue(activePreset?.tags ?? []);
  setPublishItemBusy(false, "Publishing preset...");

  setUploadStatus(activePreset ? `Publishing: ${activePreset.name ?? activePreset.id}` : "");

  const publishButton = element<HTMLButtonElement>("tone-sharing-upload-item");
  const origin = getPresetToneSharingOrigin(activePreset);
  // Published from this preset before: publishing again updates that item.
  const alreadyPublished = activePreset ? findPublishedItemForPreset(activePreset, toneSharingState.myItems) : null;
  if (publishButton) {
    publishButton.disabled = false;
    publishButton.title = "";
    publishButton.textContent = alreadyPublished?.match === "preset" ? UPDATE_BUTTON_LABEL : PUBLISH_BUTTON_LABEL;
  }
  if (alreadyPublished?.match === "preset") {
    setUploadStatus(`Already on Tone Sharing as "${alreadyPublished.item.title}": publishing again updates it, keeping its link, downloads and ratings.`);
  }
  if (origin?.republishBlocked) {
    setUploadStatus("This preset came from Tone Sharing. Use Save As to create a local copy before publishing.");
    if (publishButton) {
      publishButton.disabled = true;
      publishButton.title = "Use Save As to create a new local preset before publishing";
    }
  } else if (activePreset) {
    const chainProblem = summarizeSignalChainIssues(validatePresetSignalChain(activePreset));
    if (chainProblem) {
      setUploadStatus(`Can't publish yet: ${chainProblem}`);
      if (publishButton) {
        publishButton.disabled = true;
        publishButton.title = "Fix the signal chain before publishing";
      }
    }
  }

  // Reset pack assign UI
  element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.remove("active");
  const packSelect = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  if (packSelect) packSelect.value = "";
  void loadMyDraftPacksForSelect();

  modal.style.display = "flex";
}

export function closeToneSharingPublishPresetModal(force = false): void {
  if (publishItemInFlight && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-publish-modal");
  if (!modal) {
    return;
  }
  modal.style.display = "none";
}

export function getToneSharingTagsPickerValue(): string[] {
  const picker = element<HTMLElement>("tone-sharing-tags-picker");
  if (!picker) return [];
  return Array.from(picker.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip.active"))
    .map((btn) => btn.dataset.tag ?? "")
    .filter(Boolean);
}

export function setToneSharingTagsPickerValue(tags: string[]): void {
  const picker = element<HTMLElement>("tone-sharing-tags-picker");
  if (!picker) return;
  const tagSet = new Set(tags);
  picker.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip").forEach((btn) => {
    btn.classList.toggle("active", tagSet.has(btn.dataset.tag ?? ""));
  });
}

export async function uploadAndPublishItem(): Promise<void> {
  if (publishItemInFlight) {
    return;
  }
  let title = element<HTMLInputElement>("tone-sharing-item-title")?.value.trim() ?? "";
  let description = element<HTMLTextAreaElement>("tone-sharing-item-description")?.value.trim() ?? "";
  const selectedTags = getToneSharingTagsPickerValue();

  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  if (!activePreset) {
    setUploadStatus("No preset selected.");
    return;
  }

  if (!title) {
    title = activePreset.name ?? activePreset.id;
  }
  if (!description) {
    description = activePreset.description ?? "";
  }

  if (!toneSharingState.user) {
    setUploadStatus("Sign in first.");
    return;
  }
  if (!title) {
    setUploadStatus("Title is required.");
    return;
  }

  const origin = getPresetToneSharingOrigin(activePreset);
  if (origin?.republishBlocked) {
    setUploadStatus("This preset came from Tone Sharing. Use Save As before publishing it again.");
    return;
  }

  // The chain is checked before consent is asked for, so nobody agrees to publish a preset
  // that is then refused.
  if (!hasPublishableSignalChain(activePreset)) {
    setUploadStatus("Preset must have a signal chain with at least one effect node to publish.");
    return;
  }

  const chainProblem = summarizeSignalChainIssues(validatePresetSignalChain(activePreset));
  if (chainProblem) {
    setUploadStatus(`Can't publish yet: ${chainProblem}`);
    return;
  }

  try {
    await ensurePublishConsent();
  } catch (error) {
    setUploadStatus(`Publish cancelled: ${(error as Error).message}`);
    return;
  }

  // Re-publishing a preset updates the item it already is. An older item with the same name but
  // no link to this preset is only a guess, so the user is asked.
  let existing: PublishedItemMatch | null;
  try {
    const mine = await apiFetch<{ items: ToneSharingItem[] }>("/items/me/list");
    toneSharingState.myItems = mine.items;
    existing = findPublishedItemForPreset(activePreset, mine.items);
  } catch {
    existing = findPublishedItemForPreset(activePreset, toneSharingState.myItems);
  }
  if (existing?.match === "title") {
    const update = await showConfirm(
      `You already have "${existing.item.title}" on Tone Sharing.\n\nUpdate it with this preset? Its link, downloads and ratings are kept.\n\nTo publish this as a separate preset, give it a different name first.`,
      "Update Published Preset",
      { ok: "Update" },
    );
    if (!update) {
      setUploadStatus("Not published.");
      return;
    }
  }

  setUploadStatus("Building & uploading archive...");
  setPublishItemBusy(true, existing ? "Updating preset..." : "Uploading preset...");

  try {
    const meta = { title, description, tags: selectedTags };
    let updated: boolean;
    let item: ToneSharingItem;
    if (existing) {
      item = await updateToneSharingPresetItem(existing.item.id, activePreset, meta);
      updated = true;
    } else {
      const created = await createToneSharingPresetItem(activePreset, meta);
      item = created;
      updated = created.updatedExisting;
    }

    const published = await apiFetch<{ moderationStatus?: string }>(`/items/${item.id}/publish`, { method: "POST" });
    // Live at once, unless the creator is held for review (or an older server, which says nothing).
    const live = (published?.moderationStatus ?? "approved") === "approved";

    let shareCopied = false;
    try {
      const link = await copyToneSharingShareLink({ kind: "item", id: item.id });
      showNotification("Share link copied", link);
      shareCopied = true;
    } catch {
      shareCopied = false;
    }

    const addToNewPack = element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.contains("active") ?? false;
    const assignPackId = element<HTMLSelectElement>("tone-sharing-pack-assign-select")?.value ?? "";
    setPublishItemBusy(false, "Uploading preset for approval...");
    closeToneSharingPublishPresetModal(true);

    const shareNote = live
      ? (shareCopied ? " Share link copied." : " Open the item card and press Share to copy its link.")
      : (shareCopied ? " Share link copied (it opens once approved)." : " Open the item card and press Share to copy the link once approved.");
    const outcome = updated
      ? (live ? "Published preset updated." : "Preset updated and submitted for review. It is listed once a moderator approves it.")
      : (live ? "Preset published." : "Preset submitted for review. It is listed once a moderator approves it.");

    if (addToNewPack) {
      await openPackModal(undefined, item.id);
      setUploadStatus(`${outcome}${shareNote}`);
    } else if (assignPackId) {
      try {
        await addItemToExistingPack(assignPackId, item.id);
        setUploadStatus(`${outcome} Added to the pack.${shareNote}`);
      } catch (error) {
        setUploadStatus(`${outcome} The pack update failed: ${(error as Error).message}.${shareNote}`);
      }
    } else {
      setUploadStatus(`${outcome}${shareNote}`);
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
  } finally {
    setPublishItemBusy(false, "Uploading preset for approval...");
  }
}
