import { postMessage, setAppSetting } from "./bridge.js";
import { showConfirm } from "./dialogs.js";
import { buildPresetArchiveBlob } from "./presets.js";
import { clonePreset, uiState } from "./state.js";
import type { Preset } from "./types.js";
import { escapeHtml, idAccentColor } from "./utils.js";

type ToneSharingUser = {
  id: string;
  email: string;
  role: string;
};

type ToneSharingItem = {
  id: string;
  title: string;
  type: string;
  moderationStatus?: string;
  description?: string | null;
};

type ToneSharingPack = {
  id: string;
  title: string;
  moderationStatus?: string;
  description?: string | null;
  thumbnailUrl?: string | null;
  thumbnailAssetId?: string | null;
};

type ToneSharingPackDetails = {
  pack: ToneSharingPack;
  items: Array<{ itemId: string; sortOrder: number; title: string; type: string; description?: string | null }>;
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
    description?: string | null;
    thumbnailUrl?: string | null;
    thumbnailAssetId?: string | null;
  }>;
};

const storageKeys = {
  apiBase: "toneSharing.apiBase",
  sessionId: "toneSharing.sessionId"
};

const state = {
  apiBase: "http://127.0.0.1:8787/v1", //https://api.soundshed.com/v1",
  sessionId: "",
  user: null as ToneSharingUser | null,
  myItems: [] as ToneSharingItem[]
};

const packThumbnailObjectUrls = new Map<string, string>();

let browseMode: "featured" | "items" | "packs" | "mine" = "featured";
let editingPackId: string | null = null;
let previewingItemId: string | null = null;
let previewingItemTitle = "";
let previewPriorPresetId: string | null = null;

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

export function isToneSharingSignedIn(): boolean {
  return Boolean(state.user);
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

  setUploadStatus(activePreset ? `Publishing: ${activePreset.name ?? activePreset.id}` : "");

  // Reset pack assign UI
  element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.remove("active");
  const packSelect = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  if (packSelect) packSelect.value = "";
  void loadMyDraftPacksForSelect();

  modal.style.display = "flex";
}

function closeToneSharingPublishPresetModal(): void {
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

function closePackModal(): void {
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
      `<option value="">Or add to existing draft pack\u2026</option>` +
      drafts.map((p) => `<option value="${p.id}">${p.title}</option>`).join("");
  } catch {
    // pack assignment is optional; silent fail
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

  const itemIds = Array.from(
    document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[data-pack-item-id]:checked")
  ).map((input) => input.dataset.packItemId || "").filter(Boolean);

  if (publish && !itemIds.length) {
    setText("tone-sharing-pack-status", "Select at least one preset to publish.");
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
      setText("tone-sharing-pack-status", "Pack published successfully.");
      closePackModal();
    } else {
      setText("tone-sharing-pack-status", "Draft saved.");
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-pack-status", `${publish ? "Publish" : "Save"} failed: ${(error as Error).message}`);
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
  if (!signedIn) {
    closeToneSharingPublishPresetModal();
    closePackModal();
  }
}

function normalizeBase(input: string): string {
  const trimmed = input.trim();
  if (!trimmed) {
    return "https://api.soundshed.com/v1";
  }
  return trimmed.endsWith("/") ? trimmed.slice(0, -1) : trimmed;
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
          return `
                  <div class=\"${cardClass}${isPreviewing ? " is-previewing" : ""}\" data-kind=\"${item.kind}\" data-id=\"${item.id}\" data-title=\"${item.title}\"${backgroundStyle}${accentStyleAttr}>
                    <div class=\"tone-sharing-card-item-content\">
                      <div class=\"tone-sharing-card-item-title\">${item.title}</div>
                      <div class=\"tone-sharing-card-item-meta\">${item.kind === "item" ? item.type ?? "preset" : "pack"}</div>
                      ${item.description ? `<div class=\"tone-sharing-card-item-description\">${item.description}</div>` : ""}
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
      description: kind === "pack" ? (entry as ToneSharingPack).description ?? null : (entry as ToneSharingItem).description ?? null,
      thumbnailUrl: kind === "pack"
        ? ((entry as ToneSharingPack).thumbnailUrl ?? ((entry as ToneSharingPack).thumbnailAssetId ? `/packs/${entry.id}/thumbnail` : null))
        : null
    }))
  };
}

function clearPackDetail(): void {
  const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

async function renderPackDetail(details: ToneSharingPackDetails): Promise<void> {
  const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
  if (!modal) {
    return;
  }

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

  const buffer = await response.arrayBuffer();
  const preset = await readPresetFromArchive(buffer);
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

    await loadMine();
  } catch (error) {
    feed.innerHTML = `<div class="tone-sharing-status">Load failed: ${(error as Error).message}</div>`;
  }
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
        buildSingleRow("My Presets", itemsData.items, "item"),
        buildSingleRow("My Packs", packsData.packs, "pack")
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

async function uploadAndPublishItem(): Promise<void> {
  let title = element<HTMLInputElement>("tone-sharing-item-title")?.value.trim() ?? "";
  let description = element<HTMLTextAreaElement>("tone-sharing-item-description")?.value.trim() ?? "";

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

  let uploadPayload: Blob;
  try {
    uploadPayload = await buildPresetArchiveBlob(clonePreset(activePreset));
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
    return;
  }

  setUploadStatus("Building & uploading archive...");

  try {
    const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        kind: "item_payload",
        mimeType: uploadPayload.type || "application/octet-stream",
        byteSize: uploadPayload.size
      })
    });

    const uploadResponse = await fetch(buildApiUrl(`/uploads/${init.uploadId}`), {
      method: "PUT",
      headers: {
        "content-type": uploadPayload.type || "application/octet-stream",
        ...(state.sessionId ? { "x-session-id": state.sessionId } : {})
      },
      body: uploadPayload,
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

    const item = await apiFetch<{ item: ToneSharingItem }>("/items", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        type: "preset",
        title,
        description,
        payloadAssetId: complete.assetId
      })
    });

    await apiFetch(`/items/${item.item.id}/publish`, { method: "POST" });

    const addToNewPack = element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.contains("active") ?? false;
    const assignPackId = element<HTMLSelectElement>("tone-sharing-pack-assign-select")?.value ?? "";
    closeToneSharingPublishPresetModal();

    if (addToNewPack) {
      await openPackModal(undefined, item.item.id);
    } else if (assignPackId) {
      try {
        await addItemToExistingPack(assignPackId, item.item.id);
        setUploadStatus("Published and added to pack.");
      } catch (error) {
        setUploadStatus(`Published but pack update failed: ${(error as Error).message}`);
      }
    } else {
      setUploadStatus("Published successfully.");
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
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

  if (!response.ok) {
    throw new Error(`Download failed (${response.status})`);
  }

  const disposition = response.headers.get("content-disposition") || "";
  const fileMatch = disposition.match(/filename=\"?([^\"]+)\"?/i);
  const fileName = fileMatch?.[1] || `${kind}-${id}${kind === "pack" ? ".zip" : ""}`;

  if (kind === "pack") {
    const bytes = new Uint8Array(await response.arrayBuffer());
    const chunkSize = 0x8000;
    let binary = "";
    for (let index = 0; index < bytes.length; index += chunkSize) {
      binary += String.fromCharCode(...bytes.subarray(index, Math.min(index + chunkSize, bytes.length)));
    }
    const data = btoa(binary);

    postMessage({
      type: "importToneSharingPack",
      packId: id,
      fileName,
      mimeType: response.headers.get("content-type") || "application/zip",
      data
    });
    return;
  }

  const blob = await response.blob();

  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = fileName;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(url);
}

async function deleteAsset(kind: "item" | "pack", id: string): Promise<void> {
  const path = kind === "item" ? `/items/${id}` : `/packs/${id}`;
  await apiFetch(path, { method: "DELETE" });
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
  const storedBase = localStorage.getItem(storageKeys.apiBase);
  const storedSession = localStorage.getItem(storageKeys.sessionId);
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

  if (!changed) {
    return;
  }

  clearPackThumbnailObjectUrls();
  void (async () => {
    await loadAuthSession();
    await Promise.all([loadBrowse(), loadMine()]);
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
