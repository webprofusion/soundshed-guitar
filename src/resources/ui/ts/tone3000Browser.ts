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

interface Tone3000Tone {
  id: string;
  title: string;
  description?: string;
  gear?: string;
  platform?: string;
  models_count?: number;
  user?: { username?: string };
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

export function initTone3000Browser(): void {
  renderCategories();
  if (resultsEl) {
    resultsEl.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      if (!target) return;
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

async function runSearch(): Promise<void> {
  if (!resultsEl) return;

  activeQuery = searchInputEl?.value.trim() ?? "";
  await ensureTone3000Session();

  const session = uiState.tone3000Session;
  if (!session?.accessToken) {
    resultsEl.innerHTML = `<div class="tone3000-empty">Add a Tone3000 API key to browse models.</div>`;
    return;
  }

  resultsEl.innerHTML = `<div class="tone3000-empty">Loading...</div>`;

  try {
    const params = new URLSearchParams({
      page: "1",
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

    renderResults(filtered);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    resultsEl.innerHTML = `<div class="tone3000-empty">${message}</div>`;
  }
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
      return `
        <div class="tone3000-item" data-tone-id="${String(tone.id)}">
          <div class="tone3000-item-main">
            <div class="tone3000-item-title">${escapeHtml(tone.title)}</div>
            <div class="tone3000-item-meta">
              <span>${escapeHtml(tone.gear ?? "")}</span>
              <span>${escapeHtml(tone.platform ?? "")}</span>
              <span>${modelCount} models</span>
              <span>${escapeHtml(tone.user?.username ?? "")}</span>
            </div>
          </div>
          <div class="tone3000-item-actions">
            <button class="tone3000-import-btn" data-tone-id="${String(tone.id)}">Import</button>
          </div>
        </div>
      `;
    })
    .join("");

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
    const subfolder = `${tone.gear ?? "other"}`.replace(/[^a-z0-9-_]+/gi, "-");

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
      const fileNameHint = sanitizeFilename(`${tone.title}-${model.name}`);

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
