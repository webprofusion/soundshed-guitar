import { uiState } from "./state.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { postMessage } from "./bridge.js";
import { ensureTone3000Session } from "./tone3000.js";
import { arrayBufferToBase64 } from "./utils.js";

const API_BASE = "https://www.tone3000.com/api/v1";
const PAGE_SIZE = 25;

const categoryListEl = document.getElementById("tone3000-category-list");
const resultsEl = document.getElementById("tone3000-results");
const searchInputEl = document.getElementById("tone3000-search-input") as HTMLInputElement | null;
const searchButtonEl = document.getElementById("tone3000-search-button");
const paginationEl = document.getElementById("tone3000-pagination");
const prevButtonEl = document.getElementById("tone3000-prev-btn") as HTMLButtonElement | null;
const nextButtonEl = document.getElementById("tone3000-next-btn") as HTMLButtonElement | null;
const pageLabelEl = document.getElementById("tone3000-page-label");
const detailsModalEl = document.getElementById("tone3000-details-modal");
const detailsCloseEl = document.getElementById("tone3000-details-close");
const detailsTitleEl = document.getElementById("tone3000-details-title");
const detailsImageEl = document.getElementById("tone3000-details-image");
const detailsMetaEl = document.getElementById("tone3000-details-meta");
const detailsDescriptionEl = document.getElementById("tone3000-details-description");
const detailsTagsEl = document.getElementById("tone3000-details-tags");
const detailsModelsStatusEl = document.getElementById("tone3000-details-models-status");
const detailsModelsEl = document.getElementById("tone3000-details-models");

interface Tone3000Tone {
  id: string;
  title: string;
  name?: string;
  description?: string;
  gear?: string;
  platform?: string;
  models_count?: number;
  user?: { username?: string };
  images?: string[];
  tags?: Array<{ name?: string }>;
  equipment_image_url?: string;
  equipment_image?: string;
  gear_image_url?: string;
  image_url?: string;
  thumbnail_url?: string;
}

interface Tone3000Model {
  id: string;
  name: string;
  model_url: string;
}

interface CategoryConfig {
  id: string;
  label: string;
  gear?: string;
  platform?: string;
}

const CATEGORIES: CategoryConfig[] = [
  { id: "pedal", label: "Pedals (FX)", gear: "pedal", platform: "nam" },
  { id: "preamp", label: "Preamps", gear: "outboard", platform: "nam" },
  { id: "amp", label: "Amps", gear: "amp", platform: "nam" },
  { id: "full-rig", label: "Full Rigs", gear: "full-rig", platform: "nam" },
  { id: "cab", label: "Cab IRs", gear: "ir", platform: "ir" },
];

let activeCategory = CATEGORIES[0];
let activeQuery = "";
let currentTones: Tone3000Tone[] = [];
let currentPage = 1;
let totalPages = 1;

function getToneImportStatus(tone: Tone3000Tone): { status: "imported" | "partial" | "none"; importedCount: number } {
  const toneId = String(tone.id);
  const seen = new Set<string>();
  const resourceTypes = ["nam", "ir"];

  resourceTypes.forEach((type) => {
    const resources = uiState.resourceLibrary[type] || [];
    resources.forEach((resource) => {
      if (resource.fileMissing) {
        return;
      }
      const metadata = resource.metadata || {};
      const resourceToneId = metadata.toneId || metadata.groupId;
      if (resourceToneId && String(resourceToneId) === toneId) {
        const modelKey = type === "ir"
          ? metadata.entryName || metadata.modelId || resource.id
          : metadata.modelId || resource.id;
        if (modelKey) {
          seen.add(String(modelKey));
        }
      }
    });
  });

  const importedCount = seen.size;
  const modelCount = tone.models_count ?? 0;
  if (modelCount > 0 && importedCount >= modelCount) {
    return { status: "imported", importedCount };
  }
  if (importedCount > 0) {
    return { status: "partial", importedCount };
  }
  return { status: "none", importedCount };
}

export function initTone3000Browser(): void {
  renderCategories();
  if (resultsEl) {
    resultsEl.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      if (!target) return;
      const detailsButton = target.closest(".tone3000-details-btn") as HTMLButtonElement | null;
      if (detailsButton) {
        const toneId = detailsButton.dataset.toneId;
        const tone = currentTones.find((item) => String(item.id) === toneId);
        if (!tone) {
          showNotification("Details unavailable", "Tone not found");
          return;
        }
        void openToneDetails(tone);
        return;
      }
      const button = target.closest(".tone3000-import-btn") as HTMLButtonElement | null;
      if (!button) return;

      const toneId = button.dataset.toneId;
      const tone = currentTones.find((item) => String(item.id) === toneId);
      if (!tone) {
        showNotification("Import failed", "Tone not found");
        return;
      }

      showNotification("Import started", tone.title ?? "Tone3000");
      void importToneModels(button, tone);
    });
  }
  searchButtonEl?.addEventListener("click", () => void runSearch());
  searchInputEl?.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      void runSearch();
    }
  });

  prevButtonEl?.addEventListener("click", () => {
    if (currentPage > 1) {
      void runSearch(currentPage - 1);
    }
  });

  nextButtonEl?.addEventListener("click", () => {
    if (currentPage < totalPages) {
      void runSearch(currentPage + 1);
    }
  });

  detailsCloseEl?.addEventListener("click", () => closeToneDetails());
  detailsModalEl?.addEventListener("click", (event) => {
    if (event.target === detailsModalEl) {
      closeToneDetails();
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && detailsModalEl?.style.display !== "none") {
      closeToneDetails();
    }
  });

  void runSearch();
}

function renderCategories(): void {
  if (!categoryListEl) return;
  categoryListEl.innerHTML = CATEGORIES.map((category) => {
    const activeClass = category.id === activeCategory.id ? "active" : "";
    return `
      <button class="tone3000-category ${activeClass}" data-category="${category.id}">
        <span>${category.label}</span>
      </button>
    `;
  }).join("");

  categoryListEl.querySelectorAll(".tone3000-category").forEach((button) => {
    button.addEventListener("click", () => {
      const id = (button as HTMLElement).dataset.category;
      const next = CATEGORIES.find((cat) => cat.id === id);
      if (!next) return;
      activeCategory = next;
      renderCategories();
      void runSearch();
    });
  });
}

async function runSearch(page = 1): Promise<void> {
  if (!resultsEl) return;

  activeQuery = searchInputEl?.value.trim() ?? "";
  currentPage = page;
  await ensureTone3000Session();

  const session = uiState.tone3000Session;
  if (!session?.accessToken) {
    resultsEl.innerHTML = `<div class="tone3000-empty">Add a Tone3000 API key to browse models.</div>`;
    return;
  }

  resultsEl.innerHTML = `<div class="tone3000-empty">Loading...</div>`;
  updatePagination(true);

  try {
    const params = new URLSearchParams({
      page: String(page),
      page_size: PAGE_SIZE.toString(),
    });
    if (activeQuery) {
      params.set("query", activeQuery);
    }
    if (activeCategory.gear) {
      params.set("gear", activeCategory.gear);
    }

    const response = await fetch(`${API_BASE}/tones/search?${params.toString()}`, {
      headers: {
        Authorization: `Bearer ${session.accessToken}`,
      },
    });

    if (!response.ok) {
      throw new Error(`Search failed: ${response.status}`);
    }

    const data = await response.json();
    const tones: Tone3000Tone[] = Array.isArray(data?.tones)
      ? data.tones
      : Array.isArray(data?.results)
        ? data.results
        : Array.isArray(data?.items)
          ? data.items
          : Array.isArray(data?.data)
            ? data.data
            : Array.isArray(data)
              ? data
              : [];

    const filtered = activeCategory.platform
      ? tones.filter((tone) => {
          const platform = (tone.platform ?? "").toLowerCase();
          const platforms = (tone as { platforms?: string[] }).platforms;
          if (!platform && !Array.isArray(platforms)) {
            return true;
          }
          if (platform === activeCategory.platform) {
            return true;
          }
          return Array.isArray(platforms)
            ? platforms.some((value) => value.toLowerCase() === activeCategory.platform)
            : false;
        })
      : tones;

    updatePagination(false, data, filtered.length);
    renderResults(filtered);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    resultsEl.innerHTML = `<div class="tone3000-empty">${message}</div>`;
    updatePagination(false);
  }
}

function updatePagination(loading: boolean, data?: Record<string, unknown>, pageSize?: number): void {
  if (!paginationEl || !pageLabelEl || !prevButtonEl || !nextButtonEl) {
    return;
  }

  if (loading) {
    paginationEl.style.opacity = "0.6";
  } else {
    paginationEl.style.opacity = "1";
  }

  const pageValue = typeof data?.page === "number" ? data.page
    : typeof data?.current_page === "number" ? data.current_page
      : currentPage;
  currentPage = pageValue;

  const total = typeof data?.total === "number"
    ? data.total
    : typeof data?.total_count === "number"
      ? data.total_count
      : typeof data?.count === "number"
        ? data.count
        : null;

  const totalPagesValue = typeof data?.total_pages === "number"
    ? data.total_pages
    : typeof data?.totalPages === "number"
      ? data.totalPages
      : typeof data?.pages === "number"
        ? data.pages
        : total
          ? Math.max(1, Math.ceil(total / PAGE_SIZE))
          : pageSize && pageSize < PAGE_SIZE
            ? currentPage
            : currentPage;

  totalPages = totalPagesValue || currentPage;

  pageLabelEl.textContent = `Page ${currentPage}${totalPages ? ` of ${totalPages}` : ""}`;
  prevButtonEl.disabled = loading || currentPage <= 1;
  nextButtonEl.disabled = loading || (totalPages ? currentPage >= totalPages : false);
}

function renderResults(tones: Tone3000Tone[]): void {
  if (!resultsEl) return;

  currentTones = tones;

  if (!tones.length) {
    resultsEl.innerHTML = `<div class="tone3000-empty">No tones found in this category.</div>`;
    return;
  }

  resultsEl.innerHTML = tones
    .map((tone) => {
      const modelCount = tone.models_count ?? 0;
      const equipmentImageUrl = getEquipmentImageUrl(tone);
      const importStatus = getToneImportStatus(tone);
      const statusLabel =
        importStatus.status === "imported"
          ? "Imported"
          : importStatus.status === "partial"
            ? "Partially Imported"
            : "";
      const statusBadge = statusLabel ? `<span class="tone3000-status">${statusLabel}</span>` : "";
      const disableImport = false;
      const buttonLabel = importStatus.status === "imported"
        ? "Re-import"
        : importStatus.status === "partial"
          ? "Re-import"
          : "Import";
      const imageMarkup = equipmentImageUrl
        ? `
          <div class="tone3000-item-image">
            <img src="${escapeHtml(equipmentImageUrl)}" alt="${escapeHtml(tone.gear ?? "Equipment")}" loading="lazy" />
          </div>
        `
        : "";
      return `
        <div class="tone3000-item" data-tone-id="${String(tone.id)}">
          ${imageMarkup}
          <div class="tone3000-item-main">
            <div class="tone3000-item-title">${escapeHtml(tone.title)}</div>
            <div class="tone3000-item-meta">
              <span>${escapeHtml(tone.gear ?? "")}</span>
              <span>${escapeHtml(tone.platform ?? "")}</span>
              <span>${modelCount} models</span>
              <span>${escapeHtml(tone.user?.username ?? "")}</span>
              ${statusBadge}
            </div>
          </div>
          <div class="tone3000-item-actions">
            <button class="tone3000-details-btn" data-tone-id="${String(tone.id)}" type="button">Details</button>
            <button class="tone3000-import-btn" data-tone-id="${String(tone.id)}" ${disableImport ? "disabled" : ""}>${buttonLabel}</button>
          </div>
        </div>
      `;
    })
    .join("");

}

function getEquipmentImageUrl(tone: Tone3000Tone): string | null {
  const candidates = [
    Array.isArray(tone.images) ? tone.images[0] : undefined,
    tone.equipment_image_url,
    tone.equipment_image,
    tone.gear_image_url,
    tone.image_url,
    tone.thumbnail_url,
  ];

  for (const candidate of candidates) {
    const value = typeof candidate === "string" ? candidate.trim() : "";
    if (!value) continue;
    if (value.startsWith("http://") || value.startsWith("https://") || value.startsWith("data:")) {
      return value;
    }
  }

  return null;
}

function closeToneDetails(): void {
  if (!detailsModalEl) return;
  detailsModalEl.style.display = "none";
}

async function openToneDetails(tone: Tone3000Tone): Promise<void> {
  if (!detailsModalEl || !detailsTitleEl || !detailsImageEl || !detailsMetaEl || !detailsDescriptionEl || !detailsTagsEl || !detailsModelsEl || !detailsModelsStatusEl) {
    return;
  }

  detailsTitleEl.textContent = tone.title ?? tone.name ?? "Tone Details";
  const imageUrl = getEquipmentImageUrl(tone);
  detailsImageEl.innerHTML = imageUrl
    ? `<img src="${escapeHtml(imageUrl)}" alt="${escapeHtml(tone.gear ?? "Equipment")}" />`
    : `<div class="tone3000-details-image-placeholder">No image</div>`;

  const metaParts = [
    tone.gear ? `Gear: ${tone.gear}` : null,
    tone.platform ? `Platform: ${tone.platform}` : null,
    typeof tone.models_count === "number" ? `Models: ${tone.models_count}` : null,
    tone.user?.username ? `By: ${tone.user.username}` : null,
  ].filter(Boolean);

  detailsMetaEl.textContent = metaParts.length ? metaParts.join(" · ") : "No metadata available.";
  detailsDescriptionEl.textContent = tone.description?.trim() || "No description provided.";
  const tags = Array.isArray(tone.tags)
    ? tone.tags
      .map((tag) => tag?.name?.trim())
      .filter((tagName): tagName is string => Boolean(tagName))
    : [];
  detailsTagsEl.innerHTML = tags.length
    ? tags.map((tag) => `<span class="tone3000-details-tag">${escapeHtml(tag)}</span>`).join("")
    : `<span class="tone3000-details-tag tone3000-details-tag-empty">No tags available.</span>`;

  detailsModelsEl.innerHTML = "";
  detailsModelsStatusEl.textContent = "Loading models...";
  detailsModalEl.style.display = "flex";

  await ensureTone3000Session();
  const session = uiState.tone3000Session;
  if (!session?.accessToken) {
    detailsModelsStatusEl.textContent = "Add a Tone3000 API key to load models.";
    return;
  }

  try {
    const models = await fetchToneModels(tone, session.accessToken);
    if (!models.length) {
      detailsModelsStatusEl.textContent = "No models found for this tone.";
      return;
    }
    detailsModelsStatusEl.textContent = "";
    detailsModelsEl.innerHTML = models
      .map((model) => `<li>${escapeHtml(model.name || model.id)}</li>`)
      .join("");
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    detailsModelsStatusEl.textContent = `Unable to load models: ${message}`;
  }
}

async function fetchToneModels(tone: Tone3000Tone, accessToken: string): Promise<Tone3000Model[]> {
  const response = await fetch(`${API_BASE}/models?tone_id=${encodeURIComponent(tone.id)}&page=1&page_size=100`, {
    headers: {
      Authorization: `Bearer ${accessToken}`,
    },
  });

  if (!response.ok) {
    throw new Error(`Model fetch failed: ${response.status}`);
  }

  const data = await response.json();
  const models: Tone3000Model[] = Array.isArray(data?.models)
    ? data.models
    : Array.isArray(data?.data)
      ? data.data
      : Array.isArray(data?.results)
        ? data.results
        : Array.isArray(data)
          ? data
          : [];

  return models;
}

async function importToneModels(button: HTMLButtonElement, tone: Tone3000Tone): Promise<void> {
  const session = uiState.tone3000Session;
  if (!session?.accessToken) {
    showNotification("Tone3000 session missing");
    return;
  }

  button.disabled = true;
  button.textContent = "Importing...";

  try {
    const response = await fetch(`${API_BASE}/models?tone_id=${encodeURIComponent(tone.id)}&page=1&page_size=25`, {
      headers: {
        Authorization: `Bearer ${session.accessToken}`,
      },
    });

    if (!response.ok) {
      throw new Error(`Model fetch failed: ${response.status}`);
    }

    const data = await response.json();
    const models: Tone3000Model[] = Array.isArray(data?.models)
      ? data.models
      : Array.isArray(data?.data)
        ? data.data
        : Array.isArray(data?.results)
          ? data.results
          : Array.isArray(data)
            ? data
            : [];

    if (!models.length) {
      throw new Error("No models found for tone");
    }

    const resourceType = (tone.platform ?? "nam").toLowerCase() === "ir" ? "ir" : "nam";
    const gearFolder = sanitizeFilename(tone.gear ?? "other");
    const toneLabel = tone.title ?? tone.name ?? "tone";
    const toneFolder = sanitizeFilename(toneLabel);
    const subfolder = `${gearFolder}/${toneFolder}`;

    for (const model of models) {
      const modelResponse = await fetch(model.model_url, {
        headers: {
          Authorization: `Bearer ${session.accessToken}`,
        },
      });

      if (!modelResponse.ok) {
        throw new Error(`Model download failed: ${modelResponse.status}`);
      }

      const buffer = await modelResponse.arrayBuffer();
      const contentType = modelResponse.headers.get("content-type") ?? "";
      const fileNameHint = sanitizeFilename(model.name ?? toneLabel ?? "model");

      if (contentType.includes("zip") || model.model_url.toLowerCase().endsWith(".zip")) {
        await importZipBuffer(buffer, {
          tone,
          modelId: model.id,
          nameHint: fileNameHint,
          subfolder,
        });
      } else {
        const data = arrayBufferToBase64(buffer);
        const extension = resourceType === "ir" ? ".wav" : ".nam";
        const fileName = `${fileNameHint}${extension}`;

        postMessage({
          type: "importRemoteResource",
          provider: "tone3000",
          resourceType,
          resourceId: `tone3000:${model.id}`,
          name: `${tone.title} - ${model.name}`,
          description: tone.description ?? "",
          category: tone.gear ?? "",
          subfolder,
          fileName,
          metadata: {
            provider: "tone3000",
            toneId: String(tone.id),
            toneTitle: tone.title ?? tone.name ??"",
            gear: tone.gear ?? "",
            platform: tone.platform ?? "",
            modelId: String(model.id),
            modelName: model.name ?? "",
          },
          data,
        });
      }
    }

    appendLog(`tone3000 import queued: ${tone.title}`);
    showNotification("Import started", tone.title);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    appendLog(`tone3000 import failed: ${message}`);
    showNotification("Import failed", message);
  } finally {
    button.disabled = false;
    button.textContent = "Import";
  }
}

async function importZipBuffer(
  buffer: ArrayBuffer,
  options: { tone: Tone3000Tone; modelId: string; nameHint: string; subfolder: string },
): Promise<void> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("JSZip not loaded");
  }

  const zip = await zipLib.loadAsync(buffer);
  const entries = Object.values(zip.files) as JSZipObject[];
  const tone = options.tone;
  let imported = 0;

  for (const entry of entries) {
    if (entry.dir) continue;
    const lowerName = entry.name.toLowerCase();
    const isNam = lowerName.endsWith(".nam") || lowerName.endsWith(".json");
    const isIr = lowerName.endsWith(".wav") || lowerName.endsWith(".ir");
    if (!isNam && !isIr) continue;

    const fileBuffer = await entry.async("arraybuffer");
    const data = arrayBufferToBase64(fileBuffer);
    const resourceType = isIr ? "ir" : "nam";
    const fileName = sanitizeFilename(entry.name.split("/").pop() ?? options.nameHint);

    postMessage({
      type: "importRemoteResource",
      provider: "tone3000",
      resourceType,
      resourceId: `tone3000:${options.modelId}:${sanitizeFilename(entry.name)}`,
      name: `${tone.title} - ${entry.name}`,
      description: tone.description ?? "",
      category: tone.gear ?? "",
      subfolder: options.subfolder,
      fileName,
      metadata: {
        provider: "tone3000",
        toneId: String(tone.id),
        toneTitle: tone.title ?? "",
        groupId: String(tone.id),
        groupName: tone.title ?? tone.name ?? "",
        gear: tone.gear ?? "",
        platform: tone.platform ?? "",
        modelId: String(options.modelId),
        modelName: options.nameHint ?? "",
        entryName: entry.name,
      },
      data,
    });

    imported += 1;
  }

  if (!imported) {
    throw new Error("Zip contained no supported model or IR files");
  }
}

function sanitizeFilename(raw: string): string {
  const trimmed = raw.trim() || "resource";
  return trimmed.replace(/[^a-z0-9-_\.]+/gi, "-");
}

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}
