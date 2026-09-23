/**
 * Resource Browser Modal
 *
 * Enhanced modal for selecting NAM models and IR cabs with:
 * - Resource Library tab (existing library items)
 * - Folder tab (browsing the filesystem, in ./resourceBrowser/folderTab.ts)
 * - Tone3000 tab (browse and preview remote items)
 * - Preview/temporary loading before import
 */
import { postMessage, sendResourceFavorite } from "./bridge.js";
import { recordAppSetting } from "./appSettingsStore.js";
import { showConfirm } from "./dialogs.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "./featureFlags.js";
import { getPlaySvg } from "./iconAssets.js";
import { showNotification } from "./notifications.js";
import { FolderTab } from "./resourceBrowser/folderTab.js";
import { folderFileLibraryMatch } from "./resourceBrowser/folderRows.js";
import { normalizeArchitectureBadge, getResourceCreator, getResourceLibraryFacets, getResourceTags, isDoubleClickExempt, normalizeFilterValue, normalizeNamArchitectureBadge, resolveLibraryCategoryFromHint, splitTagValues } from "./resourceBrowser/helpers.js";
import { DEFAULT_RESOURCE_CONTEXT_KEY, RESOURCE_FAVORITES_SETTING } from "./resourceBrowser/settings.js";
import { Tone3000Tab } from "./resourceBrowser/tone3000Tab.js";
import type { LibraryFilterSnapshot, NavigationCacheOptions, PersistedResourceBrowserState, PreviewLoadingState, PreviewState, ResourceBrowserOptions, ResourceBrowserTab, ResourceImportedDetail, ResourceNavigationResult, ResourceNavigationState, ResourceType } from "./resourceBrowser/types.js";
import { deduplicateResourcesByHashAndPath, resolveResourceIdAlias } from "./resourceDedup.js";
import { uiState } from "./state.js";
import { isTone3000AuthReady, tone3000AuthenticatedFetch } from "./tone3000.js";
import type { Tone3000Architecture } from "./tone3000ApiTypes.js";
import type { LibraryResource } from "./types.js";
import { arrayBufferToBase64, copyTextToClipboard, escapeHtml, findResourceById } from "./utils.js";

export type { ResourceNavigationResult } from "./resourceBrowser/types.js";

export class ResourceBrowserModal {
  /**
   * The Folder tab, which owns its own DOM and listing state.
   *
   * It is handed an adapter of closures rather than this modal, so the members
   * it needs stay private to this class — the adapter is built inside it.
   */
  private readonly folderTab: FolderTab;

  /** The Tone3000 tab, on the same terms as the Folder tab. */
  private readonly tone3000Tab: Tone3000Tab;

  constructor() {
    this.tone3000Tab = new Tone3000Tab({
      getOptions: () => this.options,
      getSelectedArchitecture: () => this.getSelectedArchitecture(),
      getContextKey: () => this.folderTab.folderContextKey,
      rememberNavigationView: (contextKey, view) => this.lastNavigationViewByContext.set(contextKey, view),
      getNavigationView: (contextKey) => this.lastNavigationViewByContext.get(contextKey),
      getPreviewState: () => this.previewState,
      getPreviewLoading: () => this.previewLoading,
      startPreview: (toneId, modelId, modelUrl) => this.startPreview(toneId, modelId, modelUrl),
      cancelPreview: (restoreOriginal) => this.cancelPreview(restoreOriginal),
      selectAndImportModel: (toneId, modelId, modelUrl, modelName) =>
        this.selectAndImportModel(toneId, modelId, modelUrl, modelName),
      renderFavoritesPrompt: () => this.renderFavoritesPrompt(),
      saveState: () => this.saveCurrentStateForResourceType(),
    });
    this.folderTab = new FolderTab({
      getOptions: () => this.options,
      setLibraryPreviewActive: (active) => {
        this.libraryPreviewActive = active;
      },
      closeModal: () => this.close(),
      isResourceFavorite: (resourceId) => this.isResourceFavorite(resourceId),
      setResourceFavorite: (resourceId, isFavorite) => this.setResourceFavorite(resourceId, isFavorite),
      renderLibraryList: () => this.renderLibraryList(),
      openEditPopover: (resourceId, resourceType) => this.openEditPopover(resourceId, resourceType),
      openEditPopoverForValues: (options) => this.openEditPopoverForValues(options),
      resolveDefaultImportCategory: (fallbackCategory) => this.resolveDefaultImportCategory(fallbackCategory),
      updateSelectButtonState: () => this.updateSelectButtonState(),
      rememberNavigationView: (contextKey, view) => this.lastNavigationViewByContext.set(contextKey, view),
    });
  }

  /**
   * Whether the node prev/next controls should keep walking a Tone3000 result
   * set. Forwarded from the Tone3000 tab, which owns that state.
   */
  isTone3000NavigationActive(resourceType: ResourceType, options?: NavigationCacheOptions): boolean {
    return this.tone3000Tab.navigation.isTone3000NavigationActive(resourceType, options);
  }

  /** Move to the next or previous model in that result set, importing if needed. */
  stepTone3000Resource(
    resourceType: ResourceType,
    currentResourceId: string,
    offset: number,
    options?: NavigationCacheOptions,
  ): Promise<ResourceNavigationResult | null> {
    return this.tone3000Tab.navigation.stepTone3000Resource(resourceType, currentResourceId, offset, options);
  }

  private initialized = false;
  private options: ResourceBrowserOptions | null = null;
  private previewState: PreviewState | null = null;
  private originalResourceId: string = ""; // Track original for revert on cancel
  private libraryPreviewActive = false;
  private expandedLibraryItemId: string | null = null;
  
  // DOM elements
  private modal: HTMLElement | null = null;
  private title: HTMLElement | null = null;
  private closeBtn: HTMLButtonElement | null = null;
  private cancelBtn: HTMLButtonElement | null = null;
  private selectBtn: HTMLButtonElement | null = null;
  private footerHint: HTMLElement | null = null;
  private editPopover: HTMLElement | null = null;
  private editNameInput: HTMLInputElement | null = null;
  private editCategoryInput: HTMLInputElement | null = null;
  private editTagsInput: HTMLInputElement | null = null;
  private editSaveBtn: HTMLButtonElement | null = null;
  private editCancelBtn: HTMLButtonElement | null = null;
  private editingResourceId = "";
  private editingResourceType: ResourceType | null = null;
  private editingFolderPath = "";
  private editingFolderResourceType: ResourceType | null = null;
  
  // Tab elements
  private tabsContainer: HTMLElement | null = null;
  private tabButtons: HTMLButtonElement[] = [];
  private tabPanels: HTMLElement[] = [];
  private activeTab: ResourceBrowserTab = "library";
  
  // Library tab elements
  private librarySearch: HTMLInputElement | null = null;
  private libraryCategory: HTMLSelectElement | null = null;
  private libraryArchitecture: HTMLSelectElement | null = null;
  private libraryCreator: HTMLSelectElement | null = null;
  private libraryTagFilterBar: HTMLElement | null = null;
  private libraryFavoritesAllBtn: HTMLButtonElement | null = null;
  private libraryFavoritesOnlyBtn: HTMLButtonElement | null = null;
  private libraryFavoritesOnly = false;
  private libraryNavPrevBtn: HTMLButtonElement | null = null;
  private libraryNavNextBtn: HTMLButtonElement | null = null;
  private libraryBrowseBtn: HTMLButtonElement | null = null;
  private libraryList: HTMLElement | null = null;
  private selectedResourceId: string = "";
  private libraryCreatorFilter = "all";
  private libraryArchitectureFilter = "all";
  private libraryTagFilters: Set<string> = new Set();

  // Per-type cache so preloads for different resource types don't clobber each other.
  private libraryNavigationStates: Map<string, ResourceNavigationState> = new Map();
  private pendingLibraryNavigationRefreshes: Map<string, number> = new Map();
  private libraryResourceAliases: Map<string, Map<string, string>> = new Map(); // Maps resourceType -> (aliasId -> canonicalId)
  private lastNavigationViewByContext: Map<string, "library" | "folder" | "tone3000"> = new Map();
  
  
  private previewLoading: PreviewLoadingState | null = null;
  private persistedStateByType: Partial<Record<ResourceType, PersistedResourceBrowserState>> = {};
  private resourceUsageInfo: Map<string, { inUse: boolean; presetName?: string }> = new Map();
  private requestedUsageKeys: Set<string> = new Set();
  private usageObserver: IntersectionObserver | null = null;

  private handleResourceImportedEvent = (event: Event): void => {
    const detail = (event as CustomEvent<ResourceImportedDetail>).detail;
    if (!this.options || !this.modal || this.modal.style.display !== "flex") {
      return;
    }

    const importedNorm = (detail?.filePath ?? "").replace(/\\/g, "/").toLowerCase();
    const importedId = (detail?.id ?? "").trim();
    if (importedId && importedNorm && this.folderTab.folderListing) {
      const file = this.folderTab.folderListing.files.find((entry) => entry.path.replace(/\\/g, "/").toLowerCase() === importedNorm);
      if (file) {
        file.alreadyInLibrary = true;
        file.libraryId = importedId;
      }
    }
    if (importedId && importedNorm && this.folderTab.pendingFolderFavoritePaths.has(importedNorm)) {
      this.setResourceFavorite(importedId, true);
      this.folderTab.pendingFolderFavoritePaths.delete(importedNorm);
      if (this.folderTab.folderListing) {
        const file = this.folderTab.folderListing.files.find((entry) => entry.path.replace(/\\/g, "/").toLowerCase() === importedNorm);
        if (file) {
          file.alreadyInLibrary = true;
          file.libraryId = importedId;
        }
      }
    }

    // Complete a pending folder-tab "Select" once its import lands.
    if (this.folderTab.pendingFolderSelectPath) {
      const pendingNorm = this.folderTab.pendingFolderSelectPath.replace(/\\/g, "/").toLowerCase();
      if (importedId && importedNorm === pendingNorm) {
        const displayName = this.folderTab.folderFileDisplayName(this.folderTab.pendingFolderSelectPath);
        this.folderTab.finalizeFolderSelection(importedId, displayName);
        return;
      }
      // Fallback: if the imported path didn't normalize identically, resolve the id
      // from the (now refreshed) library listing for the pending file.
      const pendingFile = this.folderTab.folderListing?.files.find(
        (f) => f.path.replace(/\\/g, "/").toLowerCase() === pendingNorm,
      );
      if (pendingFile) {
        const resolved = folderFileLibraryMatch(pendingFile);
        if (resolved.inLibrary && resolved.id) {
          const displayName = this.folderTab.folderFileDisplayName(this.folderTab.pendingFolderSelectPath);
          this.folderTab.finalizeFolderSelection(resolved.id, displayName);
          return;
        }
      }
    }

    // The folder tab can import resources of any type, so refresh it regardless
    // of the node's current resource type.
    if (this.folderTab.folderList) {
      this.folderTab.renderFolderList();
    }

    const importedType = detail?.resourceType;
    if (!importedType || importedType !== this.options.resourceType) {
      return;
    }

    const resources = uiState.resourceLibrary[importedType] ?? [];
    const normalizedPath = (detail?.filePath ?? "").trim().replace(/\\/g, "/").toLowerCase();
    const matchedId = importedId
      || resources.find((resource) => {
        const resourcePath = (resource.filePath ?? "").trim().replace(/\\/g, "/").toLowerCase();
        return normalizedPath.length > 0 && resourcePath === normalizedPath;
      })?.id
      || "";

    // Ensure freshly imported local resources are visible even when a category filter is active.
    if (this.libraryCategory) {
      const hasAllOption = Array.from(this.libraryCategory.options).some((option) => option.value === "all");
      if (hasAllOption) {
        this.libraryCategory.value = "all";
      }
    }

    if (this.libraryArchitecture) {
      this.libraryArchitecture.disabled = importedType !== "nam";
      if (importedType !== "nam") {
        this.libraryArchitecture.value = "all";
      } else if (!this.libraryArchitecture.value) {
        this.libraryArchitecture.value = this.libraryArchitectureFilter || "all";
      }
    }

    this.renderLibraryList();

    if (matchedId) {
      this.selectedResourceId = matchedId;
      this.renderLibraryList();
      this.scrollSelectedLibraryItemIntoView();
      this.updateSelectButtonState();
      this.saveCurrentStateForResourceType();
    }
  };

  private handleResourceRemovedEvent = (event: Event): void => {
    if (!this.options || !this.modal || this.modal.style.display !== "flex") {
      return;
    }
    const detail = (event as CustomEvent<{ id?: string; resourceType?: string }>).detail;
    const removedId = (detail?.id ?? "").trim();
    if (removedId) {
      this.setResourceFavorite(removedId, false);
      if (this.selectedResourceId === removedId) {
        this.selectedResourceId = "";
        this.updateSelectButtonState();
      }
    }
    if (this.folderTab.folderList) {
      this.folderTab.renderFolderList();
    }
    this.renderLibraryList();
  };

  private handleUsageInfoEvent = (event: Event): void => {
    const detail = (event as CustomEvent<{ resourceType?: string; id?: string; inUse?: boolean; presetName?: string }>).detail;
    const resourceType = (detail?.resourceType ?? "").trim();
    const resourceId = (detail?.id ?? "").trim();
    
    if (!resourceType || !resourceId) {
      return;
    }

    const key = `${resourceType}:${resourceId}`;
    this.resourceUsageInfo.set(key, {
      inUse: detail.inUse ?? false,
      presetName: detail.presetName
    });

    // Patch only the affected row in place to avoid a full re-render
    // (and scroll reset) for every streamed usage response.
    if (this.libraryList && this.modal?.style.display === "flex") {
      this.updateRowUsage(resourceType, resourceId);
    }
  };

  private updateRowUsage(resourceType: string, resourceId: string): void {
    if (!this.libraryList || this.options?.resourceType !== resourceType) {
      return;
    }

    let row: HTMLElement | null = null;
    const rows = this.libraryList.querySelectorAll<HTMLElement>(".resource-browser-item-row[data-resource-id]");
    for (const candidate of Array.from(rows)) {
      if (candidate.dataset.resourceId === resourceId) {
        row = candidate;
        break;
      }
    }
    if (!row) {
      return;
    }

    const usage = this.resourceUsageInfo.get(`${resourceType}:${resourceId}`);
    const isInUse = usage?.inUse ?? false;

    const deleteBtn = row.querySelector<HTMLButtonElement>(".resource-browser-item-delete-btn");
    if (deleteBtn) {
      deleteBtn.disabled = isInUse;
      deleteBtn.title = "Delete from resource library";
    }
  };




  private initialize(): void {
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
    this.footerHint = this.modal.querySelector(".resource-browser-footer-hint") as HTMLElement | null;
    this.editPopover = document.getElementById("resource-browser-edit-popover");
    this.editNameInput = document.getElementById("resource-browser-edit-name") as HTMLInputElement | null;
    this.editCategoryInput = document.getElementById("resource-browser-edit-category") as HTMLInputElement | null;
    this.editTagsInput = document.getElementById("resource-browser-edit-tags") as HTMLInputElement | null;
    this.editSaveBtn = document.getElementById("resource-browser-edit-save") as HTMLButtonElement | null;
    this.editCancelBtn = document.getElementById("resource-browser-edit-cancel") as HTMLButtonElement | null;
    
    // Tab buttons and panels
    this.tabsContainer = this.modal.querySelector(".resource-browser-tabs") as HTMLElement | null;
    this.tabButtons = Array.from(this.modal.querySelectorAll(".resource-browser-tab-btn")) as HTMLButtonElement[];
    this.tabPanels = Array.from(this.modal.querySelectorAll(".resource-browser-tab-panel")) as HTMLElement[];
    
    // Library tab elements
    this.librarySearch = document.getElementById("resource-browser-library-search") as HTMLInputElement | null;
    this.libraryCategory = document.getElementById("resource-browser-library-category") as HTMLSelectElement | null;
    this.libraryArchitecture = document.getElementById("resource-browser-library-architecture") as HTMLSelectElement | null;
    this.libraryCreator = document.getElementById("resource-browser-library-creator") as HTMLSelectElement | null;
    this.libraryTagFilterBar = document.getElementById("resource-browser-library-tag-filters");
    this.libraryFavoritesAllBtn = document.getElementById("resource-browser-library-favorites-all") as HTMLButtonElement | null;
    this.libraryFavoritesOnlyBtn = document.getElementById("resource-browser-library-favorites-only") as HTMLButtonElement | null;
    this.libraryNavPrevBtn = document.getElementById("resource-browser-library-prev") as HTMLButtonElement | null;
    this.libraryNavNextBtn = document.getElementById("resource-browser-library-next") as HTMLButtonElement | null;
    this.libraryBrowseBtn = document.getElementById("resource-browser-library-browse") as HTMLButtonElement | null;
    this.libraryList = document.getElementById("resource-browser-library-list");

    
    // Bind events
    this.closeBtn?.addEventListener("click", () => this.close());
    this.cancelBtn?.addEventListener("click", () => this.close());
    this.selectBtn?.addEventListener("click", () => this.confirmSelection());
    this.editSaveBtn?.addEventListener("click", () => this.saveEditPopover());
    this.editCancelBtn?.addEventListener("click", () => this.closeEditPopover());
    this.editPopover?.addEventListener("mousedown", (event) => {
      if (event.target === this.editPopover) {
        this.closeEditPopover();
      }
    });
    this.editPopover?.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        this.closeEditPopover();
      }
    });
    
    this.modal.addEventListener("mousedown", (event) => {
      if (event.target === this.modal) {
        this.close();
      }
    });
    
    // Tab switching
    this.tabButtons.forEach((btn) => {
      btn.addEventListener("click", () => {
        const tab = btn.dataset.tab as ResourceBrowserTab | undefined;
        if (tab) {
          this.setActiveTab(tab);
        }
    });
    });
    
    // Library search
    this.librarySearch?.addEventListener("input", () => {
      this.renderLibraryList();
      this.saveCurrentStateForResourceType();
    });
    this.libraryCategory?.addEventListener("change", () => {
      this.renderLibraryList();
      this.saveCurrentStateForResourceType();
    });
    this.libraryArchitecture?.addEventListener("change", () => {
      this.libraryArchitectureFilter = this.libraryArchitecture?.value ?? "all";
      this.renderLibraryList();
      this.saveCurrentStateForResourceType();
    });
    this.libraryCreator?.addEventListener("change", () => {
      this.libraryCreatorFilter = this.libraryCreator?.value ?? "all";
      this.renderLibraryList();
      this.saveCurrentStateForResourceType();
    });
    this.libraryTagFilterBar?.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      const chip = target?.closest(".preset-tag-filter-chip") as HTMLButtonElement | null;
      if (!chip) {
        return;
      }
      const tag = chip.dataset.tag ?? "";
      if (!tag) {
        this.libraryTagFilters.clear();
        this.renderLibraryList();
        this.saveCurrentStateForResourceType();
        return;
      }
      if (tag === "__clear__") {
        this.libraryTagFilters.clear();
        this.renderLibraryList();
        this.saveCurrentStateForResourceType();
        return;
      }
      if (this.libraryTagFilters.has(tag)) {
        this.libraryTagFilters.delete(tag);
      } else {
        this.libraryTagFilters.add(tag);
      }
      this.renderLibraryList();
      this.saveCurrentStateForResourceType();
    });
    this.libraryFavoritesAllBtn?.addEventListener("click", () => {
      if (this.libraryFavoritesOnly) {
        this.libraryFavoritesOnly = false;
        this.syncLibraryFavoritesUi();
        this.renderLibraryList();
        this.saveCurrentStateForResourceType();
      }
    });
    this.libraryFavoritesOnlyBtn?.addEventListener("click", () => {
      if (!this.libraryFavoritesOnly) {
        this.libraryFavoritesOnly = true;
        this.syncLibraryFavoritesUi();
        this.renderLibraryList();
        this.saveCurrentStateForResourceType();
      }
    });
    this.libraryNavPrevBtn?.addEventListener("click", () => this.navigateLibrarySelection(-1));
    this.libraryNavNextBtn?.addEventListener("click", () => this.navigateLibrarySelection(1));
    this.libraryBrowseBtn?.addEventListener("click", () => this.browseForLibraryFile());
    
    // Library item click
    this.libraryList?.addEventListener("click", (event) => void this.handleLibraryClick(event));
    this.libraryList?.addEventListener("dblclick", (event) => this.handleLibraryDoubleClick(event));

    this.folderTab.initialize();

    this.tone3000Tab.initialize();

    document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => this.handleFeatureFlagsChanged());
    document.addEventListener("resource-browser:resource-imported", this.handleResourceImportedEvent as EventListener);
    document.addEventListener("resource-browser:resource-removed", this.handleResourceRemovedEvent as EventListener);
    document.addEventListener("resource-browser:usage-info", this.handleUsageInfoEvent as EventListener);
    this.syncAvailableTabs();
  }

  private createDefaultPersistedState(resourceType: ResourceType): PersistedResourceBrowserState {
    const isIr = resourceType === "ir";
    return {
      activeTab: "library",
      librarySearch: "",
      libraryCategory: "all",
      libraryArchitecture: "all",
      libraryCreator: "all",
      libraryTagFilters: [],
      libraryFavoritesOnly: false,
      tone3000Search: "",
      tone3000Category: isIr ? "ir" : "amp",
      tone3000Sort: "popular",
      tone3000Architecture: isIr ? "all" : "2",
      tone3000FavoritesOnly: false,
      tone3000Page: 1,
      tone3000TotalPages: 1,
      tone3000Tones: [],
      expandedToneId: null,
      toneModelsCache: [],
    };
  }

  private getOrCreatePersistedState(resourceType: ResourceType): PersistedResourceBrowserState {
    const existing = this.persistedStateByType[resourceType];
    if (existing) {
      return existing;
    }
    const created = this.createDefaultPersistedState(resourceType);
    this.persistedStateByType[resourceType] = created;
    return created;
  }

  private saveCurrentStateForResourceType(): void {
    const resourceType = this.options?.resourceType;
    if (!resourceType) {
      return;
    }

    const persisted = this.getOrCreatePersistedState(resourceType);
    persisted.activeTab = this.activeTab;
    persisted.librarySearch = this.librarySearch?.value ?? "";
    persisted.libraryCategory = this.libraryCategory?.value ?? "all";
    persisted.libraryArchitecture = this.libraryArchitecture?.value ?? this.libraryArchitectureFilter;
    persisted.libraryCreator = this.libraryCreator?.value ?? this.libraryCreatorFilter;
    persisted.libraryTagFilters = Array.from(this.libraryTagFilters);
    persisted.libraryFavoritesOnly = this.libraryFavoritesOnly;
    this.tone3000Tab.captureTone3000State(persisted, resourceType);
  }


  private restoreStateForResourceType(resourceType: ResourceType): void {
    const persisted = this.getOrCreatePersistedState(resourceType);

    if (this.librarySearch) {
      this.librarySearch.value = persisted.librarySearch;
      this.librarySearch.placeholder = resourceType === "ir"
        ? "Search IRs..."
        : "Search models...";
    }

    this.tone3000Tab.restoreTone3000Controls(persisted, resourceType);

    if (this.libraryCategory) {
      const hasOption = Array.from(this.libraryCategory.options).some((option) => option.value === persisted.libraryCategory);
      this.libraryCategory.value = hasOption ? persisted.libraryCategory : "all";
    }

    this.libraryArchitectureFilter = persisted.libraryArchitecture ?? "all";
    if (this.libraryArchitecture) {
      const hasOption = Array.from(this.libraryArchitecture.options).some((option) => option.value === this.libraryArchitectureFilter);
      this.libraryArchitecture.value = hasOption ? this.libraryArchitectureFilter : "all";
      this.libraryArchitecture.disabled = resourceType !== "nam";
      if (resourceType !== "nam") {
        this.libraryArchitecture.value = "all";
      }
    }

    this.libraryCreatorFilter = persisted.libraryCreator ?? "all";
    if (this.libraryCreator) {
      const hasOption = Array.from(this.libraryCreator.options).some((option) => option.value === this.libraryCreatorFilter);
      this.libraryCreator.value = hasOption ? this.libraryCreatorFilter : "all";
    }

    this.libraryTagFilters = new Set(Array.isArray(persisted.libraryTagFilters) ? persisted.libraryTagFilters : []);

    this.libraryFavoritesOnly = persisted.libraryFavoritesOnly ?? false;
    this.syncLibraryFavoritesUi();

    this.activeTab = persisted.activeTab;
    this.tone3000Tab.restoreTone3000Results(persisted);
  }





  private syncLibraryFavoritesUi(): void {
    if (this.libraryFavoritesAllBtn) {
      this.libraryFavoritesAllBtn.classList.toggle("active", !this.libraryFavoritesOnly);
      this.libraryFavoritesAllBtn.setAttribute("aria-pressed", !this.libraryFavoritesOnly ? "true" : "false");
    }
    if (this.libraryFavoritesOnlyBtn) {
      this.libraryFavoritesOnlyBtn.classList.toggle("active", this.libraryFavoritesOnly);
      this.libraryFavoritesOnlyBtn.setAttribute("aria-pressed", this.libraryFavoritesOnly ? "true" : "false");
    }
  }

  private renderFavoritesPrompt(): string {
    return `<div class="resource-browser-empty">To view your favourites, add your own Tone3000 API key under Settings.</div>`;
  }

  private resolveDefaultImportCategory(fallbackCategory: string): string {
    if (this.options?.resourceType === "ir" && this.options.libraryCategoryHint === "reverb") {
      return "reverb";
    }

    return fallbackCategory.trim() || "Local";
  }

  private resolveSelectedResourceCategory(resourceType: ResourceType): string {
    if (!this.selectedResourceId) {
      return "";
    }

    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const dedupResult = deduplicateResourcesByHashAndPath(resources, {
      preferredResourceIds: [this.selectedResourceId],
    });
    const canonicalId = resolveResourceIdAlias(this.selectedResourceId, dedupResult.aliasById);
    const selectedResource = findResourceById(dedupResult.deduped, canonicalId)
      ?? findResourceById(resources, this.selectedResourceId);
    return (selectedResource?.category ?? "").trim() || "Uncategorized";
  }

  private browseForLibraryFile(): void {
    if (!this.options?.nodeId) {
      return;
    }

    postMessage({
      type: "browseNodeResource",
      nodeId: this.options.nodeId,
      resourceType: this.options.resourceType,
      resourceIndex: this.options.resourceIndex,
      exposedResourceId: this.options.exposedResourceId,
      category: this.resolveDefaultImportCategory("Local"),
    });
  }

  private scrollSelectedLibraryItemIntoView(): void {
    if (!this.libraryList || !this.selectedResourceId) {
      return;
    }

    const selectedItem = this.libraryList.querySelector(
      `.resource-browser-item[data-resource-id="${CSS.escape(this.selectedResourceId)}"]`,
    ) as HTMLElement | null;
    selectedItem?.scrollIntoView({ behavior: "instant", block: "center" });
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
    this.folderTab.folderPreviewActive = false;
    this.folderTab.folderPreviewPath = null;
    this.folderTab.selectedFolderPath = "";
    this.folderTab.folderTagFilters.clear();
    this.closeEditPopover();
    this.folderTab.pendingFolderSelectPath = null;
    this.folderTab.pendingFolderFavoritePaths.clear();

    // Each effect role browses from its own remembered folder, so drop the
    // in-session path when the picker is opened for a different kind of node.
    const contextKey = options.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
    if (contextKey !== this.folderTab.folderContextKey) {
      this.folderTab.folderContextKey = contextKey;
      this.folderTab.folderCurrentPath = "";
      this.folderTab.folderListing = null;
    }
    this.folderTab.folderListingFallbackAttempted = false;
    if (this.title) {
      this.title.textContent = options.resourceType === "ir" 
        ? "Select IR Cabinet" 
        : "Select Amp Model";
    }
    
    // Update category options
    this.updateCategoryOptions();
    this.restoreStateForResourceType(options.resourceType);

    // Pre-select the library category based on effect node type, overriding
    // any persisted state. This ensures e.g. Neural FX always opens on Pedals.
    if (this.libraryCategory) {
      const categoryHint = options.libraryCategoryHint
        ?? (options.resourceType === "ir" ? "ir" : options.tone3000CategoryFilter);
      const availableCategories = Array.from(this.libraryCategory.options)
        .map((o) => o.value)
        .filter((v) => v !== "all");
      const match = categoryHint
        ? resolveLibraryCategoryFromHint(categoryHint, availableCategories)
        : null;
      if (match) {
        this.libraryCategory.value = match;
      }

      const selectedCategory = this.resolveSelectedResourceCategory(options.resourceType);
      if (selectedCategory && this.libraryCategory.value !== "all" && this.libraryCategory.value !== selectedCategory) {
        const hasSelectedCategory = availableCategories.includes(selectedCategory);
        this.libraryCategory.value = hasSelectedCategory ? selectedCategory : "all";
      }
    }
    
    // Render library list
    this.renderLibraryList();

    if (this.tone3000Tab.tone3000List) {
      if (this.tone3000Tab.tone3000Tones.length > 0) {
        this.tone3000Tab.renderTone3000List();
      } else {
        this.tone3000Tab.tone3000List.innerHTML = this.tone3000Tab.tone3000FavoritesOnly
          ? `<div class="resource-browser-empty">No favourite tones found. Favourite them on Tone3000 first, then refresh.</div>`
          : `<div class="resource-browser-empty">Enter a search query to browse Tone3000.</div>`;
      }
    }
    this.tone3000Tab.updateTone3000Pagination(false);
    if (this.tone3000Tab.tone3000PageLabel) {
      this.tone3000Tab.tone3000PageLabel.textContent = this.tone3000Tab.tone3000TotalPages > 1
        ? `Page ${this.tone3000Tab.tone3000Page} of ${this.tone3000Tab.tone3000TotalPages}`
        : `Page ${this.tone3000Tab.tone3000Page}`;
    }
    this.syncAvailableTabs();
    
    this.setActiveTab(this.activeTab);
    
    // Update select button state
    this.updateSelectButtonState();
    this.saveCurrentStateForResourceType();
    
    this.modal.style.display = "flex";
    // Scroll the already-selected item into view once the modal is visible
    requestAnimationFrame(() => this.scrollSelectedLibraryItemIntoView());
  }
  
  close(): void {
    if (!this.modal) {
      return;
    }
    
    // Cancel any active Tone3000 preview
    if (this.previewState?.active) {
      this.cancelPreview();
    }
    
    // Revert library or folder preview if we changed the node and didn't commit
    const needLibraryRevert = this.libraryPreviewActive
      && this.selectedResourceId !== this.originalResourceId;
    const needFolderRevert = this.folderTab.folderPreviewActive;
    if (this.options && (needLibraryRevert || needFolderRevert)) {
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
    
    // Clear cached usage info when modal closes
    this.resourceUsageInfo.clear();
    this.requestedUsageKeys.clear();
    this.usageObserver?.disconnect();
    this.usageObserver = null;
    
    this.libraryPreviewActive = false;
    this.folderTab.folderPreviewActive = false;
    this.folderTab.folderPreviewPath = null;
    this.folderTab.selectedFolderPath = "";
    this.closeEditPopover();
    this.modal.style.display = "none";
    this.saveCurrentStateForResourceType();
    this.options = null;
  }

  private handleFeatureFlagsChanged(): void {
    if (!this.initialized) {
      return;
    }

    if (!isFeatureEnabled(Features.Tone3000)) {
      if (this.previewState?.active) {
        this.cancelPreview();
      }
      this.previewLoading = null;
      if (this.tone3000Tab.tone3000Status) {
        this.tone3000Tab.tone3000Status.textContent = "";
      }
    }

    this.syncAvailableTabs();
    this.updateSelectButtonState();
  }

  private syncAvailableTabs(): void {
    const tone3000Enabled = isFeatureEnabled(Features.Tone3000);
    const tone3000TabButton = this.tabButtons.find((button) => button.dataset.tab === "tone3000") ?? null;
    const tone3000TabPanel = this.tabPanels.find((panel) => panel.dataset.tabPanel === "tone3000") ?? null;

    // The tab bar always offers Library and Folder, so keep it visible.
    this.tabsContainer?.toggleAttribute("hidden", false);
    tone3000TabButton?.toggleAttribute("hidden", !tone3000Enabled);
    tone3000TabPanel?.toggleAttribute("hidden", !tone3000Enabled);

    if (!tone3000Enabled && this.activeTab === "tone3000") {
      this.setActiveTab("library");
      return;
    }

    this.tabButtons.forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.tab === this.activeTab && !btn.hasAttribute("hidden"));
    });

    this.tabPanels.forEach((panel) => {
      const isActive = panel.dataset.tabPanel === this.activeTab && !panel.hasAttribute("hidden");
      panel.classList.toggle("active", isActive);
    });
  }
  
  private setActiveTab(tab: ResourceBrowserTab): void {
    const resolvedTab = tab === "tone3000" && !isFeatureEnabled(Features.Tone3000) ? "library" : tab;
    this.activeTab = resolvedTab;
    
    this.tabButtons.forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.tab === resolvedTab && !btn.hasAttribute("hidden"));
    });
    
    this.tabPanels.forEach((panel) => {
      const isActive = panel.dataset.tabPanel === resolvedTab && !panel.hasAttribute("hidden");
      panel.classList.toggle("active", isActive);
    });
    
    // Run initial Tone3000 search if switching to that tab
    if (resolvedTab === "tone3000" && !this.tone3000Tab.tone3000Tones.length) {
      void this.tone3000Tab.runTone3000Search();
    }

    if (resolvedTab === "folder") {
      this.folderTab.initFolderTab();
    }

    if (resolvedTab === "library") {
      requestAnimationFrame(() => this.scrollSelectedLibraryItemIntoView());
    }

    this.updateSelectButtonState();
    this.saveCurrentStateForResourceType();
  }
  
  private updateCategoryOptions(): void {
    const resourceType = this.options?.resourceType ?? "nam";
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const tone3000CategoryFilter = this.options?.tone3000CategoryFilter;
    const currentCategory = this.libraryCategory?.value ?? "all";
    
    // Deduplicate resources first
    const dedupResult = deduplicateResourcesByHashAndPath(resources, {
      preferredResourceIds: this.selectedResourceId ? [this.selectedResourceId] : [],
    });
    const dedupedResources = dedupResult.deduped;
    
    // Collect unique categories
    const categories = new Set<string>();
    dedupedResources.forEach((res) => {
      const cat = (res.category ?? "").trim() || "Uncategorized";
      categories.add(cat);
    });
    
    const sorted = Array.from(categories).sort();
    
    if (this.libraryCategory) {
      this.libraryCategory.innerHTML = `<option value="all">All Categories</option>` +
        sorted.map((cat) => `<option value="${escapeHtml(cat)}">${escapeHtml(cat)}</option>`).join("");
      this.libraryCategory.disabled = false;
      if (currentCategory !== "all" && sorted.includes(currentCategory)) {
        this.libraryCategory.value = currentCategory;
      } else if (resourceType === "nam" && tone3000CategoryFilter) {
        const match = resolveLibraryCategoryFromHint(tone3000CategoryFilter, sorted);
        this.libraryCategory.value = match ?? "all";
      } else if (resourceType === "ir") {
        const match = resolveLibraryCategoryFromHint(this.options?.libraryCategoryHint ?? "ir", sorted);
        this.libraryCategory.value = match ?? "all";
      } else {
        this.libraryCategory.value = "all";
      }
    }
    
    // Tone3000 category options based on resource type
    if (this.tone3000Tab.tone3000Category) {
      if (resourceType === "ir") {
        this.tone3000Tab.tone3000Category.innerHTML = `<option value="ir" selected>Cab IRs</option>`;
        this.tone3000Tab.tone3000Category.value = "ir";
        this.tone3000Tab.tone3000Category.disabled = true;
      } else {
        this.tone3000Tab.tone3000Category.innerHTML = `
          <option value="amp" selected>Amps</option>
          <option value="pedal">Pedals (FX)</option>
          <option value="preamp">Preamps</option>
          <option value="full-rig">Full Rigs</option>
        `;
        this.tone3000Tab.tone3000Category.value = tone3000CategoryFilter ?? "amp";
        this.tone3000Tab.tone3000Category.disabled = false;
      }
    }

    if (this.tone3000Tab.tone3000Architecture) {
      const isIr = resourceType === "ir";
      this.tone3000Tab.tone3000Architecture.disabled = isIr;
      this.tone3000Tab.tone3000Architecture.value = isIr ? "all" : "2";
    }
  }

  private getSelectedArchitecture(): Tone3000Architecture | null {
    if (!this.tone3000Tab.tone3000Architecture || this.options?.resourceType === "ir") {
      return null;
    }
    const selected = this.tone3000Tab.tone3000Architecture.value;
    if (selected === "1" || selected === "2" || selected === "custom") {
      return selected;
    }
    return null;
  }

  private getLibraryResourceArchitecture(resource: LibraryResource): string {
    const metadata = resource.metadata ?? {};
    return normalizeNamArchitectureBadge(
      metadata.architectureVersion
      || metadata.architecture_version
      || metadata.architecture
      || "",
    );
  }

  private getLibraryFilterSnapshot(resourceType: ResourceType): LibraryFilterSnapshot {
    const persisted = this.getOrCreatePersistedState(resourceType);
    return {
      query: (this.librarySearch?.value ?? persisted.librarySearch ?? "").trim().toLowerCase(),
      category: this.libraryCategory?.value ?? persisted.libraryCategory ?? "all",
      architecture: this.libraryArchitecture?.value ?? persisted.libraryArchitecture ?? "all",
      creator: this.libraryCreator?.value ?? persisted.libraryCreator ?? "all",
      tags: this.libraryTagFilters.size > 0 ? Array.from(this.libraryTagFilters) : [...(persisted.libraryTagFilters ?? [])],
      favoritesOnly: this.libraryFavoritesOnly,
    };
  }

  private buildLibraryNavigationCacheKey(resourceType: ResourceType, categoryHint?: string): string {
    const normalizedHint = (categoryHint ?? "").trim().toLowerCase();
    return normalizedHint ? `${resourceType}:${normalizedHint}` : resourceType;
  }

  private dispatchLibraryNavigationCacheUpdated(resourceType: ResourceType, categoryHint?: string): void {
    document.dispatchEvent(new CustomEvent("resource-browser:navigation-cache-updated", {
      detail: {
        resourceType,
        categoryHint: (categoryHint ?? "").trim().toLowerCase(),
        view: "library",
      },
    }));
  }

  private navigationStatesEqual(left: ResourceNavigationState | null | undefined, right: ResourceNavigationState | null | undefined): boolean {
    if (!left || !right) {
      return left === right;
    }
    if (left.resourceType !== right.resourceType || left.items.length !== right.items.length) {
      return false;
    }
    return left.items.every((item, index) => {
      const other = right.items[index];
      return item.resourceId === other.resourceId && item.filePath === other.filePath;
    });
  }

  private buildLibraryNavigationState(resourceType: ResourceType, options?: NavigationCacheOptions): ResourceNavigationState {
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const filters = this.getLibraryFilterSnapshot(resourceType);
    
    // Deduplicate resources by hash and file path
    const dedupResult = deduplicateResourcesByHashAndPath(resources, {
      preferredResourceIds: this.selectedResourceId ? [this.selectedResourceId] : [],
    });
    this.libraryResourceAliases.set(resourceType, dedupResult.aliasById);

    const dedupedResources = dedupResult.deduped;
    const availableCategories = Array.from(new Set(
      dedupedResources.map((res) => (res.category ?? "").trim() || "Uncategorized"),
    ));
    const resolvedCategoryHint = options?.categoryHint
      ? (resolveLibraryCategoryFromHint(options.categoryHint, availableCategories) ?? options.categoryHint)
      : "";
    const effectiveCategory = resolvedCategoryHint || filters.category;

    let filtered = dedupedResources.filter((res) => !res.fileMissing);

    if (effectiveCategory !== "all") {
      filtered = filtered.filter((res) => ((res.category ?? "").trim() || "Uncategorized") === effectiveCategory);
    }

    if (resourceType === "nam" && filters.architecture !== "all") {
      filtered = filtered.filter((res) => this.getLibraryResourceArchitecture(res) === filters.architecture);
    }

    if (filters.creator !== "all") {
      const creatorFilter = normalizeFilterValue(filters.creator);
      filtered = filtered.filter((res) => normalizeFilterValue(getResourceCreator(res)) === creatorFilter);
    }

    if (filters.tags.length > 0) {
      filtered = filtered.filter((res) => {
        const resourceTags = getResourceTags(res).map(normalizeFilterValue);
        return filters.tags.every((tag) => resourceTags.includes(normalizeFilterValue(tag)));
      });
    }

    if (filters.favoritesOnly) {
      filtered = filtered.filter((res) => this.isResourceFavorite(res.id));
    }

    if (filters.query) {
      filtered = filtered.filter((res) => {
        const haystack = [
          res.name,
          res.id,
          res.category,
          res.description,
          getResourceCreator(res),
          ...getResourceTags(res),
        ].join(" ").toLowerCase();
        return haystack.includes(filters.query);
      });
    }

    filtered.sort((a, b) => {
      const aFav = this.isResourceFavorite(a.id);
      const bFav = this.isResourceFavorite(b.id);
      if (aFav !== bFav) {
        return aFav ? -1 : 1;
      }
      const leftName = (a.name || a.id);
      const rightName = (b.name || b.id);
      const byName = leftName.localeCompare(rightName);
      if (byName !== 0) {
        return byName;
      }
      return (a.filePath ?? "").localeCompare(b.filePath ?? "");
    });

    return {
      resourceType,
      items: filtered.map((res) => ({ resourceId: res.id })),
    };
  }

  public preloadLibraryNavigationCache(resourceType: ResourceType, options?: NavigationCacheOptions): void {
    const categoryHint = options?.categoryHint;
    const cacheKey = this.buildLibraryNavigationCacheKey(resourceType, categoryHint);
    if (this.pendingLibraryNavigationRefreshes.has(cacheKey)) {
      return;
    }

    const refreshHandle = window.setTimeout(() => {
      this.pendingLibraryNavigationRefreshes.delete(cacheKey);

      const state = this.buildLibraryNavigationState(resourceType, options);
      const previous = this.libraryNavigationStates.get(cacheKey) ?? null;
      this.libraryNavigationStates.set(cacheKey, state);

      if (!this.navigationStatesEqual(previous, state)) {
        this.dispatchLibraryNavigationCacheUpdated(resourceType, categoryHint);
      }
    }, 0);

    this.pendingLibraryNavigationRefreshes.set(cacheKey, refreshHandle);
  }

  /// Resolves a NAM architecture badge (A1/A2), falling back to the NAM
  /// top-level "architecture" token (e.g. "WaveNet" -> A1, "SlimmableContainer"
  /// -> A2) when an explicit version is not present in the metadata.

  private async copyLocalLibraryPath(resourceId: string): Promise<void> {
    if (!this.options) {
      return;
    }

    const resource = findResourceById(uiState.resourceLibrary[this.options.resourceType] ?? [], resourceId);
    if (!resource) {
      showNotification("Copy path failed", "Resource not found.");
      return;
    }

    const path = (resource.filePath ?? "").trim();
    if (!path) {
      showNotification("Copy path unavailable", "This resource does not have a local file path.");
      return;
    }

    try {
      await copyTextToClipboard(path);
      showNotification("Path copied", path);
    } catch {
      const promptResult = window.prompt("Copy local resource path", path);
      if (promptResult === null) {
        showNotification("Copy cancelled", "Local path was not copied.");
        return;
      }
      showNotification("Path ready", "Local resource path is shown for manual copy.");
    }
  }
  
  private isResourceFavorite(resourceId: string): boolean {
    const raw = uiState.appSettings?.[RESOURCE_FAVORITES_SETTING];
    if (!Array.isArray(raw)) {
      return false;
    }
    return raw.includes(resourceId);
  }

  private setResourceFavorite(resourceId: string, isFavorite: boolean): void {
    if (this.isResourceFavorite(resourceId) === isFavorite) {
      return;
    }

    // The engine edits the one entry and answers with the whole setting ("appSettingChanged");
    // the local copy changes now so the star redraws without waiting for it.
    const raw = uiState.appSettings?.[RESOURCE_FAVORITES_SETTING];
    const favorites = Array.isArray(raw) ? raw.filter((val): val is string => typeof val === "string") : [];
    recordAppSetting(RESOURCE_FAVORITES_SETTING, isFavorite ? [...favorites, resourceId] : favorites.filter((id) => id !== resourceId));
    sendResourceFavorite(resourceId, isFavorite);
  }

  private toggleResourceFavorite(resourceId: string): void {
    this.setResourceFavorite(resourceId, !this.isResourceFavorite(resourceId));

    // Re-render library list to show changes
    this.renderLibraryList();
  }

  private renderLibraryFilterFacets(resources: LibraryResource[]): void {
    const { tags, creators } = getResourceLibraryFacets(resources);

    if (this.libraryCreator) {
      const currentValue = this.libraryCreatorFilter || "all";
      this.libraryCreator.innerHTML = [
        `<option value="all">All Creators</option>`,
        ...creators.map((creator) => `<option value="${escapeHtml(creator)}">${escapeHtml(creator)}</option>`),
      ].join("");
      this.libraryCreator.value = creators.includes(currentValue) ? currentValue : "all";
      this.libraryCreatorFilter = this.libraryCreator.value;
    }

    if (this.libraryTagFilterBar) {
      if (!tags.length) {
        this.libraryTagFilterBar.innerHTML = `<span class="resource-browser-empty">No tags available for these resources.</span>`;
        return;
      }

      const activeTags = this.libraryTagFilters;
      const chips = tags.map((tag) => {
        const active = activeTags.has(tag);
        return `<button class="preset-tag-filter-chip${active ? " active" : ""}" type="button" data-tag="${escapeHtml(tag)}">${escapeHtml(tag)}</button>`;
      });
      this.libraryTagFilterBar.innerHTML = [
        `<button class="preset-tag-filter-chip${activeTags.size === 0 ? " active" : ""}" type="button" data-tag="">All Tags</button>`,
        ...chips,
        activeTags.size > 0
          ? `<button class="preset-tag-filter-chip" type="button" data-tag="__clear__">Clear</button>`
          : "",
      ].join("");
    }
  }

  private renderLibraryList(): void {
    if (!this.libraryList || !this.options) {
      return;
    }
    
    const resourceType = this.options.resourceType;
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    
    // Deduplicate resources by hash and file path
    const dedupResult = deduplicateResourcesByHashAndPath(resources, {
      preferredResourceIds: this.selectedResourceId ? [this.selectedResourceId] : [],
    });
    this.libraryResourceAliases.set(resourceType, dedupResult.aliasById);
    const dedupedResources = dedupResult.deduped;
    
    this.renderLibraryFilterFacets(dedupedResources);
    const query = (this.librarySearch?.value ?? "").trim().toLowerCase();
    const category = this.libraryCategory?.value ?? "all";
    const architecture = this.libraryArchitecture?.value ?? this.libraryArchitectureFilter ?? "all";
    const creator = this.libraryCreator?.value ?? this.libraryCreatorFilter ?? "all";
    const currentId = this.selectedResourceId;
    const activeTags = Array.from(this.libraryTagFilters);
    
    let filtered = dedupedResources.filter((res) => !res.fileMissing);
    
    if (category !== "all") {
      filtered = filtered.filter((res) => {
        const cat = (res.category ?? "").trim() || "Uncategorized";
        return cat === category;
      });
    }

    if (resourceType === "nam" && architecture !== "all") {
      filtered = filtered.filter((res) => this.getLibraryResourceArchitecture(res) === architecture);
    }

    if (creator !== "all") {
      const creatorFilter = normalizeFilterValue(creator);
      filtered = filtered.filter((res) => normalizeFilterValue(getResourceCreator(res)) === creatorFilter);
    }

    if (activeTags.length > 0) {
      filtered = filtered.filter((res) => {
        const resourceTags = getResourceTags(res).map(normalizeFilterValue);
        return activeTags.every((tag) => resourceTags.includes(normalizeFilterValue(tag)));
      });
    }

    if (this.libraryFavoritesOnly) {
      filtered = filtered.filter((res) => this.isResourceFavorite(res.id));
    }
    
    if (query) {
      filtered = filtered.filter((res) => {
        const haystack = [
          res.name,
          res.id,
          res.category,
          res.description,
          getResourceCreator(res),
          ...getResourceTags(res),
        ].join(" ").toLowerCase();
        return haystack.includes(query);
      });
    }
    
    filtered.sort((a, b) => {
      const aFav = this.isResourceFavorite(a.id);
      const bFav = this.isResourceFavorite(b.id);
      if (aFav !== bFav) {
        return aFav ? -1 : 1;
      }
      const leftName = (a.name || a.id);
      const rightName = (b.name || b.id);
      const byName = leftName.localeCompare(rightName);
      if (byName !== 0) {
        return byName;
      }
      return (a.filePath ?? "").localeCompare(b.filePath ?? "");
    });
    
    const navigationState: ResourceNavigationState = {
      resourceType,
      items: filtered.map((res) => ({ resourceId: res.id })),
    };
    const cacheKey = this.buildLibraryNavigationCacheKey(resourceType, category);
    this.libraryNavigationStates.set(cacheKey, navigationState);
    // The caller looks this list up by category *hint* ("amp", "ir"), which the
    // modal has already resolved to a concrete category ("Amps"). Register under
    // the hint too so node-panel next/prev sees exactly what the modal is showing.
    const hint = this.options.libraryCategoryHint ?? this.options.tone3000CategoryFilter;
    const hintCacheKey = this.buildLibraryNavigationCacheKey(resourceType, hint);
    if (hintCacheKey !== cacheKey) {
      this.libraryNavigationStates.set(hintCacheKey, navigationState);
    }
    this.lastNavigationViewByContext.set(this.folderTab.folderContextKey, "library");
    this.dispatchLibraryNavigationCacheUpdated(resourceType, category);

    if (!filtered.length) {
      this.libraryList.innerHTML = `<div class="results-empty resource-browser-empty">No ${resourceType === "ir" ? "IRs" : "models"} match the current filters.</div>`;
      return;
    }

    // Usage info is queried lazily as items scroll into view (see observeVisibleUsage).
    this.libraryList.innerHTML = filtered
      .map((res) => {
        const title = res.name?.trim() || res.id;
        const categoryLabel = (res.category ?? "").trim() || "Uncategorized";
        // Check if current ID matches this resource, or if current ID is aliased to this resource
        const aliasMap = this.libraryResourceAliases.get(resourceType) || new Map();
        const resolvedCurrentId = resolveResourceIdAlias(currentId, aliasMap);
        const isSelected = res.id === currentId || res.id === resolvedCurrentId;
        const selectedClass = isSelected ? "results-item resource-browser-item is-selected" : "results-item resource-browser-item";
        const metadata = res.metadata ?? {};
        const provider = metadata.provider ?? "";
        const providerBadge = provider ? `<span class="resource-browser-provider">${escapeHtml(provider)}</span>` : "";
        const authorUsername = metadata.authorUsername ?? metadata.modeledBy ?? "";
        const sourceUrl = metadata.sourceUrl ?? "";
        const authorBadge = authorUsername ? `<span class="resource-browser-author">by: ${escapeHtml(authorUsername)}</span>` : "";
        const sourceLinkBadge = sourceUrl.startsWith("https://www.tone3000.com/") ? `<a class="resource-browser-attribution-link" href="${escapeHtml(sourceUrl)}" target="_blank" rel="noopener noreferrer">↗ tone3000</a>` : "";
        const tags = getResourceTags(res);
        const tagsBadge = tags.length
          ? `<span class="resource-browser-tag-list">${tags.map((tag) => `<span class="resource-browser-tag-pill">${escapeHtml(tag)}</span>`).join("")}</span>`
          : "";
        const creator = getResourceCreator(res);
        const creatorBadge = creator
          ? `<span class="resource-browser-creator">by ${escapeHtml(creator)}</span>`
          : "";
        const architecture = resourceType === "nam"
          ? normalizeArchitectureBadge(
            metadata.architectureVersion
            || metadata.architecture_version
            || metadata.architecture
            || "",
          )
          : "";
        const architectureBadge = architecture
          ? `<span class="resource-browser-architecture-badge" title="Model architecture">${escapeHtml(architecture)}</span>`
          : "";
        const gearMake = metadata.gearMake ?? "";
        const gearModel = metadata.gearModel ?? "";
        const gearDesc = [gearMake, gearModel].filter(Boolean).join(" ");
        const gearDescBadge = gearDesc
          ? `<span class="resource-browser-gear-desc" title="${escapeHtml(gearDesc)}">${escapeHtml(gearDesc)}</span>`
          : "";
        const toneType = metadata.toneType ?? "";
        const toneTypeBadge = toneType
          ? `<span class="resource-browser-tone-type">${escapeHtml(toneType.replace(/_/g, " "))}</span>`
          : "";
        const isFav = this.isResourceFavorite(res.id);
        const favoriteAction = `<button class="resource-browser-action-icon-btn resource-browser-item-fav-toggle${isFav ? " is-active" : ""}" type="button" data-resource-id="${escapeHtml(res.id)}" title="${isFav ? "Remove from favourites" : "Add to favourites"}" aria-label="Toggle favourite">${isFav ? "★" : "☆"}</button>`;
        const providerNormalized = (metadata.provider ?? "").trim().toLowerCase();
        const isBuiltIn = providerNormalized === "built-in" || providerNormalized === "builtin" || providerNormalized === "factory";
        const editAction = isBuiltIn
          ? `<span class="resource-browser-item-action-spacer" aria-hidden="true"></span>`
          : `<button class="resource-browser-action-icon-btn resource-browser-item-edit-btn" type="button" data-resource-id="${escapeHtml(res.id)}" title="Edit name, category and tags" aria-label="Edit name, category and tags"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 1 1 3 3L7 19l-4 1 1-4Z"/></svg></button>`;
        
        const usageKey = `${resourceType}:${res.id}`;
        const usage = this.resourceUsageInfo.get(usageKey);
        const isInUse = usage?.inUse ?? false;
        const deleteDisabled = isInUse ? " disabled" : "";
        const deleteAction = `<button class="resource-browser-action-icon-btn resource-browser-item-delete-btn"${deleteDisabled} type="button" data-resource-id="${escapeHtml(res.id)}" title="Delete from resource library" aria-label="Delete from resource library"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 6h18"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/></svg></button>`;

        const isDetailsExpanded = this.expandedLibraryItemId === res.id;
        const entryClass = `resource-browser-library-entry${isDetailsExpanded ? " is-details-expanded" : ""}`;
        return `
          <div class="${entryClass}" data-source="library">
            <div class="${selectedClass} resource-browser-item-row" data-resource-id="${escapeHtml(res.id)}">
              <div class="results-item-main resource-browser-item-info">
                <div class="results-item-title resource-browser-item-title">${escapeHtml(title)}</div>
                <div class="results-item-meta resource-browser-item-meta">
                  <span>${escapeHtml(categoryLabel)}</span>
                  ${creatorBadge}
                  ${architectureBadge}${gearDescBadge}${toneTypeBadge}${providerBadge}${authorBadge}${sourceLinkBadge}${tagsBadge}
                </div>
              </div>
              <div class="resource-browser-item-actions">
                ${favoriteAction}
                ${editAction}
                <button class="resource-browser-action-icon-btn resource-browser-item-details-btn" type="button" data-resource-id="${escapeHtml(res.id)}" title="${isDetailsExpanded ? "Hide details" : "Show details"}" aria-expanded="${isDetailsExpanded ? "true" : "false"}" aria-label="Resource details">ℹ</button>
                ${deleteAction}
                <button class="resource-browser-action-icon-btn resource-browser-item-select${isSelected ? " is-active" : ""}" type="button" title="Select in effect" aria-label="Select in effect" aria-pressed="${isSelected ? "true" : "false"}">${getPlaySvg()}</button>
              </div>
            </div>
            ${isDetailsExpanded ? this.renderLibraryItemDetailsPanel(res) : ""}
          </div>
        `;
      })
      .join("");

    this.observeVisibleUsage(resourceType);
    this.updateLibraryNavigationButtons();
  }

  // Lazily query "in use" status only for library rows that scroll into view.
  // Rows in a hidden tab panel (display:none) never intersect, so switching to
  // the Tone3000 tab triggers zero usage queries until the Library tab is shown.
  private observeVisibleUsage(resourceType: string): void {
    if (!this.libraryList) {
      return;
    }

    if (typeof IntersectionObserver === "undefined") {
      // Fallback: query everything (older runtimes only).
      const rows = this.libraryList.querySelectorAll<HTMLElement>(".resource-browser-item-row[data-resource-id]");
      rows.forEach((row) => this.requestUsageForRow(resourceType, row));
      return;
    }

    this.usageObserver?.disconnect();
    this.usageObserver = new IntersectionObserver((entries, observer) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) {
          continue;
        }
        const row = entry.target as HTMLElement;
        this.requestUsageForRow(resourceType, row);
        observer.unobserve(row);
      }
    });

    const rows = this.libraryList.querySelectorAll<HTMLElement>(".resource-browser-item-row[data-resource-id]");
    rows.forEach((row) => this.usageObserver!.observe(row));
  }

  private requestUsageForRow(resourceType: string, row: HTMLElement): void {
    const resourceId = row.dataset.resourceId;
    if (!resourceId) {
      return;
    }
    const key = `${resourceType}:${resourceId}`;
    if (this.resourceUsageInfo.has(key) || this.requestedUsageKeys.has(key)) {
      return;
    }
    this.requestedUsageKeys.add(key);
    postMessage({
      type: "queryResourceUsage",
      resourceType,
      resourceId
    });
  }

  private renderLibraryItemDetailsPanel(res: LibraryResource): string {
    const METADATA_LABELS: Record<string, string> = {
      provider: "Provider",
      authorUsername: "Author",
      modeledBy: "Modeled By",
      sourceUrl: "Source",
      architectureVersion: "Architecture",
      architecture_version: "Architecture",
      architecture: "Architecture",
      namFileVersion: "NAM File Version",
      sampleRate: "Sample Rate (Hz)",
      namName: "Model Name",
      gearMake: "Gear Make",
      gearModel: "Gear Model",
      gear_type: "Gear Type",
      toneType: "Tone Type",
      inputLevelDbu: "Input Level (dBu)",
      outputLevelDbu: "Output Level (dBu)",
      modelDate: "Model Date",
      trainingFinalLoss: "Training Final Loss",
      archive: "Pack Archive",
      factoryArchiveKey: "Pack",
      factoryArchiveHash: "Pack Hash",
      originalId: "Source ID",
      sourceFileName: "Source File",
    };

    const metadata = res.metadata ?? {};
    const description = (res.description ?? "").trim();
    const rows: string[] = [];

    rows.push(`
      <tr>
        <td class="resource-browser-details-label">ID</td>
        <td class="resource-browser-details-value resource-browser-details-mono">${escapeHtml(res.id)}</td>
      </tr>
    `);

    const filePath = (res.filePath ?? "").trim();
    if (filePath) {
      rows.push(`
        <tr>
          <td class="resource-browser-details-label">File Path</td>
          <td class="resource-browser-details-value">
            <span class="resource-browser-details-path-group">
              <button class="resource-browser-action-icon-btn resource-browser-local-path-copy-btn" type="button" data-resource-id="${escapeHtml(res.id)}" title="Copy local file path" aria-label="Copy local file path"><svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg></button>
              <span class="resource-browser-details-mono">${escapeHtml(filePath)}</span>
            </span>
          </td>
        </tr>
      `);
    }

    const tags = getResourceTags(res);
    if (tags.length > 0) {
      rows.push(`
        <tr>
          <td class="resource-browser-details-label">Tags</td>
          <td class="resource-browser-details-value">${tags.map((tag) => `<span class="resource-browser-tag-pill">${escapeHtml(tag)}</span>`).join(" ")}</td>
        </tr>
      `);
    }

    for (const [key, value] of Object.entries(metadata)) {
      if (!value) continue;
      const label = METADATA_LABELS[key] ?? key.replace(/_/g, " ").replace(/([A-Z])/g, " $1").trim();
      let displayValue: string;
      if (key === "sourceUrl" && value.startsWith("http")) {
        displayValue = `<a class="resource-browser-details-link" href="${escapeHtml(value)}" target="_blank" rel="noopener noreferrer">${escapeHtml(value)}</a>`;
      } else {
        displayValue = escapeHtml(value);
      }
      rows.push(`
        <tr>
          <td class="resource-browser-details-label">${escapeHtml(label)}</td>
          <td class="resource-browser-details-value">${displayValue}</td>
        </tr>
      `);
    }

    return `
      <div class="resource-browser-item-details-panel">
        ${description ? `<p class="resource-browser-details-description">${escapeHtml(description)}</p>` : ""}
        <table class="resource-browser-details-table">
          <tbody>
            ${rows.join("")}
          </tbody>
        </table>
      </div>
    `;
  }

  private async handleLibraryClick(event: Event): Promise<void> {
    const target = event.target as HTMLElement | null;
    if (!target) {
      return;
    }

    const favToggleBtn = target.closest(".resource-browser-item-fav-toggle") as HTMLButtonElement | null;
    if (favToggleBtn) {
      const resourceId = favToggleBtn.dataset.resourceId ?? "";
      if (resourceId) {
        this.toggleResourceFavorite(resourceId);
      }
      return;
    }

    const detailsBtn = target.closest(".resource-browser-item-details-btn") as HTMLButtonElement | null;
    if (detailsBtn) {
      const resourceId = detailsBtn.dataset.resourceId ?? "";
      if (resourceId) {
        this.expandedLibraryItemId = this.expandedLibraryItemId === resourceId ? null : resourceId;
        this.renderLibraryList();
      }
      return;
    }

    const copyPathButton = target.closest(".resource-browser-local-path-copy-btn, .resource-browser-item-copy-path") as HTMLButtonElement | null;
    if (copyPathButton) {
      const resourceId = copyPathButton.dataset.resourceId ?? "";
      if (resourceId) {
        void this.copyLocalLibraryPath(resourceId);
      }
      return;
    }

    const editButton = target.closest(".resource-browser-item-edit-btn") as HTMLButtonElement | null;
    if (editButton) {
      const resourceId = editButton.dataset.resourceId ?? "";
      if (resourceId) {
        this.openEditPopover(resourceId, this.options?.resourceType ?? "nam");
      }
      return;
    }

    const deleteButton = target.closest(".resource-browser-item-delete-btn") as HTMLButtonElement | null;
    if (deleteButton) {
      // Skip if button is disabled (resource is in use)
      if (deleteButton.disabled) {
        return;
      }
      
      const resourceId = deleteButton.dataset.resourceId ?? "";
      if (!resourceId || !this.options) {
        return;
      }
      const resources = uiState.resourceLibrary[this.options.resourceType] ?? [];
      const resource = findResourceById(resources, resourceId);
      const displayName = (resource?.name ?? "").trim() || resourceId;
      const confirmed = await showConfirm(
        `Delete "${displayName}" from the Resource Library?\n\nIf the file was stored by the app it will be removed, files in other locations remain on disk.`,
        "Delete Resource",
      );
      if (!confirmed) {
        return;
      }
      postMessage({
        type: "deleteLibraryResource",
        resourceType: this.options.resourceType,
        resourceId,
      });
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
    this.applyLibrarySelectionHighlight();
    this.updateSelectButtonState();

    // Immediately preview the library resource
    this.previewLibraryResource(resourceId);
  }

  /// Moves the selection highlight without repainting the list. A full render
  /// would detach the row under the pointer between the two clicks of a double
  /// click, and a dblclick on a detached row never reaches the delegated
  /// listener on the list - so the shortcut below would silently never fire.
  private applyLibrarySelectionHighlight(): void {
    if (!this.libraryList || !this.options) {
      return;
    }

    const aliasMap = this.libraryResourceAliases.get(this.options.resourceType) ?? new Map<string, string>();
    const resolvedId = resolveResourceIdAlias(this.selectedResourceId, aliasMap);
    this.libraryList.querySelectorAll<HTMLElement>(".resource-browser-item-row[data-resource-id]").forEach((row) => {
      const id = row.dataset.resourceId ?? "";
      const isSelected = Boolean(id) && (id === this.selectedResourceId || id === resolvedId);
      row.classList.toggle("is-selected", isSelected);
      const selectBtn = row.querySelector(".resource-browser-item-select") as HTMLElement | null;
      selectBtn?.classList.toggle("is-active", isSelected);
      selectBtn?.setAttribute("aria-pressed", isSelected ? "true" : "false");
    });

    this.updateLibraryNavigationButtons();
  }

  /// A double click on a result is "preview it, then take it" - the first click
  /// of the pair has already selected and previewed, so this only has to commit.
  private handleLibraryDoubleClick(event: Event): void {
    const target = event.target as HTMLElement | null;
    if (!target || !this.options) {
      return;
    }
    if (isDoubleClickExempt(target)) {
      return;
    }

    const item = target.closest(".resource-browser-item") as HTMLElement | null;
    // Should a render between the two clicks (a library import landing, say)
    // still replace the row, the event lands on the container instead - but
    // what the first click selected is the row the user double clicked.
    const resourceId = item?.dataset.resourceId ?? this.selectedResourceId;
    if (!resourceId) {
      return;
    }

    this.selectedResourceId = resourceId;
    this.confirmSelection();
  }

  private previewLibraryResource(resourceId: string): void {
    if (!this.options) {
      return;
    }
    
    this.libraryPreviewActive = true;
    this.folderTab.folderPreviewActive = false;
    this.folderTab.folderPreviewPath = null;
    
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
    const resource = findResourceById(resources, resourceId);
    const displayName = resource?.name || resourceId;
    showNotification("Previewing", `${displayName} - click OK to confirm`);
  }

  private openEditPopover(resourceId: string, resourceType: ResourceType): void {
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const resource = findResourceById(resources, resourceId);
    if (!resource) {
      showNotification("Edit failed", "Resource not found.");
      return;
    }

    this.openEditPopoverForValues({
      resourceId,
      resourceType,
      name: (resource.name ?? "").trim() || resource.id,
      category: (resource.category ?? "").trim() || "Local",
      tags: getResourceTags(resource),
      provider: (resource.metadata?.provider ?? "").trim().toLowerCase(),
    });
  }

  private openEditPopoverForValues(options: {
    resourceId: string;
    resourceType: ResourceType;
    name: string;
    category: string;
    tags: string[];
    provider?: string;
    folderPath?: string;
  }): void {
    if (!this.options) {
      return;
    }

    const provider = (options.provider ?? "").trim().toLowerCase();
    if (provider === "built-in" || provider === "builtin" || provider === "factory") {
      showNotification("Edit unavailable", "Built-in resources cannot be edited.");
      return;
    }

    if (!this.editPopover || !this.editNameInput || !this.editCategoryInput || !this.editTagsInput) {
      return;
    }

    this.editingResourceId = options.resourceId;
    this.editingResourceType = options.resourceType;
    this.editingFolderPath = options.folderPath ?? "";
    this.editingFolderResourceType = options.folderPath ? options.resourceType : null;
    this.editNameInput.value = options.name.trim() || "Unnamed";
    this.editCategoryInput.value = options.category.trim() || "Local";
    this.editTagsInput.value = options.tags.join(", ");
    this.editPopover.hidden = false;
    this.editNameInput.focus();
    this.editNameInput.select();
  }

  private closeEditPopover(): void {
    if (this.editPopover) {
      this.editPopover.hidden = true;
    }
    this.editingResourceId = "";
    this.editingResourceType = null;
    this.editingFolderPath = "";
    this.editingFolderResourceType = null;
  }

  private saveEditPopover(): void {
    if (!this.options || !this.editNameInput || !this.editCategoryInput || !this.editTagsInput) {
      return;
    }

    const name = this.editNameInput.value.trim();
    const category = this.editCategoryInput.value.trim();
    const tagsInput = this.editTagsInput.value.trim();
    const tags = tagsInput ? Array.from(new Set(splitTagValues(tagsInput))) : [];

    if (this.editingResourceId) {
      const resourceId = this.editingResourceId;
      const resourceType = this.editingResourceType ?? this.options.resourceType;
      postMessage({
        type: "updateLibraryResource",
        resourceType,
        resourceId,
        name: name || resourceId,
        category: category || "Uncategorized",
        tags,
      });
      this.closeEditPopover();
      showNotification("Resource updated", name || resourceId);
      return;
    }

    if (this.editingFolderPath && this.editingFolderResourceType) {
      const path = this.editingFolderPath;
      const resourceType = this.editingFolderResourceType;
      postMessage(this.folderTab.buildFolderImportPayload(path, resourceType, {
        name: name || this.folderTab.folderFileDisplayName(path),
        category: category || (this.folderTab.folderListing?.name || "Folder"),
        tags,
      }));
      this.closeEditPopover();
      showNotification("Resource saved", name || this.folderTab.folderFileDisplayName(path));
    }
  }
  

  
  

  

  
  
  
  
  
  private async startPreview(toneId: string, modelId: string, modelUrl: string): Promise<void> {
    if (!this.options) {
      return;
    }
    
    // The current preview keeps playing while the next one downloads, and is not cancelled here:
    // the engine replaces it and keeps the node's original for close to restore. Cancelling first
    // left the node on a deleted temp file that the next preview then took for the original.
    if (!isTone3000AuthReady()) {
      showNotification("Preview failed", "No Tone3000 session");
      return;
    }
    
    // Update UI to show loading
    this.previewLoading = { toneId, modelId };
    this.tone3000Tab.renderTone3000List();
    if (this.tone3000Tab.tone3000Status) {
      this.tone3000Tab.tone3000Status.textContent = "Downloading for preview...";
    }
    
    try {
      // Download the model
      const response = await tone3000AuthenticatedFetch(modelUrl);
      
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
      
      this.tone3000Tab.renderTone3000List();
      
      if (this.tone3000Tab.tone3000Status) {
        this.tone3000Tab.tone3000Status.textContent = "Preview active - playing downloaded model";
      }
      
      showNotification("Preview started", "Playing Tone3000 model");
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      showNotification("Preview failed", message);
      if (this.tone3000Tab.tone3000Status) {
        this.tone3000Tab.tone3000Status.textContent = "";
      }
    } finally {
      if (this.previewLoading?.toneId === toneId && this.previewLoading?.modelId === modelId) {
        this.previewLoading = null;
        this.tone3000Tab.renderTone3000List();
      }
    }
  }
  
  private cancelPreview(restoreOriginal = true): void {
    if (!this.previewState?.active || !this.options) {
      return;
    }
    
    // Send cancel preview message to plugin
    postMessage({
      type: "cancelPreviewResource",
      nodeId: this.options.nodeId,
      resourceIndex: this.options.resourceIndex,
      restoreOriginal,
    });
    
    this.previewState = null;
    this.previewLoading = null;
    this.tone3000Tab.renderTone3000List();
    
    if (this.tone3000Tab.tone3000Status) {
      this.tone3000Tab.tone3000Status.textContent = "";
    }
  }
  
  private async selectAndImportModel(toneId: string, modelId: string, modelUrl: string, modelName: string): Promise<void> {
    if (!this.options) {
      return;
    }
    
    if (!isTone3000AuthReady()) {
      showNotification("Import failed", "No Tone3000 session");
      return;
    }
    
    const tone = this.tone3000Tab.tone3000Tones.find((t) => String(t.id) === toneId);
    if (!tone) {
      showNotification("Import failed", "Tone not found");
      return;
    }

    const modelArchitecture = this.tone3000Tab.toneModelsCache
      .get(toneId)
      ?.find((model) => String(model.id) === modelId)
      ?.architecture_version;
    
    if (this.tone3000Tab.tone3000Status) {
      this.tone3000Tab.tone3000Status.textContent = "Importing...";
    }
    
    try {
      // Download the model
      const response = await tone3000AuthenticatedFetch(modelUrl);
      
      if (!response.ok) {
        throw new Error(`Download failed: ${response.status}`);
      }
      
      const buffer = await response.arrayBuffer();
      const contentType = response.headers.get("content-type") ?? "";
      const isZip = contentType.includes("zip") || modelUrl.toLowerCase().endsWith(".zip");
      const resourceType = this.options.resourceType;
      
      // Import the resource
      const resourceId = await this.tone3000Tab.navigation.importTone3000Resource(
        tone,
        modelId,
        modelName,
        modelArchitecture ?? "",
        buffer,
        isZip,
        resourceType
      );
      
      if (this.tone3000Tab.tone3000Status) {
        this.tone3000Tab.tone3000Status.textContent = "";
      }
      
      showNotification("Imported", modelName);

      // Hand the result set to next/prev before closing so the node's controls
      // keep walking the list the user picked from.
      this.tone3000Tab.navigation.captureTone3000NavigationState();

      // Select the imported resource and close
      this.selectedResourceId = resourceId;
      this.confirmSelection();
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      showNotification("Import failed", message);
      if (this.tone3000Tab.tone3000Status) {
        this.tone3000Tab.tone3000Status.textContent = "";
      }
    }
  }

  
  
  private updateSelectButtonState(): void {
    // Tone3000 results are taken with their own per-model select button, so the
    // double click shortcut - and its hint - only apply to the other two tabs.
    if (this.footerHint) {
      this.footerHint.hidden = this.activeTab === "tone3000";
    }

    if (!this.selectBtn) {
      return;
    }

    const hasLibrarySelection = this.activeTab === "library" && Boolean(this.selectedResourceId);
    const hasFolderSelection = this.activeTab === "folder" && Boolean(this.folderTab.selectedFolderPath);
    this.selectBtn.disabled = !(hasLibrarySelection || hasFolderSelection);
    this.selectBtn.textContent = "OK";
  }





























  /// Returns the library list this context navigates through, building it on
  /// demand. Next/prev must work before the browser modal has ever been opened,
  /// so we never rely on the modal having populated the cache first.
  private getOrBuildLibraryNavigationState(
    resourceType: ResourceType,
    options?: NavigationCacheOptions,
  ): ResourceNavigationState | null {
    const cacheKey = this.buildLibraryNavigationCacheKey(resourceType, options?.categoryHint);
    let state = this.libraryNavigationStates.get(cacheKey) ?? null;
    if (!state) {
      state = this.buildLibraryNavigationState(resourceType, options);
      this.libraryNavigationStates.set(cacheKey, state);
    }
    if (state.items.length) {
      return state;
    }

    // A category hint that matches nothing (e.g. a library with no "Reverb" IRs)
    // would otherwise leave the node with no next/prev at all, so widen to the
    // full list for that resource type rather than dead-ending.
    if (!options?.categoryHint) {
      return null;
    }
    const unfilteredKey = this.buildLibraryNavigationCacheKey(resourceType);
    let unfiltered = this.libraryNavigationStates.get(unfilteredKey) ?? null;
    if (!unfiltered) {
      unfiltered = this.buildLibraryNavigationState(resourceType);
      this.libraryNavigationStates.set(unfilteredKey, unfiltered);
    }
    return unfiltered.items.length ? unfiltered : null;
  }

  public getAdjacentResourceSelection(
    resourceType: ResourceType,
    currentResourceId: string,
    currentFilePath: string,
    offset: number,
    options?: NavigationCacheOptions,
  ): ResourceNavigationResult | null {
    // Folder browsing is remembered per effect role, so an IR Cab node keeps
    // stepping through the folder it was last browsed from while a NAM Amp node
    // elsewhere in the graph still steps through its own list.
    const contextKey = options?.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
    const folderState = this.folderTab.folderNavigationStates.get(contextKey) ?? null;
    const usingFolderState = this.lastNavigationViewByContext.get(contextKey) === "folder"
      && folderState?.resourceType === resourceType
      && folderState.items.length > 0;

    const state = usingFolderState
      ? folderState
      : this.getOrBuildLibraryNavigationState(resourceType, options);
    if (!state || state.resourceType !== resourceType || !state.items.length) {
      return null;
    }

    const toResult = (item: ResourceNavigationResult): ResourceNavigationResult => (usingFolderState
      ? { filePath: item.filePath, resourceId: item.resourceId }
      : { resourceId: item.resourceId, filePath: item.filePath });

    const currentKeys = usingFolderState
      ? [currentFilePath, currentResourceId]
      : [
        resolveResourceIdAlias(currentResourceId, this.libraryResourceAliases.get(resourceType) ?? new Map()),
        currentResourceId,
        currentFilePath,
      ];
    const currentIndex = state.items.findIndex((item) => currentKeys.some((key) => (
      key
      && (item.filePath === key || item.resourceId === key)
    )));

    // Nothing loaded yet, or the loaded resource isn't part of this context's
    // list (missing file, filtered out): enter the list from the matching end.
    if (currentIndex < 0) {
      return toResult(offset >= 0 ? state.items[0] : state.items[state.items.length - 1]);
    }

    if (state.items.length < 2) {
      return null;
    }

    // Wrap around so a next/previous is always available.
    const count = state.items.length;
    const nextIndex = (((currentIndex + offset) % count) + count) % count;
    if (nextIndex === currentIndex) {
      return null;
    }

    return toResult(state.items[nextIndex]);
  }








  /// Navigation options describing the list the modal itself is showing.
  private currentModalNavigationOptions(): NavigationCacheOptions {
    return {
      categoryHint: this.options?.libraryCategoryHint ?? this.options?.tone3000CategoryFilter,
      contextKey: this.folderTab.folderContextKey,
    };
  }

  private updateLibraryNavigationButtons(): void {
    if (!this.options) {
      return;
    }

    const navOptions = this.currentModalNavigationOptions();
    const prev = this.getAdjacentResourceSelection(this.options.resourceType, this.selectedResourceId, "", -1, navOptions);
    const next = this.getAdjacentResourceSelection(this.options.resourceType, this.selectedResourceId, "", 1, navOptions);

    if (this.libraryNavPrevBtn) {
      this.libraryNavPrevBtn.disabled = !prev;
      this.libraryNavPrevBtn.setAttribute("aria-disabled", prev ? "false" : "true");
    }
    if (this.libraryNavNextBtn) {
      this.libraryNavNextBtn.disabled = !next;
      this.libraryNavNextBtn.setAttribute("aria-disabled", next ? "false" : "true");
    }
  }

  private navigateLibrarySelection(offset: number): void {
    if (!this.options) {
      return;
    }

    const next = this.getAdjacentResourceSelection(
      this.options.resourceType,
      this.selectedResourceId,
      "",
      offset,
      this.currentModalNavigationOptions(),
    );
    if (!next?.resourceId) {
      return;
    }

    this.selectedResourceId = next.resourceId;
    this.renderLibraryList();
    this.updateSelectButtonState();
    this.previewLibraryResource(next.resourceId);
  }


  private confirmSelection(): void {
    if (!this.options) {
      return;
    }

    if (this.activeTab === "folder") {
      if (!this.folderTab.selectedFolderPath) {
        return;
      }
      this.folderTab.confirmFolderSelection(this.folderTab.selectedFolderPath);
      return;
    }

    if (!this.selectedResourceId) {
      return;
    }
    
    // Commit current preview without restoring the original resource first.
    if (this.previewState?.active) {
      this.cancelPreview(false);
    }
    
    // Mark that we're committing the selection (don't revert on close)
    this.libraryPreviewActive = false;
    this.originalResourceId = this.selectedResourceId;
    
    // Get resource name for notification
    const resourceType = this.options.resourceType;
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const resource = findResourceById(resources, this.selectedResourceId);
    const displayName = resource?.name || this.selectedResourceId;
    
    this.options.onSelect(this.selectedResourceId);
    showNotification("Selected", displayName);
    this.close();
  }
}

// Singleton instance
export const resourceBrowserModal = new ResourceBrowserModal();
