import { postMessage, setAppSetting } from "./bridge.js";
import { uiState } from "./state.js";

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
  items: Array<{ itemId: string; sortOrder: number; title: string; type: string }>;
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
  if (titleInput && defaultTitle && !titleInput.value.trim()) {
    titleInput.value = defaultTitle;
  }
  if (descriptionInput && defaultDescription && !descriptionInput.value.trim()) {
    descriptionInput.value = defaultDescription;
  }

  modal.style.display = "flex";
}

function closeToneSharingPublishPresetModal(): void {
  const modal = element<HTMLElement>("tone-sharing-publish-modal");
  if (!modal) {
    return;
  }
  modal.style.display = "none";
}

function updateAuthButtonVisibility(): void {
  const signInButton = element<HTMLButtonElement>("tone-sharing-verify");
  const signOutButton = element<HTMLButtonElement>("tone-sharing-logout");
  const publishButton = element<HTMLButtonElement>("tone-sharing-open-publish-modal");
  const signedIn = !!state.user;

  if (signInButton) {
    signInButton.style.display = signedIn ? "none" : "";
  }
  if (signOutButton) {
    signOutButton.style.display = signedIn ? "" : "none";
  }
  if (publishButton) {
    publishButton.disabled = !signedIn;
    publishButton.title = signedIn ? "Publish preset" : "Sign in to publish presets";
  }
  if (!signedIn) {
    closeToneSharingPublishPresetModal();
  }
}

function normalizeBase(input: string): string {
  const trimmed = input.trim();
  if (!trimmed) {
    return "https://api.soundshed.com/v1";
  }
  return trimmed.endsWith("/") ? trimmed.slice(0, -1) : trimmed;
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
      renderPackItemSelection([]);
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
              thumbnailUrl: item.thumbnailUrl ?? null
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

          return `
                  <div class=\"${cardClass}\" data-kind=\"${item.kind}\" data-id=\"${item.id}\"${backgroundStyle}>
                    <div class=\"tone-sharing-card-item-content\">
                      <div class=\"tone-sharing-card-item-title\">${item.title}</div>
                      <div class=\"tone-sharing-card-item-meta\">${item.kind === "item" ? item.type ?? "preset" : "pack"}</div>
                      ${item.kind === "pack" && item.description ? `<div class=\"tone-sharing-card-item-description\">${item.description}</div>` : ""}
                    </div>
                    <div class=\"tone-sharing-card-item-actions\">
                      <button type=\"button\" data-action=\"${item.kind === "item" ? "preview" : "view"}\">${item.kind === "item" ? "Preview" : "View"}</button>
                      <button type=\"button\" data-action=\"download\">Download</button>
                      ${browseMode === "mine" ? `<button type=\"button\" data-action=\"delete\">Delete</button>` : ""}
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
      description: kind === "pack" ? (entry as ToneSharingPack).description ?? null : null,
      thumbnailUrl: kind === "pack"
        ? ((entry as ToneSharingPack).thumbnailUrl ?? ((entry as ToneSharingPack).thumbnailAssetId ? `/packs/${entry.id}/thumbnail` : null))
        : null
    }))
  };
}

function clearPackDetail(): void {
  const host = element<HTMLElement>("tone-sharing-pack-detail");
  if (!host) {
    return;
  }
  host.classList.remove("visible");
  host.innerHTML = "";
}

async function renderPackDetail(details: ToneSharingPackDetails): Promise<void> {
  const host = element<HTMLElement>("tone-sharing-pack-detail");
  if (!host) {
    return;
  }

  const pack = details.pack;
  let imageUrl = "";
  let thumbnailLoadFailed = false;
  if (pack.thumbnailUrl) {
    try {
      imageUrl = await resolvePackThumbnailUrl(pack);
    } catch {
      thumbnailLoadFailed = true;
      imageUrl = "";
    }
  }
  const itemsHtml = details.items
    .map((item) => `<li>${item.title}${item.type ? ` <span class=\"tone-sharing-card-item-meta\">(${item.type})</span>` : ""}</li>`)
    .join("");

  host.innerHTML = `
    <h4>${pack.title}</h4>
    ${pack.description ? `<div class="tone-sharing-status">${pack.description}</div>` : ""}
    ${imageUrl ? `<img src="${imageUrl}" alt="${pack.title} thumbnail" />` : ""}
    ${thumbnailLoadFailed ? `<div class="tone-sharing-status">Thumbnail unavailable (check API image endpoint and local HTTP/HTTPS policy).</div>` : ""}
    <div class="tone-sharing-status">Presets in pack: ${details.items.length}</div>
    <ul class="tone-sharing-pack-detail-list">${itemsHtml || "<li>No presets in this pack.</li>"}</ul>
  `;
  host.classList.add("visible");
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

async function previewPreset(itemId: string): Promise<void> {
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
    renderPackItemSelection(itemsData.items);

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

function renderPackItemSelection(items: ToneSharingItem[]): void {
  const host = element<HTMLElement>("tone-sharing-pack-items");
  if (!host) {
    return;
  }

  if (!items.length) {
    host.innerHTML = `<div class="tone-sharing-select-item">Publish an item first.</div>`;
    return;
  }

  host.innerHTML = items
    .map(
      (item) => `
        <label class="tone-sharing-select-item">
          <input type="checkbox" data-pack-item-id="${item.id}" />
          <span>${item.title}</span>
        </label>
      `
    )
    .join("");
}

async function uploadAndPublishItem(): Promise<void> {
  const title = element<HTMLInputElement>("tone-sharing-item-title")?.value.trim() ?? "";
  const type = (element<HTMLSelectElement>("tone-sharing-item-type")?.value ?? "preset") as ToneSharingItem["type"];
  const description = element<HTMLTextAreaElement>("tone-sharing-item-description")?.value.trim() ?? "";
  const file = element<HTMLInputElement>("tone-sharing-item-file")?.files?.[0] ?? null;

  if (!state.user) {
    setUploadStatus("Sign in first.");
    return;
  }
  if (!title || !file) {
    setUploadStatus("Title and file are required.");
    return;
  }

  setUploadStatus("Uploading...");

  try {
    const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        kind: "item_payload",
        mimeType: file.type || "application/octet-stream",
        byteSize: file.size
      })
    });

    const uploadResponse = await fetch(buildApiUrl(`/uploads/${init.uploadId}`), {
      method: "PUT",
      headers: {
        "content-type": file.type || "application/octet-stream",
        ...(state.sessionId ? { "x-session-id": state.sessionId } : {})
      },
      body: file,
      credentials: "include"
    });

    const uploadPayload = await uploadResponse.json();
    if (!uploadResponse.ok || uploadPayload?.ok === false) {
      throw new Error(uploadPayload?.error?.message || `Upload failed (${uploadResponse.status})`);
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
        type,
        title,
        description,
        payloadAssetId: complete.assetId
      })
    });

    await apiFetch(`/items/${item.item.id}/publish`, { method: "POST" });

    setUploadStatus("Published successfully.");
    closeToneSharingPublishPresetModal();
    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
  }
}

async function createAndPublishPack(): Promise<void> {
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

  const itemIds = Array.from(document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[data-pack-item-id]:checked")).map(
    (input) => input.dataset.packItemId || ""
  );

  if (!itemIds.length) {
    setText("tone-sharing-pack-status", "Select at least one item.");
    return;
  }

  setText("tone-sharing-pack-status", "Publishing pack...");

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

    const pack = await apiFetch<{ pack: ToneSharingPack }>("/packs", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ title, description, thumbnailAssetId })
    });

    await apiFetch(`/packs/${pack.pack.id}/items`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ itemIds })
    });

    await apiFetch(`/packs/${pack.pack.id}/publish`, { method: "POST" });
    setText("tone-sharing-pack-status", "Pack published successfully.");
    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-pack-status", `Pack publish failed: ${(error as Error).message}`);
  }
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

    if (button.dataset.action === "preview" && kind === "item") {
      try {
        await previewPreset(id);
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
      const confirmed = window.confirm(`Delete ${kind} \"${title}\"? This cannot be undone.`);
      if (!confirmed) {
        return;
      }

      try {
        await deleteAsset(kind, id);
        setUploadStatus(`${kind === "item" ? "Preset" : "Pack"} deleted.`);
        await Promise.all([loadMine(), loadBrowse()]);
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

  element<HTMLButtonElement>("tone-sharing-send-code")?.addEventListener("click", () => {
    void sendCode();
  });
  element<HTMLButtonElement>("tone-sharing-verify")?.addEventListener("click", () => {
    void verifyCode();
  });
  element<HTMLButtonElement>("tone-sharing-logout")?.addEventListener("click", () => {
    void signOut();
  });
  element<HTMLButtonElement>("tone-sharing-open-publish-modal")?.addEventListener("click", () => {
    openToneSharingPublishPresetModal();
  });
  element<HTMLButtonElement>("tone-sharing-publish-modal-close")?.addEventListener("click", () => {
    closeToneSharingPublishPresetModal();
  });
  element<HTMLButtonElement>("tone-sharing-publish-modal-cancel")?.addEventListener("click", () => {
    closeToneSharingPublishPresetModal();
  });
  element<HTMLButtonElement>("tone-sharing-upload-item")?.addEventListener("click", () => {
    void uploadAndPublishItem();
  });
  element<HTMLButtonElement>("tone-sharing-create-pack")?.addEventListener("click", () => {
    void createAndPublishPack();
  });

  element<HTMLElement>("tone-sharing-publish-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closeToneSharingPublishPresetModal();
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
