/**
 * The resource browser's Tone3000 tab: searching the remote catalog, auditioning
 * a model before committing to it, and importing the one that is chosen.
 *
 * It also owns the navigation state a Tone3000 selection leaves behind, so the
 * node's prev/next controls keep walking that result set after the modal has
 * closed — including fetching and importing models that were never downloaded.
 *
 * It reaches back into the modal only through `Tone3000TabHost`.
 */

import { getStopSvg } from "../iconAssets.js";
import { ensureTone3000Session, isTone3000AuthReady, isTone3000ByokEnabled, tone3000AuthenticatedFetch } from "../tone3000.js";
import { buildTone3000FavoritesUrl, buildTone3000SearchUrl, extractTone3000Tones, parseTone3000Pagination } from "../tone3000Api.js";
import type { Tone3000Architecture, Tone3000Model, Tone3000Tone } from "../tone3000ApiTypes.js";
import { backfillTone3000ResourceImages, fetchTone3000Models, getTone3000ImageUrl } from "../tone3000Shared.js";
import { escapeHtml } from "../utils.js";
import { TONE3000_IR_GEAR_OPTIONS } from "./slot.js";
import { Tone3000Navigator } from "./tone3000Navigation.js";
import type { PersistedResourceBrowserState, PreviewLoadingState, PreviewState, ResourceBrowserOptions, ResourceType } from "./types.js";

/**
 * What the Tone3000 tab needs from the modal around it.
 *
 * As with the Folder tab, the modal builds this as an object of closures, so
 * nothing in `ResourceBrowserModal` has to become public for the tab to move.
 */
export interface Tone3000TabHost {
  /** What the modal was opened for: the node, the resource type, the callback. */
  getOptions(): ResourceBrowserOptions | null;
  /** The architecture filter, which the Library tab shares. */
  getSelectedArchitecture(): Tone3000Architecture | null;
  /** The effect-role key that navigation state is filed under. */
  getContextKey(): string;
  rememberNavigationView(contextKey: string, view: "library" | "folder" | "tone3000"): void;
  getNavigationView(contextKey: string): "library" | "folder" | "tone3000" | undefined;
  /** The preview currently playing through the node, if any. */
  getPreviewState(): PreviewState | null;
  /** The model whose download is still in flight, if any. */
  getPreviewLoading(): PreviewLoadingState | null;
  startPreview(toneId: string, modelId: string, modelUrl: string): Promise<void>;
  cancelPreview(restoreOriginal?: boolean): void;
  selectAndImportModel(toneId: string, modelId: string, modelUrl: string, modelName: string): Promise<void>;
  /** The "sign in to see favourites" placeholder, shared with the Library tab. */
  renderFavoritesPrompt(): string;
  /** Persist the per-resource-type state after a filter change. */
  saveState(): void;
}

export class Tone3000Tab {
  /**
   * Where a Tone3000 selection leaves the node pointing, so its prev/next
   * controls keep walking this result set once the modal has closed.
   */
  readonly navigation = new Tone3000Navigator({
    getOptions: () => this.host.getOptions(),
    getSelectedArchitecture: () => this.host.getSelectedArchitecture(),
    getContextKey: () => this.host.getContextKey(),
    rememberNavigationView: (contextKey, view) => this.host.rememberNavigationView(contextKey, view),
    getNavigationView: (contextKey) => this.host.getNavigationView(contextKey),
    getTones: () => this.tone3000Tones,
    getModelsCache: () => this.toneModelsCache,
  });

  constructor(private readonly host: Tone3000TabHost) {}
  /**
   * Look up this tab's DOM and bind it. Called once from the modal's own
   * initialize(), so every tab is wired at the same point.
   */
  initialize(): void {
    this.tone3000ModeSearchTab = document.getElementById("resource-browser-tone3000-mode-search-tab") as HTMLButtonElement | null;
    this.tone3000ModeFavoritesTab = document.getElementById("resource-browser-tone3000-mode-favorites-tab") as HTMLButtonElement | null;
    this.tone3000SearchControls = document.getElementById("resource-browser-tone3000-search-controls");
    this.tone3000Search = document.getElementById("resource-browser-tone3000-search") as HTMLInputElement | null;
    this.tone3000SearchBtn = document.getElementById("resource-browser-tone3000-search-btn") as HTMLButtonElement | null;
    this.tone3000Category = document.getElementById("resource-browser-tone3000-category") as HTMLSelectElement | null;
    this.tone3000Sort = document.getElementById("resource-browser-tone3000-sort") as HTMLSelectElement | null;
    this.tone3000Architecture = document.getElementById("resource-browser-tone3000-architecture") as HTMLSelectElement | null;
    this.tone3000List = document.getElementById("resource-browser-tone3000-list");
    this.tone3000Pagination = document.getElementById("resource-browser-tone3000-pagination");
    this.tone3000PrevBtn = document.getElementById("resource-browser-tone3000-prev") as HTMLButtonElement | null;
    this.tone3000NextBtn = document.getElementById("resource-browser-tone3000-next") as HTMLButtonElement | null;
    this.tone3000PageLabel = document.getElementById("resource-browser-tone3000-page-label");
    this.tone3000Status = document.getElementById("resource-browser-tone3000-status");

    this.tone3000Search?.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        void this.runTone3000Search();
      }
    });
    this.tone3000SearchBtn?.addEventListener("click", () => void this.runTone3000Search());
    this.tone3000ModeSearchTab?.addEventListener("click", () => {
      if (this.tone3000FavoritesOnly) {
        this.tone3000FavoritesOnly = false;
        this.syncTone3000ModeUi();
        this.host.saveState();
        void this.runTone3000Search(1);
      }
    });
    this.tone3000ModeFavoritesTab?.addEventListener("click", () => {
      if (!this.tone3000FavoritesOnly) {
        this.tone3000FavoritesOnly = true;
        this.syncTone3000ModeUi();
        this.host.saveState();
        void this.runTone3000Search(1);
      }
    });
    this.tone3000Category?.addEventListener("change", () => void this.runTone3000Search());
    this.tone3000Sort?.addEventListener("change", () => void this.runTone3000Search());
    this.tone3000Architecture?.addEventListener("change", () => {
      this.expandedToneId = null;
      this.toneModelsCache.clear();
      this.host.saveState();
      void this.runTone3000Search();
    });

    this.tone3000PrevBtn?.addEventListener("click", () => {
      if (this.tone3000Page > 1) {
        void this.runTone3000Search(this.tone3000Page - 1);
      }
    });
    this.tone3000NextBtn?.addEventListener("click", () => {
      if (this.tone3000Page < this.tone3000TotalPages) {
        void this.runTone3000Search(this.tone3000Page + 1);
      }
    });

    this.tone3000List?.addEventListener("click", (event) => this.handleTone3000Click(event));
  }




  // Tone3000 tab elements
  private tone3000ModeSearchTab: HTMLButtonElement | null = null;

  private tone3000ModeFavoritesTab: HTMLButtonElement | null = null;

  private tone3000SearchControls: HTMLElement | null = null;

  private tone3000Search: HTMLInputElement | null = null;

  private tone3000SearchBtn: HTMLButtonElement | null = null;

  tone3000Category: HTMLSelectElement | null = null;

  private tone3000Sort: HTMLSelectElement | null = null;

  tone3000Architecture: HTMLSelectElement | null = null;

  tone3000List: HTMLElement | null = null;

  private tone3000Pagination: HTMLElement | null = null;

  private tone3000PrevBtn: HTMLButtonElement | null = null;

  private tone3000NextBtn: HTMLButtonElement | null = null;

  tone3000PageLabel: HTMLElement | null = null;

  tone3000Status: HTMLElement | null = null;

  // Tone3000 state
  private tone3000Query = "";

  tone3000Tones: Tone3000Tone[] = [];

  tone3000Page = 1;

  tone3000TotalPages = 1;

  tone3000FavoritesOnly = false;

  private expandedToneId: string | null = null;

  private expandedToneSection: "models" | "details" = "models";

  toneModelsCache: Map<string, Tone3000Model[]> = new Map();

  /** The Tone3000 half of the per-resource-type state the modal remembers. */
  captureTone3000State(persisted: PersistedResourceBrowserState, resourceType: ResourceType): void {
    persisted.tone3000Search = this.tone3000Search?.value ?? "";
    persisted.tone3000Category = this.tone3000Category?.value ?? (resourceType === "ir" ? "cab" : "amp");
    persisted.tone3000Sort = this.tone3000Sort?.value ?? "popular";
    persisted.tone3000Architecture = this.tone3000Architecture?.value ?? (resourceType === "ir" ? "all" : "2");
    persisted.tone3000FavoritesOnly = this.tone3000FavoritesOnly;
    persisted.tone3000Page = this.tone3000Page;
    persisted.tone3000TotalPages = this.tone3000TotalPages;
    persisted.tone3000Tones = [...this.tone3000Tones];
    persisted.expandedToneId = this.expandedToneId;
    persisted.toneModelsCache = Array.from(this.toneModelsCache.entries());
  }

  /** The Tone3000 controls: search box, filters, and the favourites toggle. */
  restoreTone3000Controls(persisted: PersistedResourceBrowserState, resourceType: ResourceType): void {
    if (this.tone3000Search) {
      this.tone3000Search.value = persisted.tone3000Search;
      this.tone3000Search.placeholder = resourceType === "ir"
        ? "Search IRs..."
        : "Search amps and pedals...";
    }

    if (this.tone3000Category) {
      this.tone3000Category.value = persisted.tone3000Category;
    }

    if (this.tone3000Sort) {
      this.tone3000Sort.value = persisted.tone3000Sort;
    }

    if (this.tone3000Architecture) {
      this.tone3000Architecture.value = persisted.tone3000Architecture;
    }

    this.tone3000FavoritesOnly = Boolean(persisted.tone3000FavoritesOnly);
    this.syncTone3000ModeUi();
  }

  /** The result set itself, so reopening the modal shows the same page again. */
  restoreTone3000Results(persisted: PersistedResourceBrowserState): void {
    this.tone3000Query = persisted.tone3000Search;
    this.tone3000Page = persisted.tone3000Page;
    this.tone3000TotalPages = persisted.tone3000TotalPages;
    this.tone3000Tones = [...persisted.tone3000Tones];
    this.expandedToneId = persisted.expandedToneId;
    this.toneModelsCache = new Map(persisted.toneModelsCache);
  }

  private canUseTone3000FavoritesMode(): boolean {
    return isTone3000ByokEnabled();
  }

  private syncTone3000ModeUi(): void {
    const enabled = this.canUseTone3000FavoritesMode();
    if (this.tone3000ModeFavoritesTab) {
      this.tone3000ModeFavoritesTab.classList.toggle("active", this.tone3000FavoritesOnly);
      this.tone3000ModeFavoritesTab.setAttribute("aria-pressed", this.tone3000FavoritesOnly ? "true" : "false");
      this.tone3000ModeFavoritesTab.title = enabled
        ? "Show your Tone3000 favourites"
        : "Favourites require your own Tone3000 API key in Settings";
    }

    if (this.tone3000ModeSearchTab) {
      this.tone3000ModeSearchTab.classList.toggle("active", !this.tone3000FavoritesOnly);
      this.tone3000ModeSearchTab.setAttribute("aria-pressed", !this.tone3000FavoritesOnly ? "true" : "false");
    }

    if (this.tone3000SearchControls) {
      this.tone3000SearchControls.style.display = this.tone3000FavoritesOnly ? "none" : "";
    }
  }

  async runTone3000Search(page = 1): Promise<void> {
    if (!this.tone3000List || !this.host.getOptions()) {
      return;
    }

    this.syncTone3000ModeUi();
    
    const useFavoritesMode = this.canUseTone3000FavoritesMode() && this.tone3000FavoritesOnly;
    if (this.tone3000FavoritesOnly && !this.canUseTone3000FavoritesMode()) {
      this.tone3000List.innerHTML = this.host.renderFavoritesPrompt();
      this.updateTone3000Pagination(false);
      return;
    }

    await ensureTone3000Session();
    if (!isTone3000AuthReady()) {
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">Add a Tone3000 API key in Settings to browse.</div>`;
      this.updateTone3000Pagination(false);
      return;
    }
    
    this.tone3000Query = this.tone3000Search?.value.trim() ?? "";
    this.tone3000Page = page;
    
    this.tone3000List.innerHTML = `<div class="resource-browser-empty">Loading...</div>`;
    this.updateTone3000Pagination(true);
    
    try {
      const params = new URLSearchParams({
        page: String(page),
        page_size: "20",
      });
      
      if (!useFavoritesMode) {
        if (this.tone3000Query) {
          params.set("query", this.tone3000Query);
        }
        
        // Set gear filter based on category. IRs search the select's own gear: cab IRs,
        // rooms ("space"), or every IR.
        const isIr = this.host.getOptions()?.resourceType === "ir";
        const categoryValue = this.tone3000Category?.value
          ?? (isIr ? "cab" : (this.host.getOptions()?.tone3000CategoryFilter ?? "amp"));
        if (isIr) {
          params.set("gear", TONE3000_IR_GEAR_OPTIONS.some((gear) => gear.value === categoryValue) ? categoryValue : "ir");
        } else if (categoryValue === "pedal") {
          params.set("gear", "pedal");
        } else if (categoryValue === "preamp") {
          params.set("gear", "outboard");
        } else if (categoryValue === "full-rig") {
          params.set("gear", "full-rig");
        } else {
          params.set("gear", "amp");
        }
        
        // Set sort
        const sortValue = this.tone3000Sort?.value ?? "popular";
        if (sortValue === "popular") {
          params.set("sort", "downloads-all-time");
        } else if (sortValue === "recent") {
          params.set("sort", "newest");
        } else if (sortValue === "trending") {
          params.set("sort", "trending");
        }

        const architecture = this.host.getSelectedArchitecture();
        if (architecture) {
          params.set("architecture", architecture);
        }
      }
      
      const endpoint = useFavoritesMode ? buildTone3000FavoritesUrl(params) : buildTone3000SearchUrl(params);
      const response = await tone3000AuthenticatedFetch(endpoint);
      
      if (!response.ok) {
        if (useFavoritesMode && (response.status === 401 || response.status === 403 || response.status === 404)) {
          throw new Error("Favourites are only available in BYOK direct API mode.");
        }
        throw new Error(`Search failed: ${response.status}`);
      }
      
      const data = await response.json();
      const tones = extractTone3000Tones(data);
      backfillTone3000ResourceImages(tones);

      // No client-side filtering needed - the API gear param already filters
      this.tone3000Tones = tones;
      this.updateTone3000PaginationFromData(data, tones.length);
      this.renderTone3000List();
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">Error: ${escapeHtml(message)}</div>`;
      this.updateTone3000Pagination(false);
    }
  }

  updateTone3000Pagination(loading: boolean): void {
    if (!this.tone3000Pagination || !this.tone3000PageLabel || !this.tone3000PrevBtn || !this.tone3000NextBtn) {
      return;
    }
    
    this.tone3000Pagination.style.opacity = loading ? "0.6" : "1";
    this.tone3000PageLabel.textContent = `Page ${this.tone3000Page}`;
    this.tone3000PrevBtn.disabled = loading || this.tone3000Page <= 1;
    this.tone3000NextBtn.disabled = loading;
  }

  private updateTone3000PaginationFromData(data: Record<string, unknown>, _pageSize: number): void {
    const parsed = parseTone3000Pagination(data, this.tone3000Page, 20);
    this.tone3000Page = parsed.page;
    this.tone3000TotalPages = parsed.total ? parsed.totalPages : this.tone3000Page;
    
    if (this.tone3000PageLabel) {
      this.tone3000PageLabel.textContent = `Page ${this.tone3000Page} of ${this.tone3000TotalPages}`;
    }
    if (this.tone3000PrevBtn) {
      this.tone3000PrevBtn.disabled = this.tone3000Page <= 1;
    }
    if (this.tone3000NextBtn) {
      this.tone3000NextBtn.disabled = this.tone3000Page >= this.tone3000TotalPages;
    }
    if (this.tone3000Pagination) {
      this.tone3000Pagination.style.opacity = "1";
    }
  }

  renderTone3000List(): void {
    if (!this.tone3000List) {
      return;
    }
    
    if (!this.tone3000Tones.length) {
      const emptyLabel = this.tone3000FavoritesOnly
        ? "No favourite tones found. Favourite them on Tone3000 first, then refresh."
        : "No tones found. Try a different search.";
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">${escapeHtml(emptyLabel)}</div>`;
      return;
    }
    
    this.tone3000List.innerHTML = this.tone3000Tones
      .map((tone) => {
        const isExpanded = this.expandedToneId === String(tone.id);
        const imageUrl = this.getToneImageUrl(tone);
        const modelCount = tone.models_count ?? 0;
        
        const imageMarkup = imageUrl
          ? `<img class="resource-browser-tone-image" src="${escapeHtml(imageUrl)}" alt="" loading="lazy" />`
          : `<div class="resource-browser-tone-image-placeholder"></div>`;
        
        const expandedClass = isExpanded ? "resource-browser-tone is-expanded" : "resource-browser-tone";
        const expandedContentHtml = isExpanded ? this.renderToneExpandedContent(tone) : "";
        const displayTitle = tone.title || tone.name || "Untitled Tone";
        const username = tone.user?.username ?? "";
        
        return `
          <div class="${expandedClass}" data-tone-id="${escapeHtml(String(tone.id))}">
            <div class="resource-browser-tone-header">
              ${imageMarkup}
              <div class="resource-browser-tone-info">
                <div class="resource-browser-tone-title">${escapeHtml(displayTitle)}</div>
                <div class="resource-browser-tone-meta">
                  <span>${escapeHtml(tone.gear ?? "")}</span>
                  <span>${escapeHtml(tone.platform ?? "")}</span>
                  <span>${modelCount} models</span>
                  <span>${tone.downloads_count ?? 0} downloads</span>
                  ${username ? `<span>${escapeHtml(username)}</span>` : ""}
                </div>
              </div>
              <button class="resource-browser-tone-expand" type="button" data-tone-id="${escapeHtml(String(tone.id))}">
                ${isExpanded ? "▲ Hide" : "▼ Show"}
              </button>
            </div>
            ${expandedContentHtml}
          </div>
        `;
      })
      .join("");
  }

  private renderToneExpandedContent(tone: Tone3000Tone): string {
    const modelsActive = this.expandedToneSection === "models";
    return `
      <div class="resource-browser-tone-sections" data-tone-id="${escapeHtml(String(tone.id))}">
        <div class="resource-browser-tone-section-tabs" role="tablist" aria-label="Tone sections">
          <button
            class="resource-browser-tone-section-tab ${modelsActive ? "is-active" : ""}"
            type="button"
            role="tab"
            aria-selected="${modelsActive ? "true" : "false"}"
            data-tone-id="${escapeHtml(String(tone.id))}"
            data-tone-section="models"
          >Models</button>
          <button
            class="resource-browser-tone-section-tab ${!modelsActive ? "is-active" : ""}"
            type="button"
            role="tab"
            aria-selected="${!modelsActive ? "true" : "false"}"
            data-tone-id="${escapeHtml(String(tone.id))}"
            data-tone-section="details"
          >Details</button>
        </div>
        <div class="resource-browser-tone-section-panel" role="tabpanel">
          ${modelsActive ? this.renderToneModels(tone) : this.renderToneDetails(tone)}
        </div>
      </div>
    `;
  }

  private renderToneModels(tone: Tone3000Tone): string {
    const models = this.toneModelsCache.get(String(tone.id));
    
    if (!models) {
      return `<div class="resource-browser-tone-models"><div class="resource-browser-empty">Loading models...</div></div>`;
    }
    
    if (!models.length) {
      return `<div class="resource-browser-tone-models"><div class="resource-browser-empty">No models available.</div></div>`;
    }
    
    const previewState = this.host.getPreviewState();
    const previewingModelId = previewState?.toneId === String(tone.id) ? previewState.modelId : null;
    const previewLoading = this.host.getPreviewLoading();
    const loadingModelId = previewLoading?.toneId === String(tone.id) ? previewLoading.modelId : null;
    // A preview plays through the node, so a browser opened without one can only select.
    const canPreview = Boolean(this.host.getOptions()?.nodeId);

    return `
      <div class="resource-browser-tone-models">
        ${models.map((model) => {
          const isPreviewing = String(model.id) === previewingModelId;
          const isLoadingPreview = String(model.id) === loadingModelId;
          const previewClass = isPreviewing
            ? "resource-browser-model is-previewing"
            : isLoadingPreview
              ? "resource-browser-model is-preview-loading"
              : "resource-browser-model";
          const previewLabel = isPreviewing ? `${getStopSvg()} Stop` : isLoadingPreview ? "Loading..." : `Preview`;
          
          return `
            <div class="${previewClass}" data-model-id="${escapeHtml(String(model.id))}">
              <span class="resource-browser-model-name">${escapeHtml(model.name)}</span>
              <div class="resource-browser-model-actions">
                <button class="resource-browser-model-preview" type="button" 
                        data-tone-id="${escapeHtml(String(tone.id))}" 
                        data-model-id="${escapeHtml(String(model.id))}"
                        data-model-url="${escapeHtml(model.model_url)}"
                        ${isLoadingPreview ? "disabled" : ""}
                        ${canPreview ? "" : "hidden"}>
                  ${previewLabel}
                </button>
                <button class="resource-browser-model-select" type="button"
                        data-tone-id="${escapeHtml(String(tone.id))}"
                        data-model-id="${escapeHtml(String(model.id))}"
                        data-model-url="${escapeHtml(model.model_url)}"
                        data-model-name="${escapeHtml(model.name)}">
                  Select
                </button>
              </div>
            </div>
          `;
        }).join("")}
      </div>
    `;
  }

  private renderToneDetails(tone: Tone3000Tone): string {
    const description = tone.description?.trim() || "No description provided.";
    const tags = Array.isArray(tone.tags)
      ? tone.tags.map((tag) => tag?.name?.trim()).filter((name): name is string => Boolean(name))
      : [];
    const infoRows = [
      ["Gear", tone.gear ?? "Unknown"],
      ["Platform", tone.platform ?? "Unknown"],
      ["Models", String(tone.models_count ?? 0)],
      ["Downloads", String(tone.downloads_count ?? 0)],
      ["Author", tone.user?.username ?? "Unknown"],
    ];

    return `
      <div class="resource-browser-tone-details-panel">
        <div class="resource-browser-tone-metadata">
          ${infoRows.map(([label, value]) => `
            <span class="resource-browser-tone-metadata-badge">
              <span class="resource-browser-tone-metadata-label">${escapeHtml(label)}</span>
              <span class="resource-browser-tone-metadata-value">${escapeHtml(value)}</span>
            </span>
          `).join("")}
        </div>
        <div class="resource-browser-tone-details-description">${escapeHtml(description)}</div>
        <div class="resource-browser-tone-details-tags">
          ${tags.length
            ? tags.map((tag) => `<span class="resource-browser-tone-details-tag">${escapeHtml(tag)}</span>`).join("")
            : `<span class="resource-browser-tone-details-tag is-empty">No tags</span>`}
        </div>
      </div>
    `;
  }

  private getToneImageUrl(tone: Tone3000Tone): string | null {
    return getTone3000ImageUrl(tone);
  }

  private async handleTone3000Click(event: Event): Promise<void> {
    const target = event.target as HTMLElement | null;
    if (!target) {
      return;
    }
    
    // Handle expand button
    const expandBtn = target.closest(".resource-browser-tone-expand") as HTMLButtonElement | null;
    if (expandBtn) {
      const toneId = expandBtn.dataset.toneId;
      if (toneId) {
        await this.toggleToneExpanded(toneId);
      }
      return;
    }

    const sectionTabBtn = target.closest(".resource-browser-tone-section-tab") as HTMLButtonElement | null;
    if (sectionTabBtn) {
      const toneId = sectionTabBtn.dataset.toneId;
      const section = sectionTabBtn.dataset.toneSection;
      if (!toneId || this.expandedToneId !== toneId) {
        return;
      }
      if (section === "models" || section === "details") {
        this.expandedToneSection = section;
        this.renderTone3000List();
      }
      return;
    }

    // Expand/collapse when the user clicks anywhere on the tone row header.
    const toneHeader = target.closest(".resource-browser-tone-header") as HTMLElement | null;
    if (toneHeader) {
      const toneContainer = toneHeader.closest(".resource-browser-tone") as HTMLElement | null;
      const toneId = toneContainer?.dataset.toneId;
      if (toneId) {
        await this.toggleToneExpanded(toneId);
      }
      return;
    }
    
    // Handle preview button
    const previewBtn = target.closest(".resource-browser-model-preview") as HTMLButtonElement | null;
    if (previewBtn) {
      const toneId = previewBtn.dataset.toneId ?? "";
      const modelId = previewBtn.dataset.modelId ?? "";
      const modelUrl = previewBtn.dataset.modelUrl ?? "";
      
      const previewState = this.host.getPreviewState();
      if (previewState?.toneId === toneId && previewState.modelId === modelId) {
        this.host.cancelPreview();
      } else {
        await this.host.startPreview(toneId, modelId, modelUrl);
      }
      return;
    }
    
    // Handle select button
    const selectBtn = target.closest(".resource-browser-model-select") as HTMLButtonElement | null;
    if (selectBtn) {
      const toneId = selectBtn.dataset.toneId ?? "";
      const modelId = selectBtn.dataset.modelId ?? "";
      const modelUrl = selectBtn.dataset.modelUrl ?? "";
      const modelName = selectBtn.dataset.modelName ?? "";
      
      await this.host.selectAndImportModel(toneId, modelId, modelUrl, modelName);
      return;
    }
  }

  private async toggleToneExpanded(toneId: string): Promise<void> {
    if (this.expandedToneId === toneId) {
      this.expandedToneId = null;
      this.expandedToneSection = "models";
      this.renderTone3000List();
      return;
    }
    
    this.expandedToneId = toneId;
    this.expandedToneSection = "models";
    this.renderTone3000List();
    
    // Load models if not cached
    if (!this.toneModelsCache.has(toneId)) {
      const tone = this.tone3000Tones.find((t) => String(t.id) === toneId);
      if (tone) {
        try {
          const models = await this.fetchToneModels(tone);
          this.toneModelsCache.set(toneId, models);
          this.renderTone3000List();
        } catch (error) {
          console.error("Failed to fetch models:", error);
          this.toneModelsCache.set(toneId, []);
          this.renderTone3000List();
        }
      }
    }
  }

  private async fetchToneModels(tone: Tone3000Tone): Promise<Tone3000Model[]> {
    if (!isTone3000AuthReady()) {
      throw new Error("No session");
    }

    return fetchTone3000Models(tone, this.host.getSelectedArchitecture() ?? undefined);
  }
}
