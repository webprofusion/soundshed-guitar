/**
 * Resource Browser Modal
 * 
 * Enhanced modal for selecting NAM models and IR cabs with:
 * - Resource Library tab (existing library items)
 * - Tone3000 tab (browse and preview remote items)
 * - Preview/temporary loading before import
 */

import { uiState } from "./state.js";
import { postMessage } from "./bridge.js";
import { ensureTone3000Session } from "./tone3000.js";
import { showNotification } from "./notifications.js";
import { arrayBufferToBase64 } from "./utils.js";
import type { LibraryResource } from "./types.js";

const API_BASE = "https://www.tone3000.com/api/v1";

type ResourceBrowserOptions = {
  resourceType: "nam" | "ir";
  currentId?: string;
  nodeId?: string;
  resourceIndex?: number;
  toneGroupId?: string | null;
  toneGroupTitle?: string | null;
  onSelect: (resourceId: string) => void;
  onPreview?: (filePath: string, tempResourceId: string) => void;
  onConfirmImport?: (resourceId: string) => void;
};

interface Tone3000Tone {
  id: string;
  title: string;
  name?: string;
  slug?: string;
  description?: string;
  gear?: string;
  platform?: string;
  models_count?: number;
  downloads_count?: number;
  user?: { username?: string };
  images?: string[];
  equipment_image_url?: string;
  image_url?: string;
  thumbnail_url?: string;
}

interface Tone3000Model {
  id: string | number;
  name: string;
  model_url: string;
}

interface PreviewState {
  active: boolean;
  toneId: string;
  modelId: string;
  tempFilePath: string;
  tempResourceId: string;
}

interface PreviewLoadingState {
  toneId: string;
  modelId: string;
}

export class ResourceBrowserModal {
  private initialized = false;
  private options: ResourceBrowserOptions | null = null;
  private previewState: PreviewState | null = null;
  private originalResourceId: string = ""; // Track original for revert on cancel
  private libraryPreviewActive = false;
  
  // DOM elements
  private modal: HTMLElement | null = null;
  private title: HTMLElement | null = null;
  private closeBtn: HTMLButtonElement | null = null;
  private cancelBtn: HTMLButtonElement | null = null;
  private selectBtn: HTMLButtonElement | null = null;
  
  // Tab elements
  private tabButtons: HTMLButtonElement[] = [];
  private tabPanels: HTMLElement[] = [];
  private activeTab: "library" | "tone3000" = "library";
  
  // Library tab elements
  private librarySearch: HTMLInputElement | null = null;
  private libraryCategory: HTMLSelectElement | null = null;
  private libraryList: HTMLElement | null = null;
  private selectedResourceId: string = "";
  
  // Tone3000 tab elements
  private tone3000Search: HTMLInputElement | null = null;
  private tone3000SearchBtn: HTMLButtonElement | null = null;
  private tone3000Category: HTMLSelectElement | null = null;
  private tone3000Sort: HTMLSelectElement | null = null;
  private tone3000List: HTMLElement | null = null;
  private tone3000Pagination: HTMLElement | null = null;
  private tone3000PrevBtn: HTMLButtonElement | null = null;
  private tone3000NextBtn: HTMLButtonElement | null = null;
  private tone3000PageLabel: HTMLElement | null = null;
  private tone3000Status: HTMLElement | null = null;
  
  // Tone3000 state
  private tone3000Query = "";
  private tone3000Tones: Tone3000Tone[] = [];
  private tone3000Page = 1;
  private tone3000TotalPages = 1;
  private expandedToneId: string | null = null;
  private toneModelsCache: Map<string, Tone3000Model[]> = new Map();
  private previewLoading: PreviewLoadingState | null = null;
  
  initialize(): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    
    // Get modal element
    this.modal = document.getElementById("resource-browser-modal");
    if (!this.modal) {
      console.warn("ResourceBrowserModal: modal element not found");
      return;
    }
    
    this.title = document.getElementById("resource-browser-title");
    this.closeBtn = document.getElementById("resource-browser-close") as HTMLButtonElement | null;
    this.cancelBtn = document.getElementById("resource-browser-cancel") as HTMLButtonElement | null;
    this.selectBtn = document.getElementById("resource-browser-select") as HTMLButtonElement | null;
    
    // Tab buttons and panels
    this.tabButtons = Array.from(this.modal.querySelectorAll(".resource-browser-tab-btn")) as HTMLButtonElement[];
    this.tabPanels = Array.from(this.modal.querySelectorAll(".resource-browser-tab-panel")) as HTMLElement[];
    
    // Library tab elements
    this.librarySearch = document.getElementById("resource-browser-library-search") as HTMLInputElement | null;
    this.libraryCategory = document.getElementById("resource-browser-library-category") as HTMLSelectElement | null;
    this.libraryList = document.getElementById("resource-browser-library-list");
    
    // Tone3000 tab elements
    this.tone3000Search = document.getElementById("resource-browser-tone3000-search") as HTMLInputElement | null;
    this.tone3000SearchBtn = document.getElementById("resource-browser-tone3000-search-btn") as HTMLButtonElement | null;
    this.tone3000Category = document.getElementById("resource-browser-tone3000-category") as HTMLSelectElement | null;
    this.tone3000Sort = document.getElementById("resource-browser-tone3000-sort") as HTMLSelectElement | null;
    this.tone3000List = document.getElementById("resource-browser-tone3000-list");
    this.tone3000Pagination = document.getElementById("resource-browser-tone3000-pagination");
    this.tone3000PrevBtn = document.getElementById("resource-browser-tone3000-prev") as HTMLButtonElement | null;
    this.tone3000NextBtn = document.getElementById("resource-browser-tone3000-next") as HTMLButtonElement | null;
    this.tone3000PageLabel = document.getElementById("resource-browser-tone3000-page-label");
    this.tone3000Status = document.getElementById("resource-browser-tone3000-status");
    
    // Bind events
    this.closeBtn?.addEventListener("click", () => this.close());
    this.cancelBtn?.addEventListener("click", () => this.close());
    this.selectBtn?.addEventListener("click", () => this.confirmSelection());
    
    this.modal.addEventListener("mousedown", (event) => {
      if (event.target === this.modal) {
        this.close();
      }
    });
    
    // Tab switching
    this.tabButtons.forEach((btn) => {
      btn.addEventListener("click", () => {
        const tab = btn.dataset.tab as "library" | "tone3000" | undefined;
        if (tab) {
          this.setActiveTab(tab);
        }
      });
    });
    
    // Library search
    this.librarySearch?.addEventListener("input", () => this.renderLibraryList());
    this.libraryCategory?.addEventListener("change", () => this.renderLibraryList());
    
    // Library item click
    this.libraryList?.addEventListener("click", (event) => this.handleLibraryClick(event));
    
    // Tone3000 search
    this.tone3000Search?.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        void this.runTone3000Search();
      }
    });
    this.tone3000SearchBtn?.addEventListener("click", () => void this.runTone3000Search());
    this.tone3000Category?.addEventListener("change", () => void this.runTone3000Search());
    this.tone3000Sort?.addEventListener("change", () => void this.runTone3000Search());
    
    // Pagination
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
    
    // Tone3000 list events
    this.tone3000List?.addEventListener("click", (event) => this.handleTone3000Click(event));
  }
  
  open(options: ResourceBrowserOptions): void {
    this.initialize();
    if (!this.modal) {
      return;
    }
    
    this.options = options;
    this.selectedResourceId = options.currentId ?? "";
    this.originalResourceId = options.currentId ?? ""; // Store original for cancel/revert
    this.previewState = null;
    this.previewLoading = null;
    this.libraryPreviewActive = false;
    
    // Update title
    if (this.title) {
      this.title.textContent = options.resourceType === "ir" 
        ? "Select IR Cabinet" 
        : "Select Amp Model";
    }
    
    // Update search placeholders
    if (this.librarySearch) {
      this.librarySearch.value = "";
      this.librarySearch.placeholder = options.resourceType === "ir"
        ? "Search IRs..."
        : "Search models...";
    }
    
    if (this.tone3000Search) {
      this.tone3000Search.value = "";
      this.tone3000Search.placeholder = options.resourceType === "ir"
        ? "Search Cab IRs..."
        : "Search amps and pedals...";
    }
    
    // Update category options
    this.updateCategoryOptions();
    
    // Render library list
    this.renderLibraryList();
    
    // Reset tone3000 state
    this.tone3000Tones = [];
    this.tone3000Page = 1;
    this.expandedToneId = null;
    this.toneModelsCache.clear();
    if (this.tone3000List) {
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">Enter a search query to browse Tone3000.</div>`;
    }
    this.updateTone3000Pagination(true);
    
    // Show library tab by default
    this.setActiveTab("library");
    
    // Update select button state
    this.updateSelectButtonState();
    
    this.modal.style.display = "flex";
  }
  
  close(): void {
    if (!this.modal) {
      return;
    }
    
    // Cancel any active Tone3000 preview
    if (this.previewState?.active) {
      this.cancelPreview();
    }
    
    // Revert library preview if we changed from original
    if (this.libraryPreviewActive && this.options && this.selectedResourceId !== this.originalResourceId) {
      // Revert to original resource using updateNodeResource
      postMessage({
        type: "updateNodeResource",
        nodeId: this.options.nodeId,
        resourceType: this.options.resourceType,
        resourceId: this.originalResourceId,
        filePath: "",
        resourceIndex: this.options.resourceIndex ?? 0,
      });
    }
    
    this.libraryPreviewActive = false;
    this.modal.style.display = "none";
    this.options = null;
  }
  
  private setActiveTab(tab: "library" | "tone3000"): void {
    this.activeTab = tab;
    
    this.tabButtons.forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.tab === tab);
    });
    
    this.tabPanels.forEach((panel) => {
      panel.classList.toggle("active", panel.dataset.tabPanel === tab);
    });
    
    // Run initial Tone3000 search if switching to that tab
    if (tab === "tone3000" && !this.tone3000Tones.length) {
      void this.runTone3000Search();
    }
  }
  
  private updateCategoryOptions(): void {
    const resourceType = this.options?.resourceType ?? "nam";
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    
    // Collect unique categories
    const categories = new Set<string>();
    resources.forEach((res) => {
      const cat = (res.category ?? "").trim() || "Uncategorized";
      categories.add(cat);
    });
    
    const sorted = Array.from(categories).sort();
    
    if (this.libraryCategory) {
      this.libraryCategory.innerHTML = `<option value="all">All Categories</option>` +
        sorted.map((cat) => `<option value="${escapeHtml(cat)}">${escapeHtml(cat)}</option>`).join("");
      this.libraryCategory.value = "all";
    }
    
    // Tone3000 category options based on resource type
    if (this.tone3000Category) {
      if (resourceType === "ir") {
        this.tone3000Category.innerHTML = `<option value="ir" selected>Cab IRs</option>`;
      } else {
        this.tone3000Category.innerHTML = `
          <option value="amp" selected>Amps</option>
          <option value="pedal">Pedals (FX)</option>
          <option value="preamp">Preamps</option>
          <option value="full-rig">Full Rigs</option>
        `;
      }
    }
  }
  
  private renderLibraryList(): void {
    if (!this.libraryList || !this.options) {
      return;
    }
    
    const resourceType = this.options.resourceType;
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const query = (this.librarySearch?.value ?? "").trim().toLowerCase();
    const category = this.libraryCategory?.value ?? "all";
    const currentId = this.selectedResourceId;
    
    let filtered = resources.filter((res) => !res.fileMissing);
    
    if (category !== "all") {
      filtered = filtered.filter((res) => {
        const cat = (res.category ?? "").trim() || "Uncategorized";
        return cat === category;
      });
    }
    
    if (query) {
      filtered = filtered.filter((res) => {
        const haystack = `${res.name} ${res.id} ${res.category} ${res.description}`.toLowerCase();
        return haystack.includes(query);
      });
    }
    
    filtered.sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));
    
    if (!filtered.length) {
      this.libraryList.innerHTML = `<div class="results-empty resource-browser-empty">No ${resourceType === "ir" ? "IRs" : "models"} match the current filters.</div>`;
      return;
    }
    
    this.libraryList.innerHTML = filtered
      .map((res) => {
        const title = res.name?.trim() || res.id;
        const categoryLabel = (res.category ?? "").trim() || "Uncategorized";
        const isSelected = res.id === currentId;
        const selectedClass = isSelected ? "results-item resource-browser-item is-selected" : "results-item resource-browser-item";
        const metadata = res.metadata ?? {};
        const provider = metadata.provider ?? "";
        const providerBadge = provider ? `<span class="resource-browser-provider">${escapeHtml(provider)}</span>` : "";
        const authorUsername = metadata.authorUsername ?? "";
        const sourceUrl = metadata.sourceUrl ?? "";
        const authorBadge = authorUsername ? `<span class="resource-browser-author">by: ${escapeHtml(authorUsername)}</span>` : "";
        const sourceLinkBadge = sourceUrl.startsWith("https://www.tone3000.com/") ? `<a class="resource-browser-attribution-link" href="${escapeHtml(sourceUrl)}" target="_blank" rel="noopener noreferrer">↗ tone3000</a>` : "";
        
        return `
          <div class="${selectedClass}" data-resource-id="${escapeHtml(res.id)}" data-source="library">
            <div class="results-item-main resource-browser-item-info">
              <div class="results-item-title resource-browser-item-title">${escapeHtml(title)}</div>
              <div class="results-item-meta resource-browser-item-meta">
                <span>${escapeHtml(categoryLabel)}</span>
                ${providerBadge}${authorBadge}${sourceLinkBadge}
              </div>
            </div>
            <button class="resource-browser-item-select" type="button">${isSelected ? "✓ Selected" : "Select"}</button>
          </div>
        `;
      })
      .join("");
  }
  
  private handleLibraryClick(event: Event): void {
    const target = event.target as HTMLElement | null;
    if (!target) {
      return;
    }
    
    const item = target.closest(".resource-browser-item") as HTMLElement | null;
    if (!item) {
      return;
    }
    
    const resourceId = item.dataset.resourceId ?? "";
    if (!resourceId) {
      return;
    }
    
    // Cancel any active Tone3000 preview when selecting from library
    if (this.previewState?.active) {
      this.cancelPreview();
    }
    
    this.selectedResourceId = resourceId;
    this.renderLibraryList();
    this.updateSelectButtonState();
    
    // Immediately preview the library resource
    this.previewLibraryResource(resourceId);
  }
  
  private previewLibraryResource(resourceId: string): void {
    if (!this.options) {
      return;
    }
    
    this.libraryPreviewActive = true;
    
    // Send message to plugin to apply this resource to the node
    // Use updateNodeResource which is the proper message for changing node resources
    // Include filePath as empty string to match what sendNodeResourceUpdate does
    postMessage({
      type: "updateNodeResource",
      nodeId: this.options.nodeId,
      resourceType: this.options.resourceType,
      resourceId,
      filePath: "",
      resourceIndex: this.options.resourceIndex ?? 0,
    });
    
    // Get resource name for the preview notification
    const resources = uiState.resourceLibrary[this.options.resourceType] ?? [];
    const resource = resources.find(r => r.id === resourceId);
    const displayName = resource?.name || resourceId;
    showNotification("Previewing", `${displayName} - click Select to confirm`);
  }
  
  private async runTone3000Search(page = 1): Promise<void> {
    if (!this.tone3000List || !this.options) {
      return;
    }
    
    await ensureTone3000Session();
    const session = uiState.tone3000Session;
    if (!session?.accessToken) {
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
      
      if (this.tone3000Query) {
        params.set("query", this.tone3000Query);
      }
      
      // Set gear filter based on category
      const categoryValue = this.tone3000Category?.value ?? "amp";
      if (categoryValue === "ir") {
        params.set("gear", "ir");
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
      
      const response = await fetch(`${API_BASE}/tones/search?${params.toString()}`, {
        headers: {
          Authorization: `Bearer ${session.accessToken}`,
        },
      });
      
      if (!response.ok) {
        throw new Error(`Search failed: ${response.status}`);
      }
      
      const data = await response.json();
      const tones: Tone3000Tone[] = this.extractTones(data);
      
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
  
  private extractTones(data: unknown): Tone3000Tone[] {
    if (Array.isArray(data)) return data;
    const obj = data as Record<string, unknown>;
    if (Array.isArray(obj.tones)) return obj.tones;
    if (Array.isArray(obj.results)) return obj.results;
    if (Array.isArray(obj.items)) return obj.items;
    if (Array.isArray(obj.data)) return obj.data;
    return [];
  }
  
  private updateTone3000Pagination(loading: boolean): void {
    if (!this.tone3000Pagination || !this.tone3000PageLabel || !this.tone3000PrevBtn || !this.tone3000NextBtn) {
      return;
    }
    
    this.tone3000Pagination.style.opacity = loading ? "0.6" : "1";
    this.tone3000PageLabel.textContent = `Page ${this.tone3000Page}`;
    this.tone3000PrevBtn.disabled = loading || this.tone3000Page <= 1;
    this.tone3000NextBtn.disabled = loading;
  }
  
  private updateTone3000PaginationFromData(data: Record<string, unknown>, pageSize: number): void {
    const total = typeof data.total === "number" ? data.total
      : typeof data.total_count === "number" ? data.total_count
        : typeof data.count === "number" ? data.count
          : null;
    
    const totalPages = typeof data.total_pages === "number" ? data.total_pages
      : typeof data.pages === "number" ? data.pages
        : total ? Math.max(1, Math.ceil(total / 20))
          : pageSize < 20 ? this.tone3000Page : this.tone3000Page;
    
    this.tone3000TotalPages = totalPages;
    
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
  
  private renderTone3000List(): void {
    if (!this.tone3000List) {
      return;
    }
    
    if (!this.tone3000Tones.length) {
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">No tones found. Try a different search.</div>`;
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
        const modelsHtml = isExpanded ? this.renderToneModels(tone) : "";
        const displayTitle = tone.title || tone.name || "Untitled Tone";
        const username = tone.user?.username ?? "";
        
        return `
          <div class="${expandedClass}" data-tone-id="${String(tone.id)}">
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
              <button class="resource-browser-tone-expand" type="button" data-tone-id="${String(tone.id)}">
                ${isExpanded ? "▲ Hide" : "▼ Show Models"}
              </button>
            </div>
            ${modelsHtml}
          </div>
        `;
      })
      .join("");
  }
  
  private renderToneModels(tone: Tone3000Tone): string {
    const models = this.toneModelsCache.get(String(tone.id));
    
    if (!models) {
      return `<div class="resource-browser-tone-models"><div class="resource-browser-empty">Loading models...</div></div>`;
    }
    
    if (!models.length) {
      return `<div class="resource-browser-tone-models"><div class="resource-browser-empty">No models available.</div></div>`;
    }
    
    const previewingModelId = this.previewState?.toneId === String(tone.id) ? this.previewState.modelId : null;
    const loadingModelId = this.previewLoading?.toneId === String(tone.id) ? this.previewLoading.modelId : null;
    
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
          const previewLabel = isPreviewing ? "⏹ Stop" : isLoadingPreview ? "Loading..." : "▶ Preview";
          
          return `
            <div class="${previewClass}" data-model-id="${String(model.id)}">
              <span class="resource-browser-model-name">${escapeHtml(model.name)}</span>
              <div class="resource-browser-model-actions">
                <button class="resource-browser-model-preview" type="button" 
                        data-tone-id="${String(tone.id)}" 
                        data-model-id="${String(model.id)}"
                        data-model-url="${escapeHtml(model.model_url)}"
                        ${isLoadingPreview ? "disabled" : ""}>
                  ${previewLabel}
                </button>
                <button class="resource-browser-model-select" type="button"
                        data-tone-id="${String(tone.id)}"
                        data-model-id="${String(model.id)}"
                        data-model-url="${escapeHtml(model.model_url)}"
                        data-model-name="${escapeHtml(model.name)}">
                  Select &amp; Import
                </button>
              </div>
            </div>
          `;
        }).join("")}
      </div>
    `;
  }
  
  private getToneImageUrl(tone: Tone3000Tone): string | null {
    const candidates = [
      Array.isArray(tone.images) ? tone.images[0] : undefined,
      tone.equipment_image_url,
      tone.image_url,
      tone.thumbnail_url,
    ];
    
    for (const candidate of candidates) {
      if (typeof candidate === "string" && candidate.trim()) {
        const value = candidate.trim();
        if (value.startsWith("http://") || value.startsWith("https://") || value.startsWith("data:")) {
          return value;
        }
      }
    }
    
    return null;
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
    
    // Handle preview button
    const previewBtn = target.closest(".resource-browser-model-preview") as HTMLButtonElement | null;
    if (previewBtn) {
      const toneId = previewBtn.dataset.toneId ?? "";
      const modelId = previewBtn.dataset.modelId ?? "";
      const modelUrl = previewBtn.dataset.modelUrl ?? "";
      
      if (this.previewState?.toneId === toneId && this.previewState.modelId === modelId) {
        this.cancelPreview();
      } else {
        await this.startPreview(toneId, modelId, modelUrl);
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
      
      await this.selectAndImportModel(toneId, modelId, modelUrl, modelName);
      return;
    }
  }
  
  private async toggleToneExpanded(toneId: string): Promise<void> {
    if (this.expandedToneId === toneId) {
      this.expandedToneId = null;
      this.renderTone3000List();
      return;
    }
    
    this.expandedToneId = toneId;
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
    const session = uiState.tone3000Session;
    if (!session?.accessToken) {
      throw new Error("No session");
    }
    
    const response = await fetch(`${API_BASE}/models?tone_id=${encodeURIComponent(tone.id)}&page=1&page_size=100`, {
      headers: {
        Authorization: `Bearer ${session.accessToken}`,
      },
    });
    
    if (!response.ok) {
      throw new Error(`Failed: ${response.status}`);
    }
    
    const data = await response.json();
    return this.extractModels(data);
  }
  
  private extractModels(data: unknown): Tone3000Model[] {
    if (Array.isArray(data)) return data;
    const obj = data as Record<string, unknown>;
    if (Array.isArray(obj.models)) return obj.models;
    if (Array.isArray(obj.data)) return obj.data;
    if (Array.isArray(obj.results)) return obj.results;
    return [];
  }
  
  private async startPreview(toneId: string, modelId: string, modelUrl: string): Promise<void> {
    if (!this.options) {
      return;
    }
    
    // Cancel existing preview
    if (this.previewState?.active) {
      this.cancelPreview();
    }
    
    const session = uiState.tone3000Session;
    if (!session?.accessToken) {
      showNotification("Preview failed", "No Tone3000 session");
      return;
    }
    
    // Update UI to show loading
    this.previewLoading = { toneId, modelId };
    this.renderTone3000List();
    if (this.tone3000Status) {
      this.tone3000Status.textContent = "Downloading for preview...";
    }
    
    try {
      // Download the model
      const response = await fetch(modelUrl, {
        headers: {
          Authorization: `Bearer ${session.accessToken}`,
        },
      });
      
      if (!response.ok) {
        throw new Error(`Download failed: ${response.status}`);
      }
      
      const buffer = await response.arrayBuffer();
      const contentType = response.headers.get("content-type") ?? "";
      const isZip = contentType.includes("zip") || modelUrl.toLowerCase().endsWith(".zip");
      
      // For preview, we send the file data to the plugin for temporary loading
      const data = arrayBufferToBase64(buffer);
      const tempResourceId = `preview:tone3000:${toneId}:${modelId}`;
      const resourceType = this.options.resourceType;
      
      // Send preview message to plugin
      postMessage({
        type: "previewRemoteResource",
        resourceType,
        tempResourceId,
        nodeId: this.options.nodeId,
        resourceIndex: this.options.resourceIndex,
        isZip,
        data,
      });
      
      this.previewState = {
        active: true,
        toneId,
        modelId,
        tempFilePath: "",
        tempResourceId,
      };
      
      this.renderTone3000List();
      
      if (this.tone3000Status) {
        this.tone3000Status.textContent = "Preview active - playing downloaded model";
      }
      
      showNotification("Preview started", "Playing Tone3000 model");
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      showNotification("Preview failed", message);
      if (this.tone3000Status) {
        this.tone3000Status.textContent = "";
      }
    } finally {
      if (this.previewLoading?.toneId === toneId && this.previewLoading?.modelId === modelId) {
        this.previewLoading = null;
        this.renderTone3000List();
      }
    }
  }
  
  private cancelPreview(): void {
    if (!this.previewState?.active || !this.options) {
      return;
    }
    
    // Send cancel preview message to plugin
    postMessage({
      type: "cancelPreviewResource",
      nodeId: this.options.nodeId,
      resourceIndex: this.options.resourceIndex,
    });
    
    this.previewState = null;
    this.previewLoading = null;
    this.renderTone3000List();
    
    if (this.tone3000Status) {
      this.tone3000Status.textContent = "";
    }
  }
  
  private async selectAndImportModel(toneId: string, modelId: string, modelUrl: string, modelName: string): Promise<void> {
    if (!this.options) {
      return;
    }
    
    // Cancel any active preview first
    if (this.previewState?.active) {
      this.cancelPreview();
    }
    
    const session = uiState.tone3000Session;
    if (!session?.accessToken) {
      showNotification("Import failed", "No Tone3000 session");
      return;
    }
    
    const tone = this.tone3000Tones.find((t) => String(t.id) === toneId);
    if (!tone) {
      showNotification("Import failed", "Tone not found");
      return;
    }
    
    if (this.tone3000Status) {
      this.tone3000Status.textContent = "Importing...";
    }
    
    try {
      // Download the model
      const response = await fetch(modelUrl, {
        headers: {
          Authorization: `Bearer ${session.accessToken}`,
        },
      });
      
      if (!response.ok) {
        throw new Error(`Download failed: ${response.status}`);
      }
      
      const buffer = await response.arrayBuffer();
      const contentType = response.headers.get("content-type") ?? "";
      const isZip = contentType.includes("zip") || modelUrl.toLowerCase().endsWith(".zip");
      const resourceType = this.options.resourceType;
      
      // Import the resource
      const resourceId = await this.importTone3000Resource(
        tone,
        modelId,
        modelName,
        buffer,
        isZip,
        resourceType
      );
      
      if (this.tone3000Status) {
        this.tone3000Status.textContent = "";
      }
      
      showNotification("Imported", modelName);
      
      // Select the imported resource and close
      this.selectedResourceId = resourceId;
      this.confirmSelection();
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      showNotification("Import failed", message);
      if (this.tone3000Status) {
        this.tone3000Status.textContent = "";
      }
    }
  }
  
  private async importTone3000Resource(
    tone: Tone3000Tone,
    modelId: string,
    modelName: string,
    buffer: ArrayBuffer,
    isZip: boolean,
    resourceType: "nam" | "ir"
  ): Promise<string> {
    const gearFolder = sanitizeFilename(tone.gear ?? "other");
    const toneFolder = sanitizeFilename(tone.title ?? tone.name ?? "tone");
    const subfolder = `${gearFolder}/${toneFolder}`;
    
    if (isZip) {
      // Handle zip file
      const zipLib = window.JSZip;
      if (!zipLib) {
        throw new Error("JSZip not loaded");
      }
      
      const zip = await zipLib.loadAsync(buffer);
      const entries = Object.values(zip.files) as JSZipObject[];
      let firstImportedId = "";
      
      for (const entry of entries) {
        if (entry.dir) continue;
        const lowerName = entry.name.toLowerCase();
        const isNam = lowerName.endsWith(".nam") || lowerName.endsWith(".json");
        const isIr = lowerName.endsWith(".wav") || lowerName.endsWith(".ir");
        
        if ((resourceType === "nam" && !isNam) || (resourceType === "ir" && !isIr)) {
          continue;
        }
        
        const fileBuffer = await entry.async("arraybuffer");
        const data = arrayBufferToBase64(fileBuffer);
        const fileName = sanitizeFilename(entry.name.split("/").pop() ?? modelName);
        const resourceId = `tone3000:${modelId}:${sanitizeFilename(entry.name)}`;
        
        postMessage({
          type: "importRemoteResource",
          provider: "tone3000",
          resourceType,
          resourceId,
          name: `${tone.title} - ${entry.name}`,
          description: tone.description ?? "",
          category: tone.gear ?? "",
          subfolder,
          fileName,
          metadata: {
            provider: "tone3000",
            toneId: String(tone.id),
            toneTitle: tone.title ?? "",
            groupId: String(tone.id),
            groupName: tone.title ?? tone.name ?? "",
            gear: tone.gear ?? "",
            platform: tone.platform ?? "",
            modelId: String(modelId),
            modelName: modelName ?? "",
            entryName: entry.name,
            sourceUrl: `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`,
            authorUsername: tone.user?.username ?? "",
          },
          data,
        });
        
        if (!firstImportedId) {
          firstImportedId = resourceId;
        }
      }
      
      if (!firstImportedId) {
        throw new Error("No supported files found in archive");
      }
      
      return firstImportedId;
    } else {
      // Single file
      const data = arrayBufferToBase64(buffer);
      const extension = resourceType === "ir" ? ".wav" : ".nam";
      const fileName = `${sanitizeFilename(modelName)}${extension}`;
      const resourceId = `tone3000:${modelId}`;
      
      postMessage({
        type: "importRemoteResource",
        provider: "tone3000",
        resourceType,
        resourceId,
        name: `${tone.title} - ${modelName}`,
        description: tone.description ?? "",
        category: tone.gear ?? "",
        subfolder,
        fileName,
        metadata: {
          provider: "tone3000",
          toneId: String(tone.id),
          toneTitle: tone.title ?? "",
          groupId: String(tone.id),
          groupName: tone.title ?? tone.name ?? "",
          gear: tone.gear ?? "",
          platform: tone.platform ?? "",
          modelId: String(modelId),
          modelName: modelName ?? "",
          sourceUrl: `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`,
          authorUsername: tone.user?.username ?? "",
        },
        data,
      });
      
      return resourceId;
    }
  }
  
  private updateSelectButtonState(): void {
    if (!this.selectBtn) {
      return;
    }
    
    // Only enable select button if we have a library resource selected
    const hasSelection = Boolean(this.selectedResourceId) && this.activeTab === "library";
    this.selectBtn.disabled = !hasSelection;
    this.selectBtn.textContent = hasSelection ? "Select" : "Select from Library";
  }
  
  private confirmSelection(): void {
    if (!this.options || !this.selectedResourceId) {
      return;
    }
    
    // Cancel any Tone3000 preview first
    if (this.previewState?.active) {
      this.cancelPreview();
    }
    
    // Mark that we're committing the selection (don't revert on close)
    this.libraryPreviewActive = false;
    this.originalResourceId = this.selectedResourceId;
    
    // Get resource name for notification
    const resourceType = this.options.resourceType;
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const resource = resources.find(r => r.id === this.selectedResourceId);
    const displayName = resource?.name || this.selectedResourceId;
    
    this.options.onSelect(this.selectedResourceId);
    showNotification("Selected", displayName);
    this.close();
  }
}

// Singleton instance
export const resourceBrowserModal = new ResourceBrowserModal();

// Helper functions
function escapeHtml(value: string | number | null | undefined): string {
  const text = value === null || value === undefined ? "" : String(value);
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function sanitizeFilename(raw: string): string {
  const trimmed = raw.trim() || "resource";
  return trimmed.replace(/[^a-z0-9-_\.]+/gi, "-");
}

// Type definition for JSZip entries
interface JSZipObject {
  name: string;
  dir: boolean;
  async(type: "arraybuffer"): Promise<ArrayBuffer>;
}
