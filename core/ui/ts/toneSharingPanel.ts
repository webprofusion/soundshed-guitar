import { postMessage, setAppSetting } from "./bridge.js";
import { showConfirm } from "./dialogs.js";
import { buildToneSharingPresetArchiveBlobs, importPackWithConfirmation, importPresetArchive } from "./presets.js";
import { clonePreset, uiState } from "./state.js";
import type { Preset, PresetFolder } from "./types.js";
import { escapeHtml, idAccentColor } from "./utils.js";
import { arrayBufferToBase64, generateResourceId } from "./archiveUtils.js";
import { switchMainPanel } from "./navigation.js";
import { activateEquipmentTab, activateLibraryTab } from "./settings.js";
import { setTone3000Search } from "./tone3000Browser.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";

type ToneSharingUser = {
  id: string;
  email: string;
  role: string;
};

type ToneSharingItem = {
  id: string;
  title: string;
  type: string;
  creatorUserId?: string | null;
  moderationStatus?: string;
  creatorEmail?: string | null;
  creatorDisplayName?: string | null;
  creatorHandle?: string | null;
  creatorProfileHandle?: string | null;
  profileHandle?: string | null;
  favoriteCount?: number;
  ratingCount?: number;
  averageRating?: number | null;
  currentUserFavorite?: boolean;
  currentUserRating?: number | null;
  description?: string | null;
  tags?: string[] | null;
};

type ToneSharingPack = {
  id: string;
  title: string;
  creatorUserId?: string | null;
  moderationStatus?: string;
  creatorEmail?: string | null;
  creatorDisplayName?: string | null;
  creatorHandle?: string | null;
  creatorProfileHandle?: string | null;
  profileHandle?: string | null;
  description?: string | null;
  thumbnailUrl?: string | null;
  thumbnailAssetId?: string | null;
};

type ToneSharingPackDetails = {
  pack: ToneSharingPack;
  items: Array<{ itemId: string; sortOrder: number; title: string; type: string; moderationStatus?: string; description?: string | null; tags?: string[] | null }>;
};

type ItemArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
};

type Tone3000ResourceRef = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  toneId?: string;
  modelId?: string;
  modelUrl?: string;
  creatorId?: string;
  creatorName?: string;
};

type ItemArchive = {
  formatVersion: number;
  preset: Record<string, unknown>;
  resources: ItemArchiveResource[];
  blends?: Array<Record<string, unknown>>;
  tone3000Resources?: Tone3000ResourceRef[];
};

type ItemCollectionArchive = {
  formatVersion: number;
  createdAt: string;
  presets: Array<Record<string, unknown>>;
  resources: ItemArchiveResource[];
  blends?: Array<Record<string, unknown>>;
  tone3000Resources?: Tone3000ResourceRef[];
};

type ShareConsentStatus = {
  consentType: string;
  version: number;
  accepted: boolean;
  acceptedAt: string | null;
};

type ToneSharingRow = {
  id: string;
  slug: string;
  title: string;
  items: Array<{
    id: string;
    kind: "item" | "pack";
    title: string;
    type: string | null;
    moderationStatus?: string;
    creatorEmail?: string | null;
    creatorDisplayName?: string | null;
    favoriteCount?: number;
    ratingCount?: number;
    averageRating?: number | null;
    currentUserFavorite?: boolean;
    currentUserRating?: number | null;
    description?: string | null;
    tags?: string[] | null;
    thumbnailUrl?: string | null;
    thumbnailAssetId?: string | null;
  }>;
};

type InstalledPackSource = "zipImport" | "toneSharingApi" | "generatedPack";

type InstalledPackResourceRef = {
  type: string;
  id: string;
};

export type InstalledPackMetadata = {
  id: string;
  title: string;
  source: InstalledPackSource;
  importedAt: string;
  packId?: string;
  archivePath?: string;
  archiveFileName?: string;
  presetIds: string[];
  resources: InstalledPackResourceRef[];
};

const storageKeys = {
  apiBase: "toneSharing.apiBase",
  sessionId: "toneSharing.sessionId",
  installedPacks: "toneSharing.installedPacks",
  publishConsent: "toneSharing.publishConsent"
};

const TONE_SHARING_PUBLISH_CONSENT_VERSION = 1;

const state = {
  apiBase: "https://api-guitar.soundshed.com/v1", //"http://127.0.0.1:8787/v1", 
  sessionId: "",
  user: null as ToneSharingUser | null,
  myItems: [] as ToneSharingItem[],
  installedPacks: [] as InstalledPackMetadata[]
};

const packThumbnailObjectUrls = new Map<string, string>();

let browseMode: "featured" | "items" | "packs" | "installed" | "mine" | "ai-search" | "review" = "featured";
let publishItemInFlight = false;
let publishPackInFlight = false;

type AiToneEffect = { type: string; name: string; settings?: Record<string, string | number> };

type AiToneCombination = {
  name: string;
  description: string;
  amp: string;
  cabinet: string;
  pedals: string[];
  effects: AiToneEffect[];
};

type AiToneSearchResult = {
  query: { band?: string; song?: string };
  combinations: AiToneCombination[];
  generatedAt: string;
};

const aiSearchState = {
  band: "",
  song: "",
  combinations: [] as AiToneCombination[],
  loading: false,
  error: ""
};
let editingPackId: string | null = null;
let previewingItemId: string | null = null;
let previewingItemTitle = "";
let previewPriorPresetId: string | null = null;

type ToneSharingShareTarget = {
  kind: "item" | "pack";
  id: string;
};

function element<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function setText(id: string, value: string): void {
  const target = element<HTMLElement>(id);
  if (target) {
    target.textContent = value;
  }
}

function setUploadStatus(value: string): void {
  setText("tone-sharing-upload-status", value);
  setText("tone-sharing-publish-modal-status", value);
}

function getApiOrigin(): string {
  try {
    return new URL(state.apiBase).origin;
  } catch {
    return "https://api-guitar.soundshed.com";
  }
}

function buildToneSharingShareLink(target: ToneSharingShareTarget): string {
  const kindPath = target.kind === "pack" ? "pack" : "item";
  return `${getApiOrigin()}/share/${kindPath}/${encodeURIComponent(target.id)}`;
}

async function copyTextToClipboard(value: string): Promise<void> {
  if (navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(value);
    return;
  }

  const textarea = document.createElement("textarea");
  textarea.value = value;
  textarea.setAttribute("readonly", "readonly");
  textarea.style.position = "fixed";
  textarea.style.left = "-9999px";
  document.body.appendChild(textarea);
  textarea.select();
  const copied = document.execCommand("copy");
  document.body.removeChild(textarea);
  if (!copied) {
    throw new Error("Clipboard unavailable");
  }
}

async function copyToneSharingShareLink(target: ToneSharingShareTarget): Promise<void> {
  await copyTextToClipboard(buildToneSharingShareLink(target));
}

function isToneSharingAdmin(): boolean {
  return state.user?.role === "admin";
}

function getPresetToneSharingOrigin(preset: Preset | null | undefined): { itemId: string; republishBlocked: boolean } | null {
  if (!preset || !preset.toneSharingOrigin || typeof preset.toneSharingOrigin !== "object") {
    return null;
  }
  const origin = preset.toneSharingOrigin;
  if (origin.source !== "toneSharingApi") {
    return null;
  }
  const itemId = origin.itemId.trim();
  if (!itemId) {
    return null;
  }
  return {
    itemId,
    republishBlocked: origin.republishBlocked !== false,
  };
}

function setPublishItemBusy(busy: boolean, message?: string): void {
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

function setPublishPackBusy(busy: boolean, message?: string): void {
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
    checkbox.disabled = busy;
  });
  if (progress) {
    progress.style.display = busy ? "flex" : "none";
    const label = progress.querySelector<HTMLElement>(".tone-sharing-progress-label");
    if (label) {
      label.textContent = message ?? "Working...";
    }
  }
}

function normalizeSettingString(value: unknown): string {
  return typeof value === "string" ? value.trim() : "";
}

function persistToneSharingApiBase(value: string): void {
  localStorage.setItem(storageKeys.apiBase, value);
  setAppSetting(storageKeys.apiBase, value);
}

function persistToneSharingSession(value: string): void {
  if (value) {
    localStorage.setItem(storageKeys.sessionId, value);
    setAppSetting(storageKeys.sessionId, value);
    return;
  }
  localStorage.removeItem(storageKeys.sessionId);
  setAppSetting(storageKeys.sessionId, null);
}

function normalizeInstalledPackMetadata(raw: unknown): InstalledPackMetadata | null {
  if (!raw || typeof raw !== "object") {
    return null;
  }

  const value = raw as Record<string, unknown>;
  const id = typeof value.id === "string" ? value.id.trim() : "";
  const title = typeof value.title === "string" ? value.title.trim() : "";
  const source = value.source === "zipImport" || value.source === "toneSharingApi" || value.source === "generatedPack"
    ? value.source
    : "zipImport";
  const importedAt = typeof value.importedAt === "string" && value.importedAt
    ? value.importedAt
    : new Date().toISOString();
  const presetIds = Array.isArray(value.presetIds)
    ? value.presetIds.filter((entry): entry is string => typeof entry === "string" && entry.trim().length > 0)
    : [];
  const resources = Array.isArray(value.resources)
    ? value.resources
        .filter((entry) => entry && typeof entry === "object")
        .map((entry) => {
          const resource = entry as Record<string, unknown>;
          return {
            type: typeof resource.type === "string" ? resource.type.trim() : "",
            id: typeof resource.id === "string" ? resource.id.trim() : "",
          };
        })
        .filter((entry) => entry.type.length > 0 && entry.id.length > 0)
    : [];

  if (!id || !title) {
    return null;
  }

  return {
    id,
    title,
    source,
    importedAt,
    packId: typeof value.packId === "string" && value.packId ? value.packId : undefined,
    archivePath: typeof value.archivePath === "string" && value.archivePath ? value.archivePath : undefined,
    archiveFileName: typeof value.archiveFileName === "string" && value.archiveFileName ? value.archiveFileName : undefined,
    presetIds: Array.from(new Set(presetIds)),
    resources,
  };
}

function persistInstalledPacks(): void {
  const serialized = JSON.stringify(state.installedPacks);
  localStorage.setItem(storageKeys.installedPacks, serialized);
  setAppSetting(storageKeys.installedPacks, state.installedPacks);
}

function mergeInstalledPackMetadata(entry: InstalledPackMetadata): void {
  const existingIndex = state.installedPacks.findIndex((pack) => pack.id === entry.id);
  if (existingIndex >= 0) {
    state.installedPacks[existingIndex] = entry;
  } else {
    state.installedPacks.unshift(entry);
  }
  persistInstalledPacks();
}

export function getApiBaseUrl(): string {
  return state.apiBase;
}

export function registerInstalledToneSharingPack(entry: InstalledPackMetadata): void {
  mergeInstalledPackMetadata(entry);
  if (browseMode === "installed") {
    void renderInstalledPacks();
  }
}

export function registerInstalledToneSharingPackFromImport(info: {
  packId?: string;
  fileName?: string;
  path?: string;
}): void {
  const packId = info.packId?.trim() ?? "";
  const fileName = info.fileName?.trim() || (packId ? `tone-sharing-pack-${packId}.zip` : "tone-sharing-pack.zip");
  const generatedId = packId
    ? `tone-sharing-api:${packId}`
    : `tone-sharing-api:${fileName.toLowerCase()}`;

  registerInstalledToneSharingPack({
    id: generatedId,
    title: fileName.replace(/\.zip$/i, ""),
    source: "toneSharingApi",
    importedAt: new Date().toISOString(),
    packId: packId || undefined,
    archivePath: info.path?.trim() || undefined,
    archiveFileName: fileName,
    presetIds: [],
    resources: [],
  });
}

export function isToneSharingSignedIn(): boolean {
  return Boolean(state.user);
}

export async function syncToneSharingFavoriteForPreset(preset: Preset | null | undefined, favorite: boolean): Promise<void> {
  if (!state.user) {
    return;
  }
  const origin = getPresetToneSharingOrigin(preset);
  if (!origin?.itemId) {
    return;
  }
  await apiFetch(`/items/${origin.itemId}/favorite`, {
    method: favorite ? "PUT" : "DELETE",
  });
}

export async function syncToneSharingRatingForPreset(preset: Preset | null | undefined, rating: number | null): Promise<void> {
  if (!state.user) {
    return;
  }
  const origin = getPresetToneSharingOrigin(preset);
  if (!origin?.itemId) {
    return;
  }
  if (rating === null) {
    await apiFetch(`/items/${origin.itemId}/rating`, { method: "DELETE" });
    return;
  }
  await apiFetch(`/items/${origin.itemId}/rating`, {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ rating }),
  });
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
  if (publishButton) {
    publishButton.disabled = false;
    publishButton.title = "";
  }
  if (origin?.republishBlocked) {
    setUploadStatus("This preset came from Tone Sharing. Use Save As to create a local copy before publishing.");
    if (publishButton) {
      publishButton.disabled = true;
      publishButton.title = "Use Save As to create a new local preset before publishing";
    }
  }

  // Reset pack assign UI
  element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.remove("active");
  const packSelect = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  if (packSelect) packSelect.value = "";
  void loadMyDraftPacksForSelect();

  modal.style.display = "flex";
}

function closeToneSharingPublishPresetModal(force = false): void {
  if (publishItemInFlight && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-publish-modal");
  if (!modal) {
    return;
  }
  modal.style.display = "none";
}

function openSignInModal(): void {
  const modal = element<HTMLElement>("tone-sharing-signin-modal");
  if (modal) {
    modal.style.display = "flex";
  }
}

function closeSignInModal(): void {
  const modal = element<HTMLElement>("tone-sharing-signin-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

async function openPackModal(packId?: string, preCheckItemId?: string): Promise<void> {
  editingPackId = packId ?? null;
  const modal = element<HTMLElement>("tone-sharing-pack-modal");
  if (!modal) {
    return;
  }
  setPublishPackBusy(false, "Publishing pack...");

  const titleEl = element<HTMLInputElement>("tone-sharing-pack-title");
  const descEl = element<HTMLTextAreaElement>("tone-sharing-pack-description");
  const imageEl = element<HTMLInputElement>("tone-sharing-pack-image");
  const titleHeader = element<HTMLElement>("tone-sharing-pack-modal-title");

  if (titleEl) titleEl.value = "";
  if (descEl) descEl.value = "";
  if (imageEl) imageEl.value = "";
  setText("tone-sharing-pack-status", "");

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

  renderPackItemSelection(state.myItems, checkedItemIds);
  setText("tone-sharing-pack-status", "");
  modal.style.display = "flex";
}

function closePackModal(force = false): void {
  if (publishPackInFlight && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-pack-modal");
  if (modal) {
    modal.style.display = "none";
  }
  editingPackId = null;
}

async function loadMyDraftPacksForSelect(): Promise<void> {
  const select = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  if (!select || !state.user) {
    return;
  }
  try {
    const data = await apiFetch<{ packs: ToneSharingPack[] }>("/packs/me/list");
    const drafts = data.packs.filter((p) => p.moderationStatus === "draft");
    select.innerHTML =
      `<option value="">Or add to existing draft pack…</option>` +
      drafts.map((p) => `<option value="${p.id}">${p.title}</option>`).join("");
  } catch {
  }
}

async function addItemToExistingPack(packId: string, itemId: string): Promise<void> {
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

async function savePack(publish: boolean): Promise<void> {
  if (publishPackInFlight) {
    return;
  }
  if (!state.user) {
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

  if (publish) {
    setPublishPackBusy(true, editingPackId ? "Submitting pack for approval..." : "Publishing pack...");
  }

  const itemIds = Array.from(document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[data-pack-item-id]:checked"))
    .map((input) => input.dataset.packItemId ?? "")
    .filter(Boolean);

  if (itemIds.length === 0) {
    setText("tone-sharing-pack-status", "Select at least one preset.");
    if (publish) {
      setPublishPackBusy(false, "Publishing pack...");
    }
    return;
  }

  setText("tone-sharing-pack-status", editingPackId ? "Saving..." : "Creating pack...");

  try {
    let thumbnailAssetId: string | undefined;
    if (imageFile) {
      const variants = await buildPackImageVariants(imageFile);
      const thumbnailBlob = variants.small.blob;
      const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          kind: "thumbnail",
          mimeType: thumbnailBlob.type || "application/octet-stream",
          byteSize: thumbnailBlob.size
        })
      });

      const uploadResponse = await fetch(buildApiUrl(`/uploads/${init.uploadId}`), {
        method: "PUT",
        headers: {
          "content-type": thumbnailBlob.type || "application/octet-stream",
          ...(state.sessionId ? { "x-session-id": state.sessionId } : {})
        },
        body: thumbnailBlob,
        credentials: "include"
      });
      const uploadPayload = await uploadResponse.json().catch(() => null);
      if (!uploadResponse.ok || uploadPayload?.ok === false) {
        throw new Error(uploadPayload?.error?.message || `Pack image upload failed (${uploadResponse.status})`);
      }

      const complete = await apiFetch<{ assetId: string }>("/uploads/complete", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ uploadId: init.uploadId })
      });
      thumbnailAssetId = complete.assetId;
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
      const pack = await apiFetch<{ pack: ToneSharingPack }>("/packs", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, thumbnailAssetId })
      });
      packId = pack.pack.id;
      editingPackId = packId;
    }

    await apiFetch(`/packs/${packId}/items`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ itemIds })
    });

    if (publish) {
      await apiFetch(`/packs/${packId}/publish`, { method: "POST" });
      setText("tone-sharing-pack-status", "Pack submitted for moderator approval.");
      setPublishPackBusy(false, "Publishing pack...");
      closePackModal(true);
    } else {
      setText("tone-sharing-pack-status", "Draft saved.");
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-pack-status", `${publish ? "Publish" : "Save"} failed: ${(error as Error).message}`);
  } finally {
    if (publish) {
      setPublishPackBusy(false, "Publishing pack...");
    }
  }
}

function updateAuthButtonVisibility(): void {
  const signInButton = element<HTMLButtonElement>("tone-sharing-verify");
  const signOutButton = element<HTMLButtonElement>("tone-sharing-logout");
  const accountChip = element<HTMLButtonElement>("tone-sharing-account-btn");
  const createPackButton = element<HTMLButtonElement>("tone-sharing-open-pack-modal");
  const signedIn = !!state.user;

  if (signInButton) {
    signInButton.style.display = signedIn ? "none" : "";
  }
  if (signOutButton) {
    signOutButton.style.display = signedIn ? "" : "none";
  }
  if (accountChip) {
    accountChip.textContent = signedIn ? (state.user?.email ?? "Account") : "Sign In";
    accountChip.classList.toggle("signed-in", signedIn);
  }
  if (createPackButton) {
    createPackButton.disabled = !signedIn;
    createPackButton.title = signedIn ? "Create a new pack" : "Sign in to create packs";
  }
  const reviewButton = element<HTMLButtonElement>("tone-sharing-browse-review");
  if (reviewButton) {
    reviewButton.style.display = isToneSharingAdmin() ? "" : "none";
  }
  if (!isToneSharingAdmin() && browseMode === "review") {
    browseMode = "featured";
  }
  if (!signedIn) {
    closeToneSharingPublishPresetModal();
    closePackModal();
  }
}

function normalizeBase(input: string): string {
  const trimmed = input.trim();
  if (!trimmed) {
    return "https://api.guitar.soundshed.com/v1";
  }
  return trimmed.endsWith("/") ? trimmed.slice(0, -1) : trimmed;
}

function normalizeCreatorHandle(raw: unknown): string | null {
  if (typeof raw !== "string") {
    return null;
  }

  const trimmed = raw.trim();
  if (!trimmed) {
    return null;
  }

  const withoutPrefix = trimmed.replace(/^@+/, "");
  if (!withoutPrefix || /\s/.test(withoutPrefix)) {
    return null;
  }

  if (!/^[A-Za-z0-9._-]{2,64}$/.test(withoutPrefix)) {
    return null;
  }

  return `@${withoutPrefix}`;
}

function resolveCreatorProfileHandle(item: Record<string, unknown>): string | null {
  const explicit = [
    item.creatorProfileHandle,
    item.creatorHandle,
    item.profileHandle,
    item.handle,
  ];

  for (const candidate of explicit) {
    const handle = normalizeCreatorHandle(candidate);
    if (handle) {
      return handle;
    }
  }

  const displayName = typeof item.creatorDisplayName === "string" ? item.creatorDisplayName.trim() : "";
  if (displayName.startsWith("@")) {
    return normalizeCreatorHandle(displayName);
  }

  return null;
}

function sanitizePublishFileName(raw: string): string {
  const cleaned = raw.trim().replace(/[^a-z0-9\-_. ]/gi, "").replace(/\s+/g, "-").replace(/\.+$/, "");
  return cleaned || "preset";
}

function buildApiUrl(pathOrUrl: string): string {
  if (/^https?:\/\//i.test(pathOrUrl)) {
    return pathOrUrl;
  }

  const base = state.apiBase.endsWith("/") ? state.apiBase.slice(0, -1) : state.apiBase;
  const normalizedPath = pathOrUrl.startsWith("/") ? pathOrUrl : `/${pathOrUrl}`;

  try {
    const parsedBase = new URL(base);
    const basePath = parsedBase.pathname.replace(/\/+$/, "");
    const pathAlreadyIncludesBase =
      !!basePath &&
      basePath !== "/" &&
      (normalizedPath === basePath || normalizedPath.startsWith(`${basePath}/`));

    if (pathAlreadyIncludesBase) {
      return `${parsedBase.origin}${normalizedPath}`;
    }
  } catch {
  }

  return `${base}${normalizedPath}`;
}

function clearPackThumbnailObjectUrls(): void {
  for (const objectUrl of packThumbnailObjectUrls.values()) {
    URL.revokeObjectURL(objectUrl);
  }
  packThumbnailObjectUrls.clear();
}

type ResizedImageResult = {
  blob: Blob;
  width: number;
  height: number;
};

async function resizeImageToMaxWidth(source: Blob, maxWidth: number): Promise<ResizedImageResult> {
  const sourceUrl = URL.createObjectURL(source);
  try {
    const image = await new Promise<HTMLImageElement>((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = () => reject(new Error("Failed to load image"));
      img.src = sourceUrl;
    });

    const targetWidth = Math.max(1, Math.min(maxWidth, image.naturalWidth));
    const targetHeight = Math.max(1, Math.round((targetWidth / image.naturalWidth) * image.naturalHeight));

    const canvas = document.createElement("canvas");
    canvas.width = targetWidth;
    canvas.height = targetHeight;
    const context = canvas.getContext("2d");
    if (!context) {
      throw new Error("Canvas context unavailable");
    }

    context.drawImage(image, 0, 0, targetWidth, targetHeight);

    const outputType = source.type === "image/png" || source.type === "image/webp" || source.type === "image/jpeg"
      ? source.type
      : "image/jpeg";

    const blob = await new Promise<Blob>((resolve, reject) => {
      canvas.toBlob(
        (value) => {
          if (!value) {
            reject(new Error("Failed to encode resized image"));
            return;
          }
          resolve(value);
        },
        outputType,
        outputType === "image/png" ? undefined : 0.9
      );
    });

    return {
      blob,
      width: targetWidth,
      height: targetHeight
    };
  } finally {
    URL.revokeObjectURL(sourceUrl);
  }
}

async function buildPackImageVariants(source: File): Promise<{
  small: ResizedImageResult;
  large: ResizedImageResult;
}> {
  const large = await resizeImageToMaxWidth(source, 2048);
  const small = await resizeImageToMaxWidth(large.blob, 512);
  return { small, large };
}

async function resolvePackThumbnailUrl(pack: ToneSharingPack): Promise<string> {
  const thumbnailPath = pack.thumbnailUrl?.trim();
  if (!thumbnailPath) {
    return "";
  }

  const cacheKey = `${state.apiBase}|${pack.id}|${thumbnailPath}`;
  const cached = packThumbnailObjectUrls.get(cacheKey);
  if (cached) {
    return cached;
  }

  const response = await fetch(buildApiUrl(thumbnailPath), {
    headers: state.sessionId ? { "x-session-id": state.sessionId } : {},
    credentials: "include"
  });
  if (!response.ok) {
    throw new Error(`Thumbnail load failed (${response.status})`);
  }

  const blob = await response.blob();
  const objectUrl = URL.createObjectURL(blob);
  packThumbnailObjectUrls.set(cacheKey, objectUrl);
  return objectUrl;
}

async function apiFetch<T = unknown>(path: string, init: RequestInit = {}): Promise<T> {
  const headers = new Headers(init.headers ?? {});
  if (state.sessionId) {
    headers.set("x-session-id", state.sessionId);
  }
  const response = await fetch(`${state.apiBase}${path}`, {
    ...init,
    headers,
    credentials: "include"
  });

  const payload = await response.json().catch(() => null);
  if (!response.ok || !payload || payload.ok === false) {
    const message = payload?.error?.message || `Request failed (${response.status})`;
    throw new Error(message);
  }

  return payload.data as T;
}

function getLocalPublishConsent(): { version: number; acceptedAt: string; userId?: string } | null {
  const raw = uiState.appSettings?.[storageKeys.publishConsent];
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) {
    return null;
  }

  const value = raw as Record<string, unknown>;
  const version = Number(value.version ?? 0);
  const acceptedAt = typeof value.acceptedAt === "string" ? value.acceptedAt : "";
  const userId = typeof value.userId === "string" ? value.userId : undefined;
  if (!acceptedAt || !Number.isFinite(version)) {
    return null;
  }

  return { version, acceptedAt, userId };
}

function persistLocalPublishConsent(status: ShareConsentStatus): void {
  const value = {
    version: status.version,
    acceptedAt: status.acceptedAt ?? new Date().toISOString(),
    userId: state.user?.id ?? "",
  };
  uiState.appSettings[storageKeys.publishConsent] = value;
  setAppSetting(storageKeys.publishConsent, value);
}

let shareConsentResolve: ((value: boolean) => void) | null = null;

function closeShareConsentModal(result: boolean): void {
  const modal = element<HTMLElement>("tone-sharing-consent-modal");
  const checkbox = element<HTMLInputElement>("tone-sharing-consent-checkbox");
  const status = element<HTMLElement>("tone-sharing-consent-status");
  if (modal) {
    modal.style.display = "none";
  }
  if (checkbox) {
    checkbox.checked = false;
  }
  if (status) {
    status.textContent = "";
  }
  if (shareConsentResolve) {
    shareConsentResolve(result);
    shareConsentResolve = null;
  }
}

async function promptForShareConsent(): Promise<boolean> {
  const modal = element<HTMLElement>("tone-sharing-consent-modal");
  const checkbox = element<HTMLInputElement>("tone-sharing-consent-checkbox");
  const acceptButton = element<HTMLButtonElement>("tone-sharing-consent-accept");
  const cancelButton = element<HTMLButtonElement>("tone-sharing-consent-cancel");
  const closeButton = element<HTMLButtonElement>("tone-sharing-consent-close");
  const status = element<HTMLElement>("tone-sharing-consent-status");
  if (!modal || !checkbox || !acceptButton || !cancelButton || !closeButton || !status) {
    throw new Error("Tone sharing consent modal is not available");
  }

  if (shareConsentResolve) {
    shareConsentResolve(false);
    shareConsentResolve = null;
  }

  checkbox.checked = false;
  acceptButton.disabled = true;
  status.textContent = "";
  modal.style.display = "flex";

  return new Promise<boolean>((resolve) => {
    shareConsentResolve = resolve;
    checkbox.onchange = () => {
      acceptButton.disabled = !checkbox.checked;
    };
    acceptButton.onclick = () => closeShareConsentModal(true);
    cancelButton.onclick = () => closeShareConsentModal(false);
    closeButton.onclick = () => closeShareConsentModal(false);
    modal.onmousedown = (event) => {
      if (event.target === modal) {
        closeShareConsentModal(false);
      }
    };
  });
}

async function ensurePublishConsent(): Promise<void> {
  if (!state.user) {
    throw new Error("Sign in first.");
  }

  const remote = await apiFetch<ShareConsentStatus>("/share-consent/status");
  const local = getLocalPublishConsent();
  const localAccepted = Boolean(
    local && local.version >= remote.version && (!local.userId || local.userId === state.user.id)
  );

  if (remote.accepted) {
    if (!localAccepted) {
      persistLocalPublishConsent(remote);
    }
    return;
  }

  const accepted = await promptForShareConsent();
  if (!accepted) {
    throw new Error("Tone sharing consent is required before publishing.");
  }

  const stored = await apiFetch<ShareConsentStatus>("/share-consent/accept", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ version: TONE_SHARING_PUBLISH_CONSENT_VERSION }),
  });
  persistLocalPublishConsent(stored);
}

async function loadAuthSession(): Promise<void> {
  try {
    const data = await apiFetch<{ user: ToneSharingUser | null }>("/auth/me");
    state.user = data.user;
    updateAuthButtonVisibility();
    if (data.user) {
      setText("tone-sharing-auth-status", `Signed in as ${data.user.email}`);
      await loadMine();
    } else {
      setText("tone-sharing-auth-status", "Signed out");
    }
  } catch (error) {
    setText("tone-sharing-auth-status", `Auth check failed: ${(error as Error).message}`);
  }
}

async function sendCode(): Promise<void> {
  const email = element<HTMLInputElement>("tone-sharing-email")?.value.trim() ?? "";
  if (!email) {
    setText("tone-sharing-auth-status", "Enter an email address");
    return;
  }

  setText("tone-sharing-auth-status", "Sending code...");
  try {
    await apiFetch("/auth/start", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ email })
    });
    setText("tone-sharing-auth-status", "Code sent. Check your email.");
  } catch (error) {
    setText("tone-sharing-auth-status", `Send code failed: ${(error as Error).message}`);
  }
}

async function verifyCode(): Promise<void> {
  const email = element<HTMLInputElement>("tone-sharing-email")?.value.trim() ?? "";
  const code = element<HTMLInputElement>("tone-sharing-code")?.value.trim() ?? "";
  if (!email || !code) {
    setText("tone-sharing-auth-status", "Enter email and code");
    return;
  }

  setText("tone-sharing-auth-status", "Signing in...");
  try {
    const data = await apiFetch<{ sessionId?: string; user: ToneSharingUser }>("/auth/verify", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ email, code })
    });

    state.user = data.user;
    state.sessionId = data.sessionId ?? "";
    updateAuthButtonVisibility();
    persistToneSharingSession(state.sessionId);
    setText("tone-sharing-auth-status", `Signed in as ${data.user.email}`);
    closeSignInModal();
    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-auth-status", `Sign-in failed: ${(error as Error).message}`);
  }
}

async function signOut(): Promise<void> {
  try {
    await apiFetch("/auth/logout", { method: "POST" });
  } catch {
  }

  state.sessionId = "";
  state.user = null;
  updateAuthButtonVisibility();
  persistToneSharingSession("");
  setText("tone-sharing-auth-status", "Signed out");
  closeSignInModal();
  await loadBrowse();
}

async function renderFeedRows(rows: ToneSharingRow[]): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  const useTiledLayout = browseMode === "featured" || browseMode === "items";
  feed.classList.toggle("tone-sharing-feed--tiled", useTiledLayout);

  if (!rows.length) {
    feed.innerHTML = `<div class="tone-sharing-status">No content yet. Publish the first tone.</div>`;
    return;
  }

  const rowHtml = await Promise.all(
    rows.map(async (row) => {
      const itemHtml = await Promise.all(
        row.items.map(async (item) => {
          let cardClass = "tone-sharing-card-item";
          let backgroundStyle = "";

          if (item.kind === "pack") {
            cardClass += " tone-sharing-pack-hero";
            const packForThumbnail: ToneSharingPack = {
              id: item.id,
              title: item.title,
              thumbnailUrl: item.thumbnailUrl ?? (item.thumbnailAssetId ? `/packs/${item.id}/thumbnail` : null)
            };
            if (packForThumbnail.thumbnailUrl) {
              try {
                const thumbnailObjectUrl = await resolvePackThumbnailUrl(packForThumbnail);
                backgroundStyle = ` style=\"background-image: linear-gradient(180deg, rgba(5, 6, 12, 0.08) 0%, rgba(5, 6, 12, 0.9) 70%, rgba(5, 6, 12, 0.98) 100%), url('${thumbnailObjectUrl}')\"`;
              } catch {
                backgroundStyle = "";
              }
            }
          }

          const isPreviewing = previewingItemId === item.id;
          const accentStyleAttr = item.kind === "item"
            ? ` style="border-left: 3px solid ${idAccentColor(item.id)}"`
            : "";
          const reviewMode = browseMode === "review";
          const creatorHandle = resolveCreatorProfileHandle(item as unknown as Record<string, unknown>);
          const typeLabel = item.kind === "item" ? (item.type ?? "preset") : "pack";
          const metaText = [typeLabel, creatorHandle].filter((value): value is string => Boolean(value)).join(" · ");
          return `
                  <div class=\"${cardClass}${isPreviewing ? " is-previewing" : ""}\" data-kind=\"${item.kind}\" data-id=\"${item.id}\" data-title=\"${item.title}\"${backgroundStyle}${accentStyleAttr}>
                    <div class=\"tone-sharing-card-item-content\">
                      <div class=\"tone-sharing-card-item-title\">${item.title}</div>
                      <div class=\"tone-sharing-card-item-meta\">${escapeHtml(metaText)}</div>
                      ${item.description ? `<div class=\"tone-sharing-card-item-description\">${item.description}</div>` : ""}
                      ${item.kind === "item" && item.tags && item.tags.length > 0 ? `<div class=\"tone-sharing-card-tags\">${item.tags.map((t) => `<span class=\"tone-sharing-tag-badge\">${t}</span>`).join("")}</div>` : ""}
                    </div>
                    <div class=\"tone-sharing-card-item-actions\">
                      ${item.kind === "item" ? `
                        <button class=\"btn btn-secondary tone-sharing-card-btn\" type=\"button\" data-action=\"preview\">
                          <svg class=\"tone-sharing-btn-icon\" viewBox=\"0 0 16 16\" fill=\"currentColor\" aria-hidden=\"true\"><polygon points=\"3,2 14,8 3,14\"/></svg>
                          ${isPreviewing ? "Previewing" : "Preview"}
                        </button>` : `
                        <button class=\"btn btn-secondary tone-sharing-card-btn\" type=\"button\" data-action=\"view\">View</button>`}
                      <button class=\"btn btn-primary tone-sharing-card-btn\" type=\"button\" data-action=\"download\">
                        <svg class=\"tone-sharing-btn-icon\" viewBox=\"0 0 16 16\" fill=\"currentColor\" aria-hidden=\"true\"><path d=\"M8 1a1 1 0 011 1v6.172l1.586-1.586a1 1 0 111.414 1.414l-3.293 3.293a1 1 0 01-1.414 0L3.999 8.001a1 1 0 111.414-1.414L7.001 8.172V2A1 1 0 018 1z\"/><rect x=\"2\" y=\"12.5\" width=\"12\" height=\"2\" rx=\"1\"/></svg>
                        Download
                      </button>
                      <button class=\"btn btn-secondary tone-sharing-card-btn\" type=\"button\" data-action=\"share\">Share</button>
                      ${reviewMode ? `<button class=\"btn btn-primary tone-sharing-card-btn\" type=\"button\" data-action=\"approve\">Approve</button>
                      <button class=\"btn btn-secondary tone-sharing-card-btn\" type=\"button\" data-action=\"reject\">Reject</button>` : ""}
                      ${browseMode === "mine" && item.kind === "pack" ? `<button class=\"btn btn-secondary tone-sharing-card-btn\" type=\"button\" data-action=\"edit-pack\">Edit</button>` : ""}
                      ${browseMode === "mine" ? `<button class=\"btn btn-secondary tone-sharing-card-btn\" type=\"button\" data-action=\"delete\">Delete</button>` : ""}
                    </div>
                  </div>
                `;
        })
      );

      return `
        <div class="tone-sharing-row">
          <div class="tone-sharing-row-title">${row.title}</div>
          <div class="tone-sharing-row-track">
            ${itemHtml.join("")}
          </div>
        </div>
      `;
    })
  );

  feed.innerHTML = rowHtml.join("");
}

function buildSingleRow(title: string, entries: Array<{ id: string; title: string; type?: string }>, kind: "item" | "pack"): ToneSharingRow {
  return {
    id: `generated-${title.toLowerCase().replace(/\s+/g, "-")}`,
    slug: `generated-${title.toLowerCase().replace(/\s+/g, "-")}`,
    title,
    items: entries.map((entry) => ({
      id: entry.id,
      kind,
      title: entry.title,
      type: entry.type ?? null,
      moderationStatus: (entry as ToneSharingItem | ToneSharingPack).moderationStatus,
      creatorEmail: (entry as ToneSharingItem | ToneSharingPack).creatorEmail ?? null,
      creatorDisplayName: (entry as ToneSharingItem | ToneSharingPack).creatorDisplayName ?? null,
      creatorHandle: (entry as ToneSharingItem | ToneSharingPack).creatorHandle ?? null,
      creatorProfileHandle: (entry as ToneSharingItem | ToneSharingPack).creatorProfileHandle ?? null,
      profileHandle: (entry as ToneSharingItem | ToneSharingPack).profileHandle ?? null,
      favoriteCount: kind === "item" ? (entry as ToneSharingItem).favoriteCount : undefined,
      ratingCount: kind === "item" ? (entry as ToneSharingItem).ratingCount : undefined,
      averageRating: kind === "item" ? (entry as ToneSharingItem).averageRating ?? null : undefined,
      currentUserFavorite: kind === "item" ? (entry as ToneSharingItem).currentUserFavorite : undefined,
      currentUserRating: kind === "item" ? (entry as ToneSharingItem).currentUserRating ?? null : undefined,
      description: kind === "pack" ? (entry as ToneSharingPack).description ?? null : (entry as ToneSharingItem).description ?? null,
      tags: kind === "item" ? (entry as ToneSharingItem).tags ?? null : null,
      thumbnailUrl: kind === "pack"
        ? ((entry as ToneSharingPack).thumbnailUrl ?? ((entry as ToneSharingPack).thumbnailAssetId ? `/packs/${entry.id}/thumbnail` : null))
        : null
    }))
  };
}

async function moderateTarget(kind: "item" | "pack", id: string, action: "approve" | "reject"): Promise<void> {
  const notes = action === "reject"
    ? (window.prompt("Optional rejection reason", "") ?? "").trim()
    : "";
  const endpoint = kind === "item" ? `/items/${id}/moderate` : `/packs/${id}/moderate`;
  await apiFetch(endpoint, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ action, notes: notes || undefined }),
  });
}

function clearPackDetail(): void {
  const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

// ===== AI Tone Search =====

function gearLinkButton(label: string, category: string): string {
  return `<button type="button" class="ai-tone-gear-link" data-t3k-query="${escapeHtml(label)}" data-t3k-category="${escapeHtml(category)}" title="Search Tone3000 for \u201c${escapeHtml(label)}\u201d">${escapeHtml(label)}</button>`;
}

function renderAiCombinations(): string {
  if (!aiSearchState.combinations.length) return "";

  const parts: string[] = [];
  if (aiSearchState.band) parts.push(aiSearchState.band);
  if (aiSearchState.song) parts.push(`"${aiSearchState.song}"`);
  const heading = parts.length ? `AI Tone Analysis: ${parts.join(" \u2014 ")}` : "AI Tone Analysis";

  const cards = aiSearchState.combinations.map((combo) => {
    const pedalsList = combo.pedals.length
      ? combo.pedals.map((p) => gearLinkButton(p, "pedal")).join("")
      : `<span class="ai-tone-tag ai-tone-tag--empty">None</span>`;
    const effectsList = combo.effects.length
      ? combo.effects.map((e) => gearLinkButton(e.name, "pedal")).join("")
      : `<span class="ai-tone-tag ai-tone-tag--empty">None</span>`;
    const libraryQuery = [combo.amp, ...(aiSearchState.band ? [aiSearchState.band] : [])].filter(Boolean).join(" ");
    return `
      <div class="ai-tone-combo-card">
        <div class="ai-tone-combo-header">
          <div class="ai-tone-combo-name">${escapeHtml(combo.name)}</div>
          <div class="ai-tone-combo-desc">${escapeHtml(combo.description)}</div>
        </div>
        <div class="ai-tone-combo-gear">
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Amp</span>
            <div class="ai-tone-tags">${gearLinkButton(combo.amp, "amp")}</div>
          </div>
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Cabinet</span>
            <div class="ai-tone-tags">${gearLinkButton(combo.cabinet, "cab")}</div>
          </div>
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Pedals</span>
            <div class="ai-tone-tags">${pedalsList}</div>
          </div>
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Effects</span>
            <div class="ai-tone-tags">${effectsList}</div>
          </div>
        </div>
        <div class="ai-tone-combo-actions">
          <button type="button" class="btn btn-secondary tone-sharing-card-btn" data-ai-library-query="${escapeHtml(libraryQuery)}">Find in Tone Sharing</button>
        </div>
        <div class="ai-tone-library-results"></div>
      </div>`;
  }).join("");

  return `
    <div class="ai-tone-results">
      <div class="ai-tone-results-heading">${escapeHtml(heading)}</div>
      ${cards}
    </div>`;
}

function renderAiSearchView(): void {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) return;

  feed.innerHTML = `
    <div class="ai-tone-search-panel">
      <div class="ai-tone-search-form">
        <div class="ai-tone-search-inputs">
          <div class="ai-tone-input-group">
            <label for="ai-tone-band">Band / Artist</label>
            <input id="ai-tone-band" type="text" placeholder="e.g. Metallica, Jimi Hendrix" value="${escapeHtml(aiSearchState.band)}" />
          </div>
          <div class="ai-tone-input-group">
            <label for="ai-tone-song">Song (optional)</label>
            <input id="ai-tone-song" type="text" placeholder="e.g. Enter Sandman" value="${escapeHtml(aiSearchState.song)}" />
          </div>
          <button id="ai-tone-search-btn" type="button" class="btn btn-primary ai-tone-search-submit" ${aiSearchState.loading ? "disabled" : ""}>
            ${aiSearchState.loading ? "Searching\u2026" : "Find Tones"}
          </button>
        </div>
        <p class="ai-tone-search-hint">Discover the amps, cabinets and effects behind famous sounds, then find matching presets in the library.</p>
      </div>
      ${aiSearchState.error ? `<div class="tone-sharing-status ai-tone-error">${escapeHtml(aiSearchState.error)}</div>` : ""}
      ${renderAiCombinations()}
    </div>`;

  element<HTMLButtonElement>("ai-tone-search-btn")?.addEventListener("click", () => {
    aiSearchState.band = (element<HTMLInputElement>("ai-tone-band")?.value ?? "").trim();
    aiSearchState.song = (element<HTMLInputElement>("ai-tone-song")?.value ?? "").trim();
    void runAiToneSearch();
  });

  for (const id of ["ai-tone-band", "ai-tone-song"]) {
    element<HTMLInputElement>(id)?.addEventListener("keydown", (e) => {
      if (e.key === "Enter") element<HTMLButtonElement>("ai-tone-search-btn")?.click();
    });
  }

  feed.querySelectorAll<HTMLButtonElement>(".ai-tone-gear-link").forEach((btn) => {
    btn.addEventListener("click", () => {
      const query = btn.dataset.t3kQuery ?? "";
      const category = btn.dataset.t3kCategory ?? "";
      if (!query) return;
      switchMainPanel("settings");
      activateEquipmentTab("library");
      if (isFeatureEnabled(Features.Tone3000)) {
        activateLibraryTab("tone3000");
        setTone3000Search(query, category || undefined);
      } else {
        activateLibraryTab("resources");
      }
    });
  });

  feed.querySelectorAll<HTMLButtonElement>("[data-ai-library-query]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const query = btn.dataset.aiLibraryQuery ?? "";
      const resultsEl = btn.closest(".ai-tone-combo-card")?.querySelector<HTMLElement>(".ai-tone-library-results");
      if (!query || !resultsEl) return;
      await runAiLibrarySearch(query, resultsEl);
    });
  });
}

async function runAiToneSearch(): Promise<void> {
  if (!aiSearchState.band && !aiSearchState.song) {
    aiSearchState.error = "Enter a band or song name to search.";
    renderAiSearchView();
    return;
  }
  aiSearchState.loading = true;
  aiSearchState.error = "";
  aiSearchState.combinations = [];
  renderAiSearchView();
  try {
    const params = new URLSearchParams();
    if (aiSearchState.band) params.set("band", aiSearchState.band);
    if (aiSearchState.song) params.set("song", aiSearchState.song);
    const result = await apiFetch<AiToneSearchResult>(`/tone-advisor?${params.toString()}`);
    aiSearchState.combinations = result.combinations;
    aiSearchState.error = "";
  } catch (err) {
    aiSearchState.error = `AI search failed: ${(err as Error).message}`;
    aiSearchState.combinations = [];
  } finally {
    aiSearchState.loading = false;
    renderAiSearchView();
  }
}

async function runAiLibrarySearch(query: string, resultsEl: HTMLElement): Promise<void> {
  resultsEl.innerHTML = `<div class="ai-tone-library-loading">Searching library for \u201c${escapeHtml(query)}\u201d\u2026</div>`;
  try {
    const params = new URLSearchParams({ q: query });
    const data = await apiFetch<{ items: Array<{ id: string; title: string; type: string }> }>(`/search?${params.toString()}`);
    if (!data.items.length) {
      resultsEl.innerHTML = `<div class="ai-tone-library-empty">No matching presets found in library.</div>`;
      return;
    }
    const itemsHtml = data.items.slice(0, 8).map((item) => `
      <div class="ai-tone-library-item" style="border-left: 3px solid ${idAccentColor(item.id)}">
        <div class="ai-tone-library-item-info">
          <span class="ai-tone-library-item-title">${escapeHtml(item.title)}</span>
          <span class="ai-tone-library-item-type">${escapeHtml(item.type)}</span>
        </div>
        <div class="ai-tone-library-item-actions">
          <button class="btn btn-secondary tone-sharing-card-btn" type="button" data-ai-preview="${escapeHtml(item.id)}" data-ai-preview-title="${escapeHtml(item.title)}">
            <svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><polygon points="3,2 14,8 3,14"/></svg>
            Preview
          </button>
          <button class="btn btn-primary tone-sharing-card-btn" type="button" data-ai-download="${escapeHtml(item.id)}">
            <svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 1a1 1 0 011 1v6.172l1.586-1.586a1 1 0 111.414 1.414l-3.293 3.293a1 1 0 01-1.414 0L3.999 8.001a1 1 0 111.414-1.414L7.001 8.172V2A1 1 0 018 1z"/><rect x="2" y="12.5" width="12" height="2" rx="1"/></svg>
            Download
          </button>
        </div>
      </div>`).join("");
    resultsEl.innerHTML = `
      <div class="ai-tone-library-list">
        <div class="ai-tone-library-heading">Library matches for \u201c${escapeHtml(query)}\u201d</div>
        ${itemsHtml}
      </div>`;
    resultsEl.querySelectorAll<HTMLButtonElement>("[data-ai-preview]").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const id = btn.dataset.aiPreview ?? "";
        const title = btn.dataset.aiPreviewTitle ?? "";
        try { await previewPreset(id, title); }
        catch (err) { setUploadStatus(`Preview failed: ${(err as Error).message}`); }
      });
    });
    resultsEl.querySelectorAll<HTMLButtonElement>("[data-ai-download]").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const id = btn.dataset.aiDownload ?? "";
        try { await downloadAsset("item", id); }
        catch (err) { setUploadStatus(`Download failed: ${(err as Error).message}`); }
      });
    });
  } catch (err) {
    resultsEl.innerHTML = `<div class="ai-tone-library-empty">Search failed: ${escapeHtml((err as Error).message)}</div>`;
  }
}

async function renderPackDetail(details: ToneSharingPackDetails): Promise<void> {
  const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
  if (!modal) {
    return;
  }
  modal.dataset.packId = details.pack.id;

  const pack = details.pack;
  const heroEl = element<HTMLElement>("tone-sharing-pack-view-hero");
  if (heroEl) {
    let imageUrl = "";
    if (pack.thumbnailUrl) {
      try {
        imageUrl = await resolvePackThumbnailUrl(pack);
      } catch { }
    }
    heroEl.style.backgroundImage = imageUrl
      ? `linear-gradient(180deg, rgba(5,6,12,0.08) 0%, rgba(5,6,12,0.65) 55%, rgba(5,6,12,0.97) 100%), url('${imageUrl}')`
      : "";
  }

  setText("tone-sharing-pack-view-title", pack.title);
  const descEl = element<HTMLElement>("tone-sharing-pack-view-description");
  if (descEl) {
    descEl.textContent = pack.description ?? "";
    descEl.style.display = pack.description ? "" : "none";
  }

  setText("tone-sharing-pack-view-count", `${details.items.length} preset${details.items.length !== 1 ? "s" : ""}`);
  const actionsEl = element<HTMLElement>("tone-sharing-pack-view-actions");
  if (actionsEl) {
    actionsEl.innerHTML = `
      <button class="btn btn-secondary tone-sharing-card-btn" type="button" data-pack-action="share-pack">
        Share Pack
      </button>
      <button class="btn btn-primary tone-sharing-card-btn" type="button" data-pack-action="download-pack">
        <svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 1a1 1 0 011 1v6.172l1.586-1.586a1 1 0 111.414 1.414l-3.293 3.293a1 1 0 01-1.414 0L3.999 8.001a1 1 0 111.414-1.414L7.001 8.172V2A1 1 0 018 1z"/><rect x="2" y="12.5" width="12" height="2" rx="1"/></svg>
        Download Pack
      </button>
    `;
  }

  const presetsEl = element<HTMLElement>("tone-sharing-pack-view-presets");
  if (presetsEl) {
    if (!details.items.length) {
      presetsEl.innerHTML = `<div class=\"tone-sharing-status\">No presets in this pack.</div>`;
    } else {
      presetsEl.innerHTML = [...details.items]
        .sort((a, b) => a.sortOrder - b.sortOrder)
        .map(
          (item) => `
          <div class=\"tone-sharing-pack-preset-row\" data-item-id=\"${escapeHtml(item.itemId)}\" style=\"border-left: 3px solid ${idAccentColor(item.itemId)}\">
            <div class=\"tone-sharing-pack-preset-info\">
              <div class=\"tone-sharing-pack-preset-title\">${escapeHtml(item.title)}</div>
              ${item.type ? `<div class=\"tone-sharing-pack-preset-type\">${escapeHtml(item.type)}</div>` : ""}
              ${item.description ? `<div class=\"tone-sharing-pack-preset-desc\">${escapeHtml(item.description)}</div>` : ""}
              ${item.tags && item.tags.length > 0 ? `<div class=\"tone-sharing-pack-preset-tags\">${item.tags.map((t) => `<span class=\"tone-sharing-tag-badge\">${escapeHtml(t)}</span>`).join("")}</div>` : ""}
            </div>
            <div class=\"tone-sharing-pack-preset-actions\">
              <button class=\"btn btn-secondary tone-sharing-card-btn\" type=\"button\"
                data-pack-action=\"preview\" data-item-id=\"${escapeHtml(item.itemId)}\" data-item-title=\"${escapeHtml(item.title)}\">
                <svg class=\"tone-sharing-btn-icon\" viewBox=\"0 0 16 16\" fill=\"currentColor\" aria-hidden=\"true\"><polygon points=\"3,2 14,8 3,14\"/></svg>
                Preview
              </button>
              <button class=\"btn btn-primary tone-sharing-card-btn\" type=\"button\"
                data-pack-action=\"download\" data-item-id=\"${escapeHtml(item.itemId)}\" data-item-title=\"${escapeHtml(item.title)}\">
                <svg class=\"tone-sharing-btn-icon\" viewBox=\"0 0 16 16\" fill=\"currentColor\" aria-hidden=\"true\"><path d=\"M8 1a1 1 0 011 1v6.172l1.586-1.586a1 1 0 111.414 1.414l-3.293 3.293a1 1 0 01-1.414 0L3.999 8.001a1 1 0 111.414-1.414L7.001 8.172V2A1 1 0 018 1z\"/><rect x=\"2\" y=\"12.5\" width=\"12\" height=\"2\" rx=\"1\"/></svg>
                Download
              </button>
            </div>
          </div>`
        )
        .join("");
    }
  }

  modal.style.display = "flex";
}
async function viewPack(packId: string): Promise<void> {
  const details = await apiFetch<ToneSharingPackDetails>(`/packs/${packId}`);
  await renderPackDetail(details);
}

function parseShareTargetFromLocation(): ToneSharingShareTarget | null {
  let url: URL;
  try {
    url = new URL(window.location.href);
  } catch {
    return null;
  }

  const itemId = (url.searchParams.get("itemId") ?? "").trim();
  if (itemId) {
    return { kind: "item", id: itemId };
  }

  const packId = (url.searchParams.get("packId") ?? "").trim();
  if (packId) {
    return { kind: "pack", id: packId };
  }

  const match = url.pathname.match(/\/share\/(item|pack)\/([^/?#]+)/i);
  if (match) {
    const kind = match[1].toLowerCase() === "pack" ? "pack" : "item";
    return { kind, id: decodeURIComponent(match[2]) };
  }

  return null;
}

async function openSharedTargetFromLocation(): Promise<void> {
  const target = parseShareTargetFromLocation();
  if (!target) {
    return;
  }

  switchMainPanel("sharing");
  browseMode = target.kind === "pack" ? "packs" : "items";

  if (target.kind === "item") {
    const itemResult = await apiFetch<{ item: ToneSharingItem }>(`/items/${target.id}`);
    await renderFeedRows([buildSingleRow("Shared Preset", [itemResult.item], "item")]);
    setUploadStatus(`Opened shared preset: ${itemResult.item.title}`);
    return;
  }

  const packResult = await apiFetch<{ pack: ToneSharingPack }>(`/packs/${target.id}`);
  await renderFeedRows([buildSingleRow("Shared Pack", [packResult.pack], "pack")]);
  await viewPack(target.id);
  setUploadStatus(`Opened shared pack: ${packResult.pack.title}`);
}

async function readPresetFromArchive(buffer: ArrayBuffer): Promise<Record<string, unknown>> {
  const bytes = new Uint8Array(buffer);
  const isZip = bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b;

  if (isZip) {
    const zipLib = window.JSZip;
    if (!zipLib) {
      throw new Error("JSZip not loaded");
    }
    const zip = await zipLib.loadAsync(buffer);
    const entries = Object.values(zip.files).filter((entry) => !entry.dir && entry.name.toLowerCase().endsWith(".json"));
    if (!entries.length) {
      throw new Error("Preset archive does not contain a JSON preset file");
    }
    const presetText = await entries[0].async("text");
    const parsed = JSON.parse(presetText) as Record<string, unknown>;
    return (parsed.preset as Record<string, unknown>) ?? parsed;
  }

  const text = new TextDecoder().decode(buffer);
  const parsed = JSON.parse(text) as Record<string, unknown>;
  return (parsed.preset as Record<string, unknown>) ?? parsed;
}

async function parseApiErrorMessage(response: Response): Promise<string> {
  const fallback = `Request failed (${response.status})`;
  try {
    const payload = await response.json() as { error?: { message?: string } };
    return payload?.error?.message || fallback;
  } catch {
    return fallback;
  }
}

async function buildPackArchiveFromDetails(details: ToneSharingPackDetails): Promise<{ blob: Blob; fileName: string }> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const mergedPresets: Array<Record<string, unknown>> = [];
  const mergedResources = new Map<string, { entry: ItemArchiveResource; bytes: Uint8Array }>();
  const mergedBlends = new Map<string, Record<string, unknown>>();
  const mergedTone3000Resources = new Map<string, Tone3000ResourceRef>();

  const sortedItems = [...details.items].sort((a, b) => a.sortOrder - b.sortOrder);
  const packCreatorHandle = resolveCreatorProfileHandle(details.pack as unknown as Record<string, unknown>) ?? undefined;
  const packCreatorId = details.pack.creatorUserId ?? undefined;
  for (const item of sortedItems) {
    const response = await fetch(buildApiUrl(`/items/${item.itemId}/download`), {
      headers: state.sessionId ? { "x-session-id": state.sessionId } : {},
      credentials: "include"
    });

    if (!response.ok) {
      const message = await parseApiErrorMessage(response);
      throw new Error(`Failed to download pack item ${item.title}: ${message}`);
    }

    const buffer = await response.arrayBuffer();
    const bytes = new Uint8Array(buffer);
    const isZip = bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b;
    if (!isZip) {
      const preset = await readPresetFromArchive(buffer);
      preset.toneSharingOrigin = {
        source: "toneSharingApi",
        itemId: item.itemId,
        originalPresetId: typeof preset.id === "string" ? preset.id : item.itemId,
        importedAt: new Date().toISOString(),
        importedFromPackId: details.pack.id,
        creatorId: packCreatorId,
        creatorHandle: packCreatorHandle,
        republishBlocked: true,
      };
      mergedPresets.push(preset);
      continue;
    }

    const zip = await zipLib.loadAsync(buffer);
    const presetEntry = zip.file("preset.json");
    const presetsEntry = zip.file("presets.json");

    if (presetEntry) {
      const parsed = JSON.parse(await presetEntry.async("text")) as ItemArchive;
      if (parsed.preset) {
        parsed.preset.toneSharingOrigin = {
          source: "toneSharingApi",
          itemId: item.itemId,
          originalPresetId: typeof parsed.preset.id === "string" ? parsed.preset.id : item.itemId,
          importedAt: new Date().toISOString(),
          importedFromPackId: details.pack.id,
          creatorId: packCreatorId,
          creatorHandle: packCreatorHandle,
          republishBlocked: true,
        };
        mergedPresets.push(parsed.preset);
      }
      for (const resource of parsed.resources ?? []) {
        const file = zip.file(`resources/${resource.fileName}`) ?? zip.file(resource.fileName);
        if (!file) {
          continue;
        }
        const key = `${resource.type}:${resource.hash ?? resource.fileName}`;
        if (mergedResources.has(key)) {
          continue;
        }
        mergedResources.set(key, {
          entry: resource,
          bytes: new Uint8Array(await file.async("arraybuffer")),
        });
      }
      for (const blend of parsed.blends ?? []) {
        const blendId = typeof blend.id === "string" ? blend.id : "";
        if (blendId && !mergedBlends.has(blendId)) {
          mergedBlends.set(blendId, blend);
        }
      }
      for (const ref of parsed.tone3000Resources ?? []) {
        const key = `${ref.type}:${ref.id}:${ref.toneId ?? ""}:${ref.modelId ?? ""}`;
        if (!mergedTone3000Resources.has(key)) {
          mergedTone3000Resources.set(key, ref);
        }
      }
      continue;
    }

    if (presetsEntry) {
      const parsed = JSON.parse(await presetsEntry.async("text")) as ItemCollectionArchive;
      mergedPresets.push(...(parsed.presets ?? []).map((preset) => ({
        ...preset,
        toneSharingOrigin: {
          source: "toneSharingApi",
          itemId: item.itemId,
          originalPresetId: typeof preset.id === "string" ? preset.id : item.itemId,
          importedAt: new Date().toISOString(),
          importedFromPackId: details.pack.id,
          creatorId: packCreatorId,
          creatorHandle: packCreatorHandle,
          republishBlocked: true,
        },
      })));
      for (const resource of parsed.resources ?? []) {
        const file = zip.file(`resources/${resource.fileName}`) ?? zip.file(resource.fileName);
        if (!file) {
          continue;
        }
        const key = `${resource.type}:${resource.hash ?? resource.fileName}`;
        if (mergedResources.has(key)) {
          continue;
        }
        mergedResources.set(key, {
          entry: resource,
          bytes: new Uint8Array(await file.async("arraybuffer")),
        });
      }
      for (const blend of parsed.blends ?? []) {
        const blendId = typeof blend.id === "string" ? blend.id : "";
        if (blendId && !mergedBlends.has(blendId)) {
          mergedBlends.set(blendId, blend);
        }
      }
      for (const ref of parsed.tone3000Resources ?? []) {
        const key = `${ref.type}:${ref.id}:${ref.toneId ?? ""}:${ref.modelId ?? ""}`;
        if (!mergedTone3000Resources.has(key)) {
          mergedTone3000Resources.set(key, ref);
        }
      }
      continue;
    }

    mergedPresets.push(await readPresetFromArchive(buffer));
  }

  if (!mergedPresets.length) {
    throw new Error("Pack has no importable presets");
  }

  const outZip = new zipLib();
  const resourcesFolder = outZip.folder("resources");
  for (const resource of mergedResources.values()) {
    resourcesFolder?.file(resource.entry.fileName, resource.bytes);
  }

  const archive: ItemCollectionArchive = {
    formatVersion: 1,
    createdAt: new Date().toISOString(),
    presets: mergedPresets,
    resources: Array.from(mergedResources.values()).map((entry) => entry.entry),
    blends: Array.from(mergedBlends.values()),
    ...(mergedTone3000Resources.size > 0 ? { tone3000Resources: Array.from(mergedTone3000Resources.values()) } : {}),
  };
  outZip.file("presets.json", JSON.stringify(archive, null, 2));

  const safeTitle = (details.pack.title || "tone-sharing-pack").trim().replace(/[^a-z0-9\-_ ]/gi, "").replace(/\s+/g, "-");
  const fileName = `${safeTitle || "tone-sharing-pack"}.zip`;
  return {
    blob: await outZip.generateAsync({ type: "blob" }),
    fileName,
  };
}

function showPreviewIndicator(title: string): void {
  const selector = element<HTMLElement>("preset-selector");
  const indicator = element<HTMLElement>("tone-sharing-preview-indicator");
  const indicatorText = element<HTMLElement>("tone-sharing-preview-text");
  if (selector) selector.style.display = "none";
  if (indicator) indicator.style.display = "flex";
  if (indicatorText) indicatorText.textContent = `Previewing preset: ${title}`;
}

function hidePreviewIndicator(): void {
  const selector = element<HTMLElement>("preset-selector");
  const indicator = element<HTMLElement>("tone-sharing-preview-indicator");
  if (selector) selector.style.display = "";
  if (indicator) indicator.style.display = "none";
}

async function clearPreviewPreset(): Promise<void> {
  const feedEl = element<HTMLElement>("tone-sharing-feed");
  if (previewingItemId) {
    const card = feedEl?.querySelector(`.tone-sharing-card-item[data-id="${CSS.escape(previewingItemId)}"]`) as HTMLElement | null;
    if (card) {
      card.classList.remove("is-previewing");
      const btn = card.querySelector<HTMLButtonElement>("[data-action='preview']");
      if (btn) {
        const svg = btn.querySelector("svg")?.cloneNode(true) as SVGElement | null;
        btn.textContent = "";
        if (svg) btn.appendChild(svg);
        btn.append(" Preview");
      }
    }
  }
  previewingItemId = null;
  previewingItemTitle = "";
  hidePreviewIndicator();
  if (previewPriorPresetId) {
    const priorPreset = uiState.presetCache.get(previewPriorPresetId);
    if (priorPreset) {
      postMessage({ type: "loadPreset", preset: priorPreset, presetId: priorPreset.id });
    }
    previewPriorPresetId = null;
  }
}

/**
 * Walk the preset JSON (as returned by readPresetFromArchive) and replace
 * every resource-id reference that appears in idMap with its mapped value.
 * This is needed so that previewed presets resolve to the resources that
 * were just imported from the tone-sharing archive.
 */
function remapPresetResourceIds(preset: Record<string, unknown>, idMap: Map<string, string>): void {
  const graph = preset.graph as {
    nodes?: Array<{
      resources?: Array<{ resourceId?: string; id?: string }>;
      config?: { blendId?: string };
    }>;
  } | undefined;

  if (graph?.nodes) {
    for (const node of graph.nodes) {
      if (Array.isArray(node.resources)) {
        for (const res of node.resources) {
          const resourceId = res.resourceId ?? res.id;
          if (resourceId) {
            const mapped = idMap.get(resourceId);
            if (mapped) {
              res.resourceId = mapped;
              res.id = mapped;
            }
          }
        }
      }
    }
  }

  if (typeof preset.audioFxModelId === "string" && idMap.has(preset.audioFxModelId)) {
    preset.audioFxModelId = idMap.get(preset.audioFxModelId);
  }
  if (typeof preset.irId === "string" && idMap.has(preset.irId)) {
    preset.irId = idMap.get(preset.irId);
  }
}

/**
 * For a single-item archive buffer from the tone-sharing API, extract any
 * embedded resource files (NAM models, IR WAVs, etc.) and import them into
 * the local library via importRemoteResource messages.  Resources already
 * present in the library (matched by content hash or id) are skipped.
 *
 * Returns a Map<oldId, newId> suitable for remapping the preset JSON before
 * it is loaded or previewed.
 */
async function importPreviewResources(buffer: ArrayBuffer): Promise<Map<string, string>> {
  const idMap = new Map<string, string>();

  const bytes = new Uint8Array(buffer);
  const isZip = bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b;
  if (!isZip) {
    return idMap; // plain-JSON preset – no embedded resources
  }

  const zipLib = window.JSZip;
  if (!zipLib) {
    return idMap;
  }

  const zip = await zipLib.loadAsync(buffer);
  const presetEntry = zip.file("preset.json");
  if (!presetEntry) {
    return idMap;
  }

  const archive = JSON.parse(await presetEntry.async("text")) as ItemArchive;
  const resources = archive.resources ?? [];

  // Build a lookup map from the zip file entries
  const fileMap = new Map<string, { async(type: "arraybuffer"): Promise<ArrayBuffer> }>();
  Object.values(zip.files).forEach((entry) => {
    if (!(entry as { dir?: boolean }).dir) {
      const stripped = entry.name.replace(/^resources\//, "");
      const e = entry as { async(type: "arraybuffer"): Promise<ArrayBuffer> };
      fileMap.set(stripped, e);
      fileMap.set(entry.name, e);
    }
  });

  for (const resource of resources) {
    const fileName = resource.fileName ?? "";

    // Dedup by content hash
    if (resource.hash) {
      const existing = (uiState.resourceLibrary[resource.type] ?? []).find(
        (r) => r.hash?.toLowerCase() === resource.hash!.toLowerCase()
      );
      if (existing) {
        idMap.set(resource.id, existing.id);
        continue;
      }
    }

    // Dedup by id
    const existingById = (uiState.resourceLibrary[resource.type] ?? []).find(
      (r) => r.id === resource.id
    );
    if (existingById) {
      idMap.set(resource.id, resource.id);
      continue;
    }

    const entry = fileMap.get(fileName);
    if (!entry) {
      continue; // binary not present in archive – skip
    }

    const dataBuffer = await entry.async("arraybuffer");
    const data = arrayBufferToBase64(dataBuffer);
    const newId = generateResourceId(fileName);
    idMap.set(resource.id, newId);

    postMessage({
      type: "importRemoteResource",
      provider: "toneSharing",
      resourceType: resource.type,
      resourceId: newId,
      name: resource.name ?? fileName,
      description: "",
      category: resource.category ?? "",
      subfolder: "tone-sharing",
      fileName,
      hash: resource.hash ?? "",
      data,
    });
  }

  return idMap;
}

async function previewPreset(itemId: string, itemTitle: string): Promise<void> {
  // Save original preset ID so we can restore it later (only on first preview)
  if (!previewingItemId) {
    previewPriorPresetId = uiState.activePresetId ?? null;
  }

  const response = await fetch(buildApiUrl(`/items/${itemId}/download`), {
    headers: state.sessionId ? { "x-session-id": state.sessionId } : {},
    credentials: "include"
  });
  if (!response.ok) {
    throw new Error(`Preview download failed (${response.status})`);
  }

  const disposition = response.headers.get("content-disposition") || "";
  const fileMatch = disposition.match(/filename=\"?([^\"]+)\"?/i);
  const fileName = fileMatch?.[1] || `item-${itemId}.soundshed.preset`;
  const blob = await response.blob();

  let itemMeta: ToneSharingItem | null = null;
  try {
    const itemResponse = await apiFetch<{ item: ToneSharingItem }>(`/items/${itemId}`);
    itemMeta = itemResponse.item;
  } catch {
    itemMeta = null;
  }

  const importFile = new File([blob], fileName, { type: blob.type || "application/octet-stream" });
  const importedPresets = await importPresetArchive(importFile, {
    source: "toneSharingApi",
    itemId,
    creatorId: itemMeta?.creatorUserId ?? undefined,
    creatorHandle: resolveCreatorProfileHandle((itemMeta ?? {}) as unknown as Record<string, unknown>) ?? undefined,
    titleHint: fileName.replace(/\.(soundshed\.preset|soundshed\.presets|zip)$/i, ""),
  }, {
    previewOnly: true,
    suppressNotifications: true,
  });

  const preset = importedPresets[importedPresets.length - 1];
  if (!preset) {
    throw new Error("Preview import produced no preset");
  }

  postMessage({ type: "loadPreset", preset, presetId: `preview-${itemId}` });

  // Update DOM to reflect new preview state without a full re-fetch
  const feedEl = element<HTMLElement>("tone-sharing-feed");
  if (previewingItemId && previewingItemId !== itemId) {
    const prevCard = feedEl?.querySelector(`.tone-sharing-card-item[data-id="${CSS.escape(previewingItemId)}"]`) as HTMLElement | null;
    if (prevCard) {
      prevCard.classList.remove("is-previewing");
      const prevBtn = prevCard.querySelector<HTMLButtonElement>("[data-action='preview']");
      if (prevBtn) {
        const svg = prevBtn.querySelector("svg")?.cloneNode(true) as SVGElement | null;
        prevBtn.textContent = "";
        if (svg) prevBtn.appendChild(svg);
        prevBtn.append(" Preview");
      }
    }
  }

  previewingItemId = itemId;
  previewingItemTitle = itemTitle;

  const newCard = feedEl?.querySelector(`.tone-sharing-card-item[data-id="${CSS.escape(itemId)}"]`) as HTMLElement | null;
  if (newCard) {
    newCard.classList.add("is-previewing");
    const newBtn = newCard.querySelector<HTMLButtonElement>("[data-action='preview']");
    if (newBtn) {
      const svg = newBtn.querySelector("svg")?.cloneNode(true) as SVGElement | null;
      newBtn.textContent = "";
      if (svg) newBtn.appendChild(svg);
      newBtn.append(" Previewing");
    }
  }

  showPreviewIndicator(itemTitle);
}

async function loadBrowse(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }
  clearPackDetail();
  feed.innerHTML = `<div class="tone-sharing-status">Loading...</div>`;

  try {
    if (browseMode === "featured") {
      const home = await apiFetch<{ rows: ToneSharingRow[] }>("/home");
      await renderFeedRows(home.rows);
      return;
    }

    if (browseMode === "items") {
      const items = await apiFetch<{ items: ToneSharingItem[] }>("/items?page=1&pageSize=36");
      await renderFeedRows([buildSingleRow("Latest Presets", items.items, "item")]);
      return;
    }

    if (browseMode === "packs") {
      const packs = await apiFetch<{ packs: ToneSharingPack[] }>("/packs?page=1&pageSize=36");
      await renderFeedRows([buildSingleRow("Latest Packs", packs.packs, "pack")]);
      return;
    }

    if (browseMode === "installed") {
      await renderInstalledPacks();
      return;
    }

    if (browseMode === "ai-search") {
      renderAiSearchView();
      return;
    }

    if (browseMode === "review") {
      if (!isToneSharingAdmin()) {
        feed.innerHTML = `<div class="tone-sharing-status">Admin access required.</div>`;
        return;
      }
      const [items, packs] = await Promise.all([
        apiFetch<{ items: ToneSharingItem[] }>("/items/pending/list"),
        apiFetch<{ packs: ToneSharingPack[] }>("/packs/pending/list"),
      ]);
      await renderFeedRows([
        buildSingleRow("Presets Awaiting Approval", items.items, "item"),
        buildSingleRow("Packs Awaiting Approval", packs.packs, "pack"),
      ]);
      return;
    }

    await loadMine();
  } catch (error) {
    feed.innerHTML = `<div class="tone-sharing-status">Load failed: ${(error as Error).message}</div>`;
  }
}

function formatInstalledSource(source: InstalledPackSource): string {
  if (source === "toneSharingApi") {
    return "Tone Sharing";
  }
  if (source === "generatedPack") {
    return "Generated Pack";
  }
  return "Imported Zip";
}

function formatInstalledTimestamp(value: string): string {
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return "unknown";
  }
  return parsed.toLocaleString();
}

async function renderInstalledPacks(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  clearPackDetail();
  if (!state.installedPacks.length) {
    feed.innerHTML = `<div class="tone-sharing-status">No installed packs yet. Import a zip or Tone Sharing pack first.</div>`;
    return;
  }

  const cards = state.installedPacks.map((pack) => {
    const subtitle = `${formatInstalledSource(pack.source)} · ${formatInstalledTimestamp(pack.importedAt)}`;
    const details = `${pack.presetIds.length} preset${pack.presetIds.length === 1 ? "" : "s"} · ${pack.resources.length} resource${pack.resources.length === 1 ? "" : "s"}`;
    const archiveDetail = pack.archiveFileName
      ? `<div class="tone-sharing-card-item-description">Archive: ${escapeHtml(pack.archiveFileName)}</div>`
      : "";

    return `
      <div class="tone-sharing-card-item tone-sharing-pack-hero" data-kind="installed" data-id="${escapeHtml(pack.id)}">
        <div class="tone-sharing-card-item-content">
          <div class="tone-sharing-card-item-title">${escapeHtml(pack.title)}</div>
          <div class="tone-sharing-card-item-meta">${escapeHtml(subtitle)}</div>
          <div class="tone-sharing-card-item-description">${escapeHtml(details)}</div>
          ${archiveDetail}
        </div>
        <div class="tone-sharing-card-item-actions">
          <button class="btn btn-secondary tone-sharing-card-btn" type="button" data-action="delete-installed">Delete</button>
        </div>
      </div>
    `;
  });

  feed.innerHTML = `
    <div class="tone-sharing-row">
      <div class="tone-sharing-row-title">Installed Packs</div>
      <div class="tone-sharing-row-track">
        ${cards.join("")}
      </div>
    </div>
  `;
}

async function loadMine(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  if (browseMode === "mine") {
    clearPackDetail();
  }

  if (!state.user) {
    if (browseMode === "mine") {
      feed.innerHTML = `<div class="tone-sharing-status">Sign in to view your content.</div>`;
    }
    return;
  }

  try {
    const [itemsData, packsData] = await Promise.all([
      apiFetch<{ items: ToneSharingItem[] }>("/items/me/list"),
      apiFetch<{ packs: ToneSharingPack[] }>("/packs/me/list")
    ]);

    state.myItems = itemsData.items;

    if (browseMode === "mine") {
      await renderFeedRows([
        buildSingleRow("My Presets on Tone Sharing", itemsData.items, "item"),
        buildSingleRow("My Packs on Tone Sharing", packsData.packs, "pack")
      ]);
    }
  } catch (error) {
    if (browseMode === "mine") {
      feed.innerHTML = `<div class="tone-sharing-status">Load failed: ${(error as Error).message}</div>`;
    }
  }
}

function renderPackItemSelection(items: ToneSharingItem[], checked = new Set<string>()): void {
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
          <input type="checkbox" data-pack-item-id="${item.id}" ${checked.has(item.id) ? "checked" : ""} />
          <span>${item.title}</span>
        </label>
      `
    )
    .join("");
}

function getToneSharingTagsPickerValue(): string[] {
  const picker = element<HTMLElement>("tone-sharing-tags-picker");
  if (!picker) return [];
  return Array.from(picker.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip.active"))
    .map((btn) => btn.dataset.tag ?? "")
    .filter(Boolean);
}

function setToneSharingTagsPickerValue(tags: string[]): void {
  const picker = element<HTMLElement>("tone-sharing-tags-picker");
  if (!picker) return;
  const tagSet = new Set(tags);
  picker.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip").forEach((btn) => {
    btn.classList.toggle("active", tagSet.has(btn.dataset.tag ?? ""));
  });
}

async function uploadAndPublishItem(): Promise<void> {
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

  if (!state.user) {
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

  try {
    await ensurePublishConsent();
  } catch (error) {
    setUploadStatus(`Publish cancelled: ${(error as Error).message}`);
    return;
  }

  // Validate signal chain: requires input + output + at least one effect node
  const graphNodes = activePreset.graph?.nodes ?? [];
  const hasInput = graphNodes.some((n) => n.type === "input");
  const hasOutput = graphNodes.some((n) => n.type === "output");
  const effectNodes = graphNodes.filter((n) => n.type !== "input" && n.type !== "output");
  if (!hasInput || !hasOutput || effectNodes.length === 0) {
    setUploadStatus("Preset must have a signal chain with at least one effect node to publish.");
    return;
  }

  let publicPayload: Blob;
  let privatePayload: Blob;
  try {
    const archiveBlobs = await buildToneSharingPresetArchiveBlobs(clonePreset(activePreset));
    publicPayload = archiveBlobs.publicBlob;
    privatePayload = archiveBlobs.privateBlob;
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
    return;
  }

  setUploadStatus("Building & uploading archive...");
  setPublishItemBusy(true, "Uploading preset for approval...");

  try {
    const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        kind: "item_payload",
        mimeType: publicPayload.type || "application/octet-stream",
        byteSize: publicPayload.size
      })
    });

    const uploadResponse = await fetch(buildApiUrl(`/uploads/${init.uploadId}`), {
      method: "PUT",
      headers: {
        "content-type": publicPayload.type || "application/octet-stream",
        ...(state.sessionId ? { "x-session-id": state.sessionId } : {})
      },
      body: publicPayload,
      credentials: "include"
    });

    const uploadResult = await uploadResponse.json();
    if (!uploadResponse.ok || uploadResult?.ok === false) {
      throw new Error(uploadResult?.error?.message || `Upload failed (${uploadResponse.status})`);
    }

    const complete = await apiFetch<{ assetId: string }>("/uploads/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ uploadId: init.uploadId })
    });

    const backupInit = await apiFetch<{ uploadId: string }>("/uploads/init", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        kind: "item_payload",
        mimeType: privatePayload.type || "application/octet-stream",
        byteSize: privatePayload.size
      })
    });

    const backupUploadResponse = await fetch(buildApiUrl(`/uploads/${backupInit.uploadId}`), {
      method: "PUT",
      headers: {
        "content-type": privatePayload.type || "application/octet-stream",
        ...(state.sessionId ? { "x-session-id": state.sessionId } : {})
      },
      body: privatePayload,
      credentials: "include"
    });

    const backupUploadResult = await backupUploadResponse.json();
    if (!backupUploadResponse.ok || backupUploadResult?.ok === false) {
      throw new Error(backupUploadResult?.error?.message || `Backup upload failed (${backupUploadResponse.status})`);
    }

    const backupComplete = await apiFetch<{ assetId: string }>("/uploads/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ uploadId: backupInit.uploadId })
    });

    const item = await apiFetch<{ item: ToneSharingItem }>("/items", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        type: "preset",
        title,
        description,
        tags: selectedTags.length > 0 ? selectedTags : undefined,
        payloadAssetId: complete.assetId,
        privatePayloadAssetId: backupComplete.assetId
      })
    });

    await apiFetch(`/items/${item.item.id}/publish`, { method: "POST" });

    const addToNewPack = element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.contains("active") ?? false;
    const assignPackId = element<HTMLSelectElement>("tone-sharing-pack-assign-select")?.value ?? "";
    setPublishItemBusy(false, "Uploading preset for approval...");
    closeToneSharingPublishPresetModal(true);

    if (addToNewPack) {
      await openPackModal(undefined, item.item.id);
    } else if (assignPackId) {
      try {
        await addItemToExistingPack(assignPackId, item.item.id);
        setUploadStatus("Submitted for approval and added to pack.");
      } catch (error) {
        setUploadStatus(`Submitted for approval but pack update failed: ${(error as Error).message}`);
      }
    } else {
      setUploadStatus("Preset submitted for moderator approval.");
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
  } finally {
    setPublishItemBusy(false, "Uploading preset for approval...");
  }
}

async function createAndPublishPack(): Promise<void> {
  return savePack(true);
}

async function downloadAsset(kind: "item" | "pack", id: string): Promise<void> {
  const path = kind === "item" ? `/items/${id}/download` : `/packs/${id}/download`;
  const response = await fetch(buildApiUrl(path), {
    headers: state.sessionId ? { "x-session-id": state.sessionId } : {},
    credentials: "include"
  });

  if (!response.ok && !(kind === "pack" && response.status === 409)) {
    const message = await parseApiErrorMessage(response);
    throw new Error(message);
  }

  const disposition = response.headers.get("content-disposition") || "";
  const fileMatch = disposition.match(/filename=\"?([^\"]+)\"?/i);
  const fileName = fileMatch?.[1] || `${kind}-${id}${kind === "pack" ? ".zip" : ""}`;

  if (kind === "pack") {
    let packMeta: ToneSharingPack | null = null;
    try {
      const packResponse = await apiFetch<{ pack: ToneSharingPack }>(`/packs/${id}`);
      packMeta = packResponse.pack;
    } catch {
      packMeta = null;
    }

    let importBlob: Blob;
    let importFileName = fileName;
    if (response.ok) {
      importBlob = await response.blob();
    } else {
      const details = await apiFetch<ToneSharingPackDetails>(`/packs/${id}`);
      const synthesized = await buildPackArchiveFromDetails(details);
      importBlob = synthesized.blob;
      importFileName = synthesized.fileName;
    }

    const importFile = new File([importBlob], importFileName, { type: importBlob.type || "application/zip" });
    await importPackWithConfirmation(importFile, {
      source: "toneSharingApi",
      packId: id,
      creatorId: packMeta?.creatorUserId ?? undefined,
      creatorHandle: resolveCreatorProfileHandle((packMeta ?? {}) as unknown as Record<string, unknown>) ?? undefined,
      titleHint: importFileName.replace(/\.zip$/i, ""),
    });
    return;
  }

  const blob = await response.blob();

  let itemMeta: ToneSharingItem | null = null;
  try {
    const itemResponse = await apiFetch<{ item: ToneSharingItem }>(`/items/${id}`);
    itemMeta = itemResponse.item;
  } catch {
    itemMeta = null;
  }

  // Import the preset (and its bundled resources) directly into the local
  // library instead of triggering a browser file-save.  importPackWithConfirmation
  // handles both single-preset and multi-preset archive formats and shows a
  // confirmation dialog summarising what will be imported.
  const importFile = new File([blob], fileName, { type: blob.type || "application/octet-stream" });
  await importPackWithConfirmation(importFile, {
    source: "toneSharingApi",
    itemId: id,
    creatorId: itemMeta?.creatorUserId ?? undefined,
    creatorHandle: resolveCreatorProfileHandle((itemMeta ?? {}) as unknown as Record<string, unknown>) ?? undefined,
    titleHint: fileName.replace(/\.(soundshed\.preset|soundshed\.presets|zip)$/i, ""),
  });
}

async function deleteAsset(kind: "item" | "pack", id: string): Promise<void> {
  const path = kind === "item" ? `/items/${id}` : `/packs/${id}`;
  await apiFetch(path, { method: "DELETE" });
}

function removePresetIdsFromFolders(folder: PresetFolder, toRemove: Set<string>): void {
  if (Array.isArray(folder.presetIds)) {
    folder.presetIds = folder.presetIds.filter((presetId) => !toRemove.has(presetId));
  }
  if (Array.isArray(folder.children)) {
    folder.children.forEach((child) => removePresetIdsFromFolders(child, toRemove));
  }
}

async function deleteInstalledPackById(id: string): Promise<void> {
  const pack = state.installedPacks.find((entry) => entry.id === id);
  if (!pack) {
    throw new Error("Installed pack not found");
  }

  const presetIds = Array.from(new Set(pack.presetIds));
  const resourceEntries = Array.from(new Map(pack.resources.map((entry) => [`${entry.type}:${entry.id}`, entry])).values());

  for (const presetId of presetIds) {
    postMessage({ type: "deletePreset", presetId });
  }

  if (presetIds.length > 0) {
    const toRemove = new Set(presetIds);
    uiState.presets = uiState.presets.filter((preset) => !toRemove.has(preset.id));
    uiState.filteredPresets = uiState.filteredPresets.filter((preset) => !toRemove.has(preset.id));
    presetIds.forEach((presetId) => {
      uiState.presetCache.delete(presetId);
    });

    if (Array.isArray(uiState.presetFolders)) {
      uiState.presetFolders.forEach((folder) => removePresetIdsFromFolders(folder, toRemove));
      postMessage({
        type: "setPresetFolders",
        folders: uiState.presetFolders,
        activeFolderId: uiState.activePresetFolderId ?? "__all__",
      });
    }

    if (uiState.presetFavorites) {
      const nextFavorites = new Set(uiState.presetFavorites);
      presetIds.forEach((presetId) => nextFavorites.delete(presetId));
      uiState.presetFavorites = nextFavorites;
      postMessage({ type: "setPresetFavorites", favorites: Array.from(nextFavorites) });
    }

    if (uiState.presetRatings) {
      const nextRatings = { ...uiState.presetRatings };
      presetIds.forEach((presetId) => {
        delete nextRatings[presetId];
      });
      uiState.presetRatings = nextRatings;
      postMessage({ type: "setPresetRatings", ratings: nextRatings });
    }

    if (uiState.activePresetId && toRemove.has(uiState.activePresetId)) {
      const nextPreset = uiState.presets[0] ?? null;
      if (nextPreset) {
        uiState.activePresetId = nextPreset.id;
        postMessage({ type: "loadPreset", preset: clonePreset(nextPreset), presetId: nextPreset.id });
      } else {
        uiState.activePresetId = null;
      }
    }
  }

  if (resourceEntries.length > 0) {
    postMessage({
      type: "cleanupResourceLibrary",
      scope: "all",
      removeFiles: true,
      resources: resourceEntries,
    });
  }

  if (pack.archivePath) {
    postMessage({ type: "deleteImportedToneSharingPack", path: pack.archivePath });
  }

  state.installedPacks = state.installedPacks.filter((entry) => entry.id !== id);
  persistInstalledPacks();
  await renderInstalledPacks();
}

function bindBrowseActions(): void {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  feed.addEventListener("click", async (event) => {
    const target = event.target as HTMLElement;
    const button = target.closest("button[data-action]") as HTMLButtonElement | null;
    const card = target.closest(".tone-sharing-card-item") as HTMLElement | null;
    if (!button || !card) {
      return;
    }

    const kind = (card.dataset.kind ?? "item") as "item" | "pack";
    const id = card.dataset.id ?? "";
    if (!id) {
      return;
    }

    if (button.dataset.action === "delete-installed") {
      const title = card.querySelector(".tone-sharing-card-item-title")?.textContent?.trim() || id;
      const pack = state.installedPacks.find((entry) => entry.id === id);
      const presetCount = pack?.presetIds.length ?? 0;
      const resourceCount = pack?.resources.length ?? 0;
      const archiveLine = pack?.archiveFileName ? `\nArchive: ${pack.archiveFileName}` : "";
      const confirmed = await showConfirm(
        `Delete installed pack \"${title}\"?\nPresets to remove: ${presetCount}\nResources to remove: ${resourceCount}${archiveLine}\n\nThis cannot be undone.`,
        "Delete Installed Pack",
      );
      if (!confirmed) {
        return;
      }

      try {
        setUploadStatus("Deleting installed pack...");
        await deleteInstalledPackById(id);
        setUploadStatus("Installed pack deleted.");
      } catch (error) {
        setUploadStatus(`Delete failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "view" && kind === "pack") {
      try {
        await viewPack(id);
        setUploadStatus("Pack details loaded.");
      } catch (error) {
        setUploadStatus(`View failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "edit-pack" && kind === "pack") {
      void openPackModal(id);
      return;
    }

    if (button.dataset.action === "preview" && kind === "item") {
      const itemTitle = card.dataset.title ?? "";
      try {
        await previewPreset(id, itemTitle);
        setUploadStatus("Preset preview applied (not installed).");
      } catch (error) {
        setUploadStatus(`Preview failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "download") {
      try {
        await downloadAsset(kind, id);
      } catch (error) {
        setUploadStatus(`Download failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "share") {
      try {
        await copyToneSharingShareLink({ kind, id });
        setUploadStatus("Share link copied to clipboard.");
      } catch (error) {
        setUploadStatus(`Share failed: ${(error as Error).message}`);
      }
      return;
    }

    if ((button.dataset.action === "approve" || button.dataset.action === "reject") && browseMode === "review") {
      try {
        const action = button.dataset.action === "approve" ? "approve" : "reject";
        await moderateTarget(kind, id, action);
        setUploadStatus(`${kind === "item" ? "Preset" : "Pack"} ${action === "approve" ? "approved" : "rejected"}.`);
        await loadBrowse();
      } catch (error) {
        setUploadStatus(`Moderation failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "delete") {
      const title = card.querySelector(".tone-sharing-card-item-title")?.textContent?.trim() || `${kind} ${id}`;
      const confirmed = await showConfirm(`Delete ${kind} \"${title}\"? This cannot be undone.`);
      if (!confirmed) {
        return;
      }

      try {
        setUploadStatus(`Deleting ${kind === "item" ? "preset" : "pack"}...`);
        await deleteAsset(kind, id);
        setUploadStatus(`${kind === "item" ? "Preset" : "Pack"} deleted.`);
        await loadMine();
        void loadBrowse().catch(() => {
        });
      } catch (error) {
        setUploadStatus(`Delete failed: ${(error as Error).message}`);
      }
    }
  });
}

function bindBrowseModeButtons(): void {
  const modes: Array<{ id: string; mode: typeof browseMode }> = [
    { id: "tone-sharing-browse-featured", mode: "featured" },
    { id: "tone-sharing-browse-items", mode: "items" },
    { id: "tone-sharing-browse-packs", mode: "packs" },
    { id: "tone-sharing-browse-review", mode: "review" },
    { id: "tone-sharing-browse-ai-search", mode: "ai-search" },
    { id: "tone-sharing-browse-installed", mode: "installed" },
    { id: "tone-sharing-browse-mine", mode: "mine" }
  ];

  const setActive = () => {
    for (const entry of modes) {
      const button = element<HTMLButtonElement>(entry.id);
      if (button) {
        button.classList.toggle("active", entry.mode === browseMode);
      }
    }
  };

  for (const entry of modes) {
    const button = element<HTMLButtonElement>(entry.id);
    if (!button) {
      continue;
    }
    button.addEventListener("click", async () => {
      browseMode = entry.mode;
      setActive();
      clearPackDetail();
      if (browseMode === "mine") {
        await loadMine();
      } else {
        await loadBrowse();
      }
    });
  }

  setActive();
}

function restoreLocalState(): void {
  const appSettings = uiState.appSettings ?? {};
  const persistedBase = normalizeSettingString(appSettings[storageKeys.apiBase]);
  const persistedSession = normalizeSettingString(appSettings[storageKeys.sessionId]);
  const persistedInstalled = appSettings[storageKeys.installedPacks];
  const storedBase = localStorage.getItem(storageKeys.apiBase);
  const storedSession = localStorage.getItem(storageKeys.sessionId);
  const storedInstalled = localStorage.getItem(storageKeys.installedPacks);
  if (persistedBase) {
    state.apiBase = normalizeBase(persistedBase);
  } else if (storedBase) {
    state.apiBase = normalizeBase(storedBase);
    setAppSetting(storageKeys.apiBase, state.apiBase);
  }
  if (persistedSession) {
    state.sessionId = persistedSession;
  } else if (storedSession) {
    state.sessionId = storedSession;
    setAppSetting(storageKeys.sessionId, state.sessionId);
  }

  const parseInstalled = (value: unknown): InstalledPackMetadata[] => {
    if (!Array.isArray(value)) {
      return [];
    }
    return value
      .map((entry) => normalizeInstalledPackMetadata(entry))
      .filter((entry): entry is InstalledPackMetadata => entry !== null);
  };

  if (Array.isArray(persistedInstalled)) {
    state.installedPacks = parseInstalled(persistedInstalled);
  } else if (storedInstalled) {
    try {
      const parsed = JSON.parse(storedInstalled) as unknown;
      state.installedPacks = parseInstalled(parsed);
      setAppSetting(storageKeys.installedPacks, state.installedPacks);
    } catch {
      state.installedPacks = [];
    }
  }

  const apiInput = element<HTMLInputElement>("tone-sharing-api-base");
  if (apiInput) {
    apiInput.value = state.apiBase;
  }
}

export function applyToneSharingAppSettings(settings?: Record<string, unknown>): void {
  if (!settings || !element("panel-sharing")) {
    return;
  }

  let changed = false;
  const persistedBase = normalizeSettingString(settings[storageKeys.apiBase]);
  const persistedSession = normalizeSettingString(settings[storageKeys.sessionId]);
  const persistedInstalled = settings[storageKeys.installedPacks];

  if (persistedBase) {
    const normalizedBase = normalizeBase(persistedBase);
    if (normalizedBase !== state.apiBase) {
      state.apiBase = normalizedBase;
      const apiInput = element<HTMLInputElement>("tone-sharing-api-base");
      if (apiInput) {
        apiInput.value = normalizedBase;
      }
      changed = true;
    }
  }

  if (persistedSession !== state.sessionId) {
    state.sessionId = persistedSession;
    changed = true;
  }

  if (Array.isArray(persistedInstalled)) {
    const normalizedInstalled = persistedInstalled
      .map((entry) => normalizeInstalledPackMetadata(entry))
      .filter((entry): entry is InstalledPackMetadata => entry !== null);
    const currentSerialized = JSON.stringify(state.installedPacks);
    const nextSerialized = JSON.stringify(normalizedInstalled);
    if (currentSerialized !== nextSerialized) {
      state.installedPacks = normalizedInstalled;
      changed = true;
    }
  }

  if (!changed) {
    return;
  }

  clearPackThumbnailObjectUrls();
  void (async () => {
    await loadAuthSession();
    await Promise.all([loadBrowse(), loadMine()]);
    await openSharedTargetFromLocation();
  })();
}

function bindTopControls(): void {
  element<HTMLButtonElement>("tone-sharing-api-toggle")?.addEventListener("click", () => {
    const row = element<HTMLElement>("tone-sharing-endpoint-row");
    if (row) {
      row.classList.toggle("tone-sharing-endpoint--hidden");
    }
  });

  const refreshButton = element<HTMLButtonElement>("tone-sharing-refresh");
  const apiInput = element<HTMLInputElement>("tone-sharing-api-base");
  if (refreshButton && apiInput) {
    refreshButton.addEventListener("click", async () => {
      state.apiBase = normalizeBase(apiInput.value);
      persistToneSharingApiBase(state.apiBase);
      clearPackThumbnailObjectUrls();
      await loadAuthSession();
      await Promise.all([loadBrowse(), loadMine()]);
    });
  }

  // Account chip opens sign-in modal
  element<HTMLButtonElement>("tone-sharing-account-btn")?.addEventListener("click", () => {
    openSignInModal();
  });

  element<HTMLButtonElement>("tone-sharing-preview-clear")?.addEventListener("click", () => {
    void clearPreviewPreset();
  });
  element<HTMLButtonElement>("tone-sharing-signin-modal-close")?.addEventListener("click", () => {
    closeSignInModal();
  });
  element<HTMLElement>("tone-sharing-signin-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closeSignInModal();
    }
  });

  element<HTMLButtonElement>("tone-sharing-send-code")?.addEventListener("click", () => {
    void sendCode();
  });
  element<HTMLButtonElement>("tone-sharing-verify")?.addEventListener("click", () => {
    void verifyCode();
  });
  element<HTMLButtonElement>("tone-sharing-logout")?.addEventListener("click", () => {
    void signOut();
  });

  // Create Pack / Edit Pack modal
  element<HTMLButtonElement>("tone-sharing-open-pack-modal")?.addEventListener("click", () => {
    void openPackModal();
  });
  element<HTMLButtonElement>("tone-sharing-pack-modal-close")?.addEventListener("click", () => {
    closePackModal();
  });
  element<HTMLButtonElement>("tone-sharing-pack-modal-cancel")?.addEventListener("click", () => {
    closePackModal();
  });
  element<HTMLButtonElement>("tone-sharing-save-pack-draft")?.addEventListener("click", () => {
    void savePack(false);
  });
  element<HTMLButtonElement>("tone-sharing-create-pack")?.addEventListener("click", () => {
    void savePack(true);
  });
  element<HTMLElement>("tone-sharing-pack-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closePackModal();
    }
  });

  // Pack view modal (Netflix-style)
  element<HTMLButtonElement>("tone-sharing-pack-view-close")?.addEventListener("click", () => {
    clearPackDetail();
  });
  element<HTMLElement>("tone-sharing-pack-view-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      clearPackDetail();
    }
  });
  element<HTMLElement>("tone-sharing-pack-view-actions")?.addEventListener("click", async (event) => {
    const target = event.target as HTMLElement;
    const button = target.closest("[data-pack-action]") as HTMLButtonElement | null;
    if (!button) {
      return;
    }

    const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
    const packId = modal?.dataset.packId ?? "";
    if (!packId) {
      return;
    }

    if (button.dataset.packAction === "share-pack") {
      try {
        await copyToneSharingShareLink({ kind: "pack", id: packId });
        setUploadStatus("Pack share link copied to clipboard.");
      } catch (error) {
        setUploadStatus(`Share failed: ${(error as Error).message}`);
      }
      return;
    }

    try {
      await downloadAsset("pack", packId);
    } catch (error) {
      setUploadStatus(`Download failed: ${(error as Error).message}`);
    }
  });
  element<HTMLElement>("tone-sharing-pack-view-presets")?.addEventListener("click", async (event) => {
    const target = event.target as HTMLElement;
    const button = target.closest("[data-pack-action]") as HTMLButtonElement | null;
    if (!button) {
      return;
    }
    const action = button.dataset.packAction!;
    const itemId = button.dataset.itemId!;
    const itemTitle = button.dataset.itemTitle!;
    if (action === "preview") {
      try {
        await previewPreset(itemId, itemTitle);
        const presetsEl = element<HTMLElement>("tone-sharing-pack-view-presets");
        presetsEl?.querySelectorAll<HTMLElement>(".tone-sharing-pack-preset-row").forEach((row) => {
          row.classList.toggle("is-previewing", row.dataset.itemId === itemId);
        });
        presetsEl?.querySelectorAll<HTMLButtonElement>("[data-pack-action='preview']").forEach((btn) => {
          const isThis = btn.dataset.itemId === itemId;
          const svg = btn.querySelector("svg")?.cloneNode(true) as SVGElement | null;
          btn.textContent = "";
          if (svg) btn.appendChild(svg);
          btn.append(isThis ? " Previewing" : " Preview");
        });
        setUploadStatus("Preset preview applied (not installed).");
      } catch (error) {
        setUploadStatus(`Preview failed: ${(error as Error).message}`);
      }
    } else if (action === "download") {
      try {
        await downloadAsset("item", itemId);
      } catch (error) {
        setUploadStatus(`Download failed: ${(error as Error).message}`);
      }
    }
  });

  // Tag chip toggles in publish modal
  element<HTMLElement>("tone-sharing-tags-picker")?.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip").forEach((btn) => {
    btn.addEventListener("click", () => btn.classList.toggle("active"));
  });

  // Publish preset modal (opened from preset chooser)
  element<HTMLButtonElement>("tone-sharing-publish-modal-close")?.addEventListener("click", () => {
    closeToneSharingPublishPresetModal();
  });
  element<HTMLButtonElement>("tone-sharing-publish-modal-cancel")?.addEventListener("click", () => {
    closeToneSharingPublishPresetModal();
  });
  element<HTMLButtonElement>("tone-sharing-upload-item")?.addEventListener("click", () => {
    void uploadAndPublishItem();
  });
  element<HTMLElement>("tone-sharing-publish-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closeToneSharingPublishPresetModal();
    }
  });

  // Pack assign controls in publish modal
  element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.addEventListener("click", () => {
    const btn = element<HTMLButtonElement>("tone-sharing-add-to-new-pack");
    if (!btn) return;
    btn.classList.toggle("active");
    if (btn.classList.contains("active")) {
      const sel = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
      if (sel) sel.value = "";
    }
  });
  element<HTMLSelectElement>("tone-sharing-pack-assign-select")?.addEventListener("change", () => {
    const sel = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
    if (sel?.value) {
      element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.remove("active");
    }
  });
}

export function initializeToneSharingPanel(): void {
  if (!element("panel-sharing")) {
    return;
  }

  restoreLocalState();
  clearPackThumbnailObjectUrls();
  bindTopControls();
  bindBrowseModeButtons();
  bindBrowseActions();

  void (async () => {
    await loadAuthSession();
    await Promise.all([loadBrowse(), loadMine()]);
  })();
}
