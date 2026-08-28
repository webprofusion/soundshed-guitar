/**
 * Resource Browser Modal
 * 
 * Enhanced modal for selecting NAM models and IR cabs with:
 * - Resource Library tab (existing library items)
 * - Tone3000 tab (browse and preview remote items)
 * - Preview/temporary loading before import
 */

import { uiState } from "./state.js";
import { postMessage, setAppSetting } from "./bridge.js";
import {
  ensureTone3000Session,
  isTone3000AuthReady,
  isTone3000ByokEnabled,
  tone3000AuthenticatedFetch,
} from "./tone3000.js";
import { showNotification } from "./notifications.js";
import { showConfirm } from "./dialogs.js";
import { arrayBufferToBase64, escapeHtml, findResourceById } from "./utils.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "./featureFlags.js";
import type { AppSettingValue, LibraryResource } from "./types.js";
import { deduplicateResourcesByHashAndPath, resolveResourceIdAlias } from "./resourceDedup.js";
import type { Tone3000Architecture, Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import {
  buildTone3000FavoritesUrl,
  buildTone3000SearchUrl,
  extractTone3000Tones,
  parseTone3000Pagination,
} from "./tone3000Api.js";
import { backfillTone3000ResourceImages, fetchTone3000Models, getTone3000ImageUrl } from "./tone3000Shared.js";
import {
  findAdjacentTone3000Model,
  locateTone3000Position,
  type Tone3000NavigationPosition,
} from "./tone3000Navigation.js";
import { getPlaySvg, getStopSvg } from "./iconAssets.js";

type ResourceBrowserOptions = {
  resourceType: "nam" | "ir";
  currentId?: string;
  nodeId?: string;
  resourceIndex?: number;
  exposedResourceId?: string;
  libraryCategoryHint?: string;
  tone3000CategoryFilter?: "pedal" | "amp" | "full-rig";
  /// Effect-role key ("ir-cab", "ir-reverb", "nam-amp", "nam-fx") used to scope
  /// the remembered folder location and resource navigation to this kind of node.
  contextKey?: string;
  toneGroupId?: string | null;
  toneGroupTitle?: string | null;
  onSelect: (resourceId: string) => void;
  onPreview?: (filePath: string, tempResourceId: string) => void;
  onConfirmImport?: (resourceId: string) => void;
};

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

function normalizeFilterValue(value: string): string {
  return value.trim().replace(/\s+/g, " ").toLowerCase();
}

/**
 * Resolves a Tone3000 category hint to the best-matching library category name.
 * Matching is case-insensitive and keyword-based so it works regardless of how
 * users have named their library categories.
 *
 * Examples: "pedal" matches "Pedal", "FX Pedals", "Pedals"
 *           "full-rig"  matches "Full Rig", "Full-Rig", "Full Rigs"
 *           "amp"       matches "Amp", "Amps", "Amplifiers" (not "Full Rig" entries)
 */
function resolveLibraryCategoryFromHint(hint: string, availableCategories: string[]): string | null {
  const normalizedHint = hint.trim().toLowerCase();
  if (!normalizedHint) {
    return null;
  }

  // 1. Exact case-insensitive match
  const exact = availableCategories.find((c) => c.toLowerCase() === normalizedHint);
  if (exact) return exact;

  // 2. Keyword-based fuzzy match
  for (const cat of availableCategories) {
    const lower = cat.toLowerCase();
    if (normalizedHint === "pedal" && lower.includes("pedal")) return cat;
    if (normalizedHint === "full-rig" && (lower.includes("full-rig") || lower.includes("full rig") || lower.includes("fullrig"))) return cat;
    if (normalizedHint === "amp" && lower.includes("amp") && !lower.includes("full")) return cat;
    if (normalizedHint === "ir" && (lower.includes("ir") || lower.includes("cab"))) return cat;
    if (normalizedHint === "reverb" && lower.includes("reverb")) return cat;
  }

  return null;
}

function splitTagValues(raw: string): string[] {
  return raw
    .split(/[,;/|]/g)
    .map((tag) => tag.trim())
    .filter((tag) => Boolean(tag));
}

function getResourceTags(resource: Pick<LibraryResource, "tags" | "metadata">): string[] {
  const collected = new Set<string>();

  if (Array.isArray(resource.tags)) {
    resource.tags.forEach((tag) => {
      const value = tag.trim();
      if (value) {
        collected.add(value);
      }
    });
  }

  const metadataTags = resource.metadata?.tags;
  if (typeof metadataTags === "string") {
    splitTagValues(metadataTags).forEach((tag) => collected.add(tag));
  }

  return Array.from(collected);
}

function getResourceCreator(resource: Pick<LibraryResource, "metadata">): string {
  const metadata = resource.metadata ?? {};
  return (
    metadata.creatorName
    ?? metadata.authorUsername
    ?? metadata.modeledBy
    ?? metadata.creator
    ?? metadata.provider
    ?? ""
  ).trim();
}

function getResourceLibraryFacets(resources: LibraryResource[]): { tags: string[]; creators: string[] } {
  const tags = new Set<string>();
  const creators = new Set<string>();

  resources.forEach((resource) => {
    getResourceTags(resource).forEach((tag) => tags.add(tag));
    const creator = getResourceCreator(resource);
    if (creator) {
      creators.add(creator);
    }
  });

  return {
    tags: Array.from(tags).sort((a, b) => a.localeCompare(b)),
    creators: Array.from(creators).sort((a, b) => a.localeCompare(b)),
  };
}

const RESOURCE_FAVORITES_SETTING = "resources.favorites";
const FOLDER_ROOTS_SETTING = "resources.folderBrowser.roots";
const FOLDER_ACTIVE_ROOT_SETTING = "resources.folderBrowser.activeRootId";
// Last browsed folder per effect role (e.g. "ir-cab", "nam-amp") so an IR Cab
// picker reopens where IR Cab browsing left off, independent of NAM browsing.
const FOLDER_LAST_LOCATIONS_SETTING = "resources.folderBrowser.lastLocationByContext";
const DEFAULT_RESOURCE_CONTEXT_KEY = "default";
const FOLDER_VIRTUAL_GAP = 6;
const FOLDER_VIRTUAL_OVERSCAN = 6;
const FOLDER_VIRTUAL_ESTIMATED_DIR_HEIGHT = 44;
const FOLDER_VIRTUAL_ESTIMATED_FILE_HEIGHT = 62;

type ResourceType = "nam" | "ir";
type ResourceBrowserTab = "library" | "folder" | "tone3000";

interface FolderRoot {
  id: string;
  label: string;
  path: string;
}

interface FolderLocation {
  rootId: string;
  path: string;
}

interface FolderListingDir {
  name: string;
  path: string;
}

interface FolderListingFile {
  name: string;
  path: string;
  resourceType: ResourceType;
  sizeBytes?: number;
  alreadyInLibrary?: boolean;
  libraryId?: string;
  metadata?: Record<string, string>;
  metadataPending?: boolean;
}

interface FolderListing {
  path: string;
  parent: string;
  name: string;
  dirs: FolderListingDir[];
  files: FolderListingFile[];
  truncated?: boolean;
}

type FolderVirtualEntry =
  | { key: string; kind: "dir"; dir: FolderListingDir }
  | { key: string; kind: "file"; file: FolderListingFile };

interface PersistedResourceBrowserState {
  activeTab: ResourceBrowserTab;
  librarySearch: string;
  libraryCategory: string;
  libraryArchitecture: string;
  libraryCreator: string;
  libraryTagFilters: string[];
  libraryFavoritesOnly?: boolean;
  tone3000Search: string;
  tone3000Category: string;
  tone3000Sort: string;
  tone3000Architecture: string;
  tone3000FavoritesOnly?: boolean;
  tone3000Page: number;
  tone3000TotalPages: number;
  tone3000Tones: Tone3000Tone[];
  expandedToneId: string | null;
  toneModelsCache: Array<[string, Tone3000Model[]]>;
}

interface ResourceImportedDetail {
  id?: string;
  resourceType?: string;
  filePath?: string;
}

export interface ResourceNavigationResult {
  resourceId?: string;
  filePath?: string;
  /// Set for results that are not in the library yet when they are returned
  /// (a Tone3000 model whose import is still landing), so callers can label the
  /// picker without waiting for the library to come back.
  displayName?: string;
}

interface ResourceNavigationState {
  resourceType: ResourceType;
  items: ResourceNavigationResult[];
}

/**
 * The Tone3000 result set a selection was made from, so the node's next/prev
 * controls keep walking that list once the modal is closed. Only the tone list
 * is captured up front — each tone's models are fetched, and the model itself
 * downloaded and imported, when navigation actually reaches it.
 */
interface Tone3000NavigationState {
  resourceType: ResourceType;
  tones: Tone3000Tone[];
  architecture: Tone3000Architecture | null;
  modelsByToneId: Map<string, Tone3000Model[]>;
}

interface NavigationCacheOptions {
  categoryHint?: string;
  contextKey?: string;
}

interface LibraryFilterSnapshot {
  query: string;
  category: string;
  architecture: string;
  creator: string;
  tags: string[];
  favoritesOnly: boolean;
}

export class ResourceBrowserModal {
  private initialized = false;
  private options: ResourceBrowserOptions | null = null;
  private previewState: PreviewState | null = null;
  private originalResourceId: string = ""; // Track original for revert on cancel
  private libraryPreviewActive = false;
  private folderPreviewActive = false;
  private folderPreviewPath: string | null = null;
  private pendingFolderSelectPath: string | null = null;
  private pendingFolderFavoritePaths = new Set<string>();
  private expandedLibraryItemId: string | null = null;
  
  // DOM elements
  private modal: HTMLElement | null = null;
  private title: HTMLElement | null = null;
  private closeBtn: HTMLButtonElement | null = null;
  private cancelBtn: HTMLButtonElement | null = null;
  private selectBtn: HTMLButtonElement | null = null;
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
  private selectedFolderPath: string = "";
  private libraryCreatorFilter = "all";
  private libraryArchitectureFilter = "all";
  private libraryTagFilters: Set<string> = new Set();

  // Folder tab elements
  private folderRootSelect: HTMLSelectElement | null = null;
  private folderAddBtn: HTMLButtonElement | null = null;
  private folderRemoveBtn: HTMLButtonElement | null = null;
  private folderSearch: HTMLInputElement | null = null;
  private folderUpBtn: HTMLButtonElement | null = null;
  private folderPathLabel: HTMLElement | null = null;
  private folderTagFilterBar: HTMLElement | null = null;
  private folderStatus: HTMLElement | null = null;
  private folderList: HTMLElement | null = null;

  // Folder tab state
  private folderCurrentPath = "";
  private folderTagFilters: Set<string> = new Set();
  private folderListing: FolderListing | null = null;
  private folderLoading = false;
  private folderRenderQueued = false;
  private folderVirtualEntries: FolderVirtualEntry[] = [];
  private folderVirtualOffsets: number[] = [];
  private folderVirtualHeights = new Map<string, number>();
  private folderVirtualWindowQueued = false;
  private folderVirtualMeasureQueued = false;
  private expandedFolderItemPath: string | null = null;
  // Per-type cache so preloads for different resource types don't clobber each other.
  private libraryNavigationStates: Map<string, ResourceNavigationState> = new Map();
  private pendingLibraryNavigationRefreshes: Map<string, number> = new Map();
  private libraryResourceAliases: Map<string, Map<string, string>> = new Map(); // Maps resourceType -> (aliasId -> canonicalId)
  // Folder listings and the library/folder preference are tracked per effect-role
  // context, so browsing a folder for one node type doesn't hijack next/prev on another.
  private folderNavigationStates: Map<string, ResourceNavigationState> = new Map();
  private tone3000NavigationStates: Map<string, Tone3000NavigationState> = new Map();
  /// Imports land in the library asynchronously, so remember what navigation has
  /// already pulled down ("<resourceType>:<modelId>" -> resource id) and never
  /// re-download a model while stepping back and forth over a result set.
  private tone3000ImportedResourceIds: Map<string, string> = new Map();
  private lastNavigationViewByContext: Map<string, "library" | "folder" | "tone3000"> = new Map();
  private folderContextKey: string = DEFAULT_RESOURCE_CONTEXT_KEY;
  private folderListingFallbackAttempted = false;
  
  // Tone3000 tab elements
  private tone3000ModeSearchTab: HTMLButtonElement | null = null;
  private tone3000ModeFavoritesTab: HTMLButtonElement | null = null;
  private tone3000SearchControls: HTMLElement | null = null;
  private tone3000Search: HTMLInputElement | null = null;
  private tone3000SearchBtn: HTMLButtonElement | null = null;
  private tone3000Category: HTMLSelectElement | null = null;
  private tone3000Sort: HTMLSelectElement | null = null;
  private tone3000Architecture: HTMLSelectElement | null = null;
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
  private tone3000FavoritesOnly = false;
  private expandedToneId: string | null = null;
  private expandedToneSection: "models" | "details" = "models";
  private toneModelsCache: Map<string, Tone3000Model[]> = new Map();
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
    if (importedId && importedNorm && this.folderListing) {
      const file = this.folderListing.files.find((entry) => entry.path.replace(/\\/g, "/").toLowerCase() === importedNorm);
      if (file) {
        file.alreadyInLibrary = true;
        file.libraryId = importedId;
      }
    }
    if (importedId && importedNorm && this.pendingFolderFavoritePaths.has(importedNorm)) {
      this.setResourceFavorite(importedId, true);
      this.pendingFolderFavoritePaths.delete(importedNorm);
      if (this.folderListing) {
        const file = this.folderListing.files.find((entry) => entry.path.replace(/\\/g, "/").toLowerCase() === importedNorm);
        if (file) {
          file.alreadyInLibrary = true;
          file.libraryId = importedId;
        }
      }
    }

    // Complete a pending folder-tab "Select" once its import lands.
    if (this.pendingFolderSelectPath) {
      const pendingNorm = this.pendingFolderSelectPath.replace(/\\/g, "/").toLowerCase();
      if (importedId && importedNorm === pendingNorm) {
        const displayName = this.folderFileDisplayName(this.pendingFolderSelectPath);
        this.finalizeFolderSelection(importedId, displayName);
        return;
      }
      // Fallback: if the imported path didn't normalize identically, resolve the id
      // from the (now refreshed) library listing for the pending file.
      const pendingFile = this.folderListing?.files.find(
        (f) => f.path.replace(/\\/g, "/").toLowerCase() === pendingNorm,
      );
      if (pendingFile) {
        const resolved = this.folderFileLibraryMatch(pendingFile);
        if (resolved.inLibrary && resolved.id) {
          const displayName = this.folderFileDisplayName(this.pendingFolderSelectPath);
          this.finalizeFolderSelection(resolved.id, displayName);
          return;
        }
      }
    }

    // The folder tab can import resources of any type, so refresh it regardless
    // of the node's current resource type.
    if (this.folderList) {
      this.renderFolderList();
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
    if (this.folderList) {
      this.renderFolderList();
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

  private handleFolderListingEvent = (event: Event): void => {
    const listing = (event as CustomEvent<FolderListing>).detail;
    if (!listing || typeof listing.path !== "string") {
      return;
    }
    this.folderLoading = false;
    this.folderListing = {
      path: listing.path,
      parent: listing.parent ?? "",
      name: listing.name ?? listing.path,
      dirs: Array.isArray(listing.dirs) ? listing.dirs : [],
      files: Array.isArray(listing.files) ? listing.files : [],
      truncated: Boolean(listing.truncated),
    };
    this.folderVirtualHeights.clear();
    this.folderList?.scrollTo({ top: 0 });
    this.folderCurrentPath = listing.path;
    this.folderListingFallbackAttempted = false;
    this.rememberFolderLocation(listing.path);
    this.renderFolderPath();
    this.renderFolderList(true);
  };

  private handleFolderMetadataEvent = (event: Event): void => {
    const detail = (event as CustomEvent<{ path?: string; items?: Array<{ path?: string; metadata?: Record<string, string> }> }>).detail;
    if (!detail || !Array.isArray(detail.items) || detail.items.length === 0) {
      return;
    }
    const listing = this.folderListing;
    if (!listing) {
      return;
    }
    // Only apply if the metadata batch is for the directory currently shown;
    // stale batches from a folder we navigated away from are ignored.
    const norm = (p: string): string => p.replace(/\\/g, "/").toLowerCase();
    if (typeof detail.path === "string" && norm(detail.path) !== norm(listing.path)) {
      return;
    }
    const byPath = new Map<string, FolderListingFile>();
    for (const file of listing.files) {
      byPath.set(norm(file.path), file);
    }
    let changed = false;
    for (const item of detail.items) {
      if (!item || typeof item.path !== "string") continue;
      const file = byPath.get(norm(item.path));
      if (!file) continue;
      file.metadata = item.metadata && typeof item.metadata === "object" ? item.metadata : {};
      file.metadataPending = false;
      changed = true;
    }
    if (changed) {
      this.queueFolderListRender();
    }
  };

  private queueFolderListRender(): void {
    if (this.folderRenderQueued) {
      return;
    }
    this.folderRenderQueued = true;
    requestAnimationFrame(() => {
      this.folderRenderQueued = false;
      this.renderFolderList();
    });
  }

  private handleFolderListingFailedEvent = (event: Event): void => {
    const detail = (event as CustomEvent<{ path?: string; message?: string }>).detail;
    this.folderLoading = false;
    this.folderListing = null;

    // A remembered folder can disappear between sessions (removable drive, moved
    // library). Fall back to the root once rather than stranding the user.
    const activeRoot = this.getActiveRoot();
    if (activeRoot
      && !this.folderListingFallbackAttempted
      && this.normalizeFolderPath(this.folderCurrentPath) !== this.normalizeFolderPath(activeRoot.path)) {
      this.folderListingFallbackAttempted = true;
      this.requestFolderListing(activeRoot.path);
      return;
    }

    if (this.folderStatus) {
      this.folderStatus.textContent = detail?.message ? `Error: ${detail.message}` : "Unable to read folder.";
    }
    if (this.folderList) {
      this.folderList.innerHTML = "";
    }
  };

  private handleFolderPickedEvent = (event: Event): void => {
    const detail = (event as CustomEvent<{ success?: boolean; path?: string; name?: string }>).detail;
    if (!detail?.success || !detail.path) {
      return;
    }
    this.addFolderRoot(detail.path, detail.name ?? detail.path);
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

    // Folder tab elements
    this.folderRootSelect = document.getElementById("resource-browser-folder-root") as HTMLSelectElement | null;
    this.folderAddBtn = document.getElementById("resource-browser-folder-add") as HTMLButtonElement | null;
    this.folderRemoveBtn = document.getElementById("resource-browser-folder-remove") as HTMLButtonElement | null;
    this.folderSearch = document.getElementById("resource-browser-folder-search") as HTMLInputElement | null;
    this.folderUpBtn = document.getElementById("resource-browser-folder-up") as HTMLButtonElement | null;
    this.folderPathLabel = document.getElementById("resource-browser-folder-path");
    this.folderTagFilterBar = document.getElementById("resource-browser-folder-tag-filters");
    this.folderStatus = document.getElementById("resource-browser-folder-status");
    this.folderList = document.getElementById("resource-browser-folder-list");
    
    // Tone3000 tab elements
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

    // Folder tab events
    this.folderAddBtn?.addEventListener("click", () => this.requestAddFolder());
    this.folderRemoveBtn?.addEventListener("click", () => this.removeActiveFolderRoot());
    this.folderRootSelect?.addEventListener("change", () => this.onFolderRootChanged());
    this.folderUpBtn?.addEventListener("click", () => this.navigateFolderUp());
    this.folderSearch?.addEventListener("input", () => this.renderFolderList(true));
    this.folderTagFilterBar?.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      const chip = target?.closest(".preset-tag-filter-chip") as HTMLButtonElement | null;
      if (!chip) {
        return;
      }
      const tag = chip.dataset.tag ?? "";
      if (!tag || tag === "__clear__") {
        this.folderTagFilters.clear();
      } else if (this.folderTagFilters.has(tag)) {
        this.folderTagFilters.delete(tag);
      } else {
        this.folderTagFilters.add(tag);
      }
      this.renderFolderList(true);
    });
    this.folderList?.addEventListener("click", (event) => this.handleFolderClick(event));
    this.folderList?.addEventListener("scroll", () => this.queueFolderVirtualWindowRender(), { passive: true });
    
    // Tone3000 search
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
        this.saveCurrentStateForResourceType();
        void this.runTone3000Search(1);
      }
    });
    this.tone3000ModeFavoritesTab?.addEventListener("click", () => {
      if (!this.tone3000FavoritesOnly) {
        this.tone3000FavoritesOnly = true;
        this.syncTone3000ModeUi();
        this.saveCurrentStateForResourceType();
        void this.runTone3000Search(1);
      }
    });
    this.tone3000Category?.addEventListener("change", () => void this.runTone3000Search());
    this.tone3000Sort?.addEventListener("change", () => void this.runTone3000Search());
    this.tone3000Architecture?.addEventListener("change", () => {
      this.expandedToneId = null;
      this.toneModelsCache.clear();
      this.saveCurrentStateForResourceType();
      void this.runTone3000Search();
    });
    
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

    document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => this.handleFeatureFlagsChanged());
    document.addEventListener("resource-browser:resource-imported", this.handleResourceImportedEvent as EventListener);
    document.addEventListener("resource-browser:resource-removed", this.handleResourceRemovedEvent as EventListener);
    document.addEventListener("resource-browser:usage-info", this.handleUsageInfoEvent as EventListener);
    document.addEventListener("resource-browser:folder-listing", this.handleFolderListingEvent as EventListener);
    document.addEventListener("resource-browser:folder-metadata", this.handleFolderMetadataEvent as EventListener);
    document.addEventListener("resource-browser:folder-listing-failed", this.handleFolderListingFailedEvent as EventListener);
    document.addEventListener("resource-browser:folder-picked", this.handleFolderPickedEvent as EventListener);
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
    persisted.tone3000Search = this.tone3000Search?.value ?? "";
    persisted.tone3000Category = this.tone3000Category?.value ?? (resourceType === "ir" ? "ir" : "amp");
    persisted.tone3000Sort = this.tone3000Sort?.value ?? "popular";
    persisted.tone3000Architecture = this.tone3000Architecture?.value ?? (resourceType === "ir" ? "all" : "2");
    persisted.tone3000FavoritesOnly = this.tone3000FavoritesOnly;
    persisted.tone3000Page = this.tone3000Page;
    persisted.tone3000TotalPages = this.tone3000TotalPages;
    persisted.tone3000Tones = [...this.tone3000Tones];
    persisted.expandedToneId = this.expandedToneId;
    persisted.toneModelsCache = Array.from(this.toneModelsCache.entries());
  }

  private restoreStateForResourceType(resourceType: ResourceType): void {
    const persisted = this.getOrCreatePersistedState(resourceType);

    if (this.librarySearch) {
      this.librarySearch.value = persisted.librarySearch;
      this.librarySearch.placeholder = resourceType === "ir"
        ? "Search IRs..."
        : "Search models...";
    }

    if (this.tone3000Search) {
      this.tone3000Search.value = persisted.tone3000Search;
      this.tone3000Search.placeholder = resourceType === "ir"
        ? "Search Cab IRs..."
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
        ? "Show your Tone3000 favorites"
        : "Favorites require your own Tone3000 API key in Settings";
    }

    if (this.tone3000ModeSearchTab) {
      this.tone3000ModeSearchTab.classList.toggle("active", !this.tone3000FavoritesOnly);
      this.tone3000ModeSearchTab.setAttribute("aria-pressed", !this.tone3000FavoritesOnly ? "true" : "false");
    }

    if (this.tone3000SearchControls) {
      this.tone3000SearchControls.style.display = this.tone3000FavoritesOnly ? "none" : "";
    }
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
    this.folderPreviewActive = false;
    this.folderPreviewPath = null;
    this.selectedFolderPath = "";
    this.folderTagFilters.clear();
    this.closeEditPopover();
    this.pendingFolderSelectPath = null;
    this.pendingFolderFavoritePaths.clear();

    // Each effect role browses from its own remembered folder, so drop the
    // in-session path when the picker is opened for a different kind of node.
    const contextKey = options.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
    if (contextKey !== this.folderContextKey) {
      this.folderContextKey = contextKey;
      this.folderCurrentPath = "";
      this.folderListing = null;
    }
    this.folderListingFallbackAttempted = false;
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

    if (this.tone3000List) {
      if (this.tone3000Tones.length > 0) {
        this.renderTone3000List();
      } else {
        this.tone3000List.innerHTML = this.tone3000FavoritesOnly
          ? `<div class="resource-browser-empty">No favorite tones found. Favorite tones on Tone3000 first, then refresh.</div>`
          : `<div class="resource-browser-empty">Enter a search query to browse Tone3000.</div>`;
      }
    }
    this.updateTone3000Pagination(false);
    if (this.tone3000PageLabel) {
      this.tone3000PageLabel.textContent = this.tone3000TotalPages > 1
        ? `Page ${this.tone3000Page} of ${this.tone3000TotalPages}`
        : `Page ${this.tone3000Page}`;
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
    const needFolderRevert = this.folderPreviewActive;
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
    this.folderPreviewActive = false;
    this.folderPreviewPath = null;
    this.selectedFolderPath = "";
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
      if (this.tone3000Status) {
        this.tone3000Status.textContent = "";
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
    if (resolvedTab === "tone3000" && !this.tone3000Tones.length) {
      void this.runTone3000Search();
    }

    if (resolvedTab === "folder") {
      this.initFolderTab();
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
    if (this.tone3000Category) {
      if (resourceType === "ir") {
        this.tone3000Category.innerHTML = `<option value="ir" selected>Cab IRs</option>`;
        this.tone3000Category.value = "ir";
        this.tone3000Category.disabled = true;
      } else {
        this.tone3000Category.innerHTML = `
          <option value="amp" selected>Amps</option>
          <option value="pedal">Pedals (FX)</option>
          <option value="preamp">Preamps</option>
          <option value="full-rig">Full Rigs</option>
        `;
        this.tone3000Category.value = tone3000CategoryFilter ?? "amp";
        this.tone3000Category.disabled = false;
      }
    }

    if (this.tone3000Architecture) {
      const isIr = resourceType === "ir";
      this.tone3000Architecture.disabled = isIr;
      this.tone3000Architecture.value = isIr ? "all" : "2";
    }
  }

  private getSelectedArchitecture(): Tone3000Architecture | null {
    if (!this.tone3000Architecture || this.options?.resourceType === "ir") {
      return null;
    }
    const selected = this.tone3000Architecture.value;
    if (selected === "1" || selected === "2" || selected === "custom") {
      return selected;
    }
    return null;
  }

  private normalizeArchitectureBadge(raw: string): string {
    const normalized = raw.trim().toLowerCase();
    if (!normalized) {
      return "";
    }
    if (normalized === "2" || normalized === "a2") {
      return "A2";
    }
    if (normalized === "1" || normalized === "a1") {
      return "A1";
    }
    if (normalized === "custom") {
      return "Custom";
    }
    return "";
  }

  private getLibraryResourceArchitecture(resource: LibraryResource): string {
    const metadata = resource.metadata ?? {};
    return this.normalizeNamArchitectureBadge(
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
  private normalizeNamArchitectureBadge(raw: string): string {
    const direct = this.normalizeArchitectureBadge(raw);
    if (direct) {
      return direct;
    }
    const normalized = raw.trim().toLowerCase();
    if (!normalized) {
      return "";
    }
    if (normalized.includes("slimmable")) {
      return "A2";
    }
    if (normalized.includes("wavenet")) {
      return "A1";
    }
    return "";
  }

  private async copyTextToClipboard(value: string): Promise<void> {
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
      await this.copyTextToClipboard(path);
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
    if (!uiState.appSettings) {
      uiState.appSettings = {};
    }
    const raw = uiState.appSettings[RESOURCE_FAVORITES_SETTING];
    let favorites: string[] = Array.isArray(raw) ? (raw.filter((val): val is string => typeof val === "string")) : [];

    const alreadyFavorite = favorites.includes(resourceId);
    if (isFavorite && !alreadyFavorite) {
      favorites.push(resourceId);
    } else if (!isFavorite && alreadyFavorite) {
      favorites = favorites.filter((id) => id !== resourceId);
    }

    uiState.appSettings[RESOURCE_FAVORITES_SETTING] = favorites;
    setAppSetting(RESOURCE_FAVORITES_SETTING, favorites);
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
    this.lastNavigationViewByContext.set(this.folderContextKey, "library");
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
          ? this.normalizeArchitectureBadge(
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
        const favoriteAction = `<button class="resource-browser-action-icon-btn resource-browser-item-fav-toggle${isFav ? " is-active" : ""}" type="button" data-resource-id="${escapeHtml(res.id)}" title="${isFav ? "Remove from favourites" : "Add to favourites"}" aria-label="Toggle favorite">${isFav ? "★" : "☆"}</button>`;
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
    this.folderPreviewActive = false;
    this.folderPreviewPath = null;
    
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

  private openFolderEditPopover(path: string, resourceType: ResourceType): void {
    const file = this.folderListing?.files.find((entry) => entry.path === path);
    if (!file) {
      showNotification("Edit failed", "File no longer exists in this folder.");
      return;
    }

    const match = this.folderFileLibraryMatch(file);
    if (match.inLibrary && match.id) {
      this.openEditPopover(match.id, resourceType);
      return;
    }

    const fileName = file.name ?? path.split(/[\\/]/).pop() ?? path;
    const defaultCategory = this.resolveDefaultImportCategory((this.folderListing?.name || "Folder").trim() || "Folder");
    this.openEditPopoverForValues({
      resourceId: "",
      resourceType,
      name: this.folderFileDisplayName(path) || fileName.replace(/\.[^.]+$/, ""),
      category: defaultCategory,
      tags: this.getFolderFileTags(file),
      folderPath: path,
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
      postMessage(this.buildFolderImportPayload(path, resourceType, {
        name: name || this.folderFileDisplayName(path),
        category: category || (this.folderListing?.name || "Folder"),
        tags,
      }));
      this.closeEditPopover();
      showNotification("Resource saved", name || this.folderFileDisplayName(path));
    }
  }
  
  private async runTone3000Search(page = 1): Promise<void> {
    if (!this.tone3000List || !this.options) {
      return;
    }

    this.syncTone3000ModeUi();
    
    const useFavoritesMode = this.canUseTone3000FavoritesMode() && this.tone3000FavoritesOnly;
    if (this.tone3000FavoritesOnly && !this.canUseTone3000FavoritesMode()) {
      this.tone3000List.innerHTML = this.renderFavoritesPrompt();
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
        
        // Set gear filter based on category
        const categoryValue = this.options?.resourceType === "ir"
          ? "ir"
          : (this.tone3000Category?.value ?? this.options?.tone3000CategoryFilter ?? "amp");
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

        const architecture = this.getSelectedArchitecture();
        if (architecture) {
          params.set("architecture", architecture);
        }
      }
      
      const endpoint = useFavoritesMode ? buildTone3000FavoritesUrl(params) : buildTone3000SearchUrl(params);
      const response = await tone3000AuthenticatedFetch(endpoint);
      
      if (!response.ok) {
        if (useFavoritesMode && (response.status === 401 || response.status === 403 || response.status === 404)) {
          throw new Error("Favorites are only available in BYOK direct API mode.");
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

  private updateTone3000Pagination(loading: boolean): void {
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
  
  private renderTone3000List(): void {
    if (!this.tone3000List) {
      return;
    }
    
    if (!this.tone3000Tones.length) {
      const emptyLabel = this.tone3000FavoritesOnly
        ? "No favorite tones found. Favorite tones on Tone3000 first, then refresh."
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
      <div class="resource-browser-tone-sections" data-tone-id="${String(tone.id)}">
        <div class="resource-browser-tone-section-tabs" role="tablist" aria-label="Tone sections">
          <button
            class="resource-browser-tone-section-tab ${modelsActive ? "is-active" : ""}"
            type="button"
            role="tab"
            aria-selected="${modelsActive ? "true" : "false"}"
            data-tone-id="${String(tone.id)}"
            data-tone-section="models"
          >Models</button>
          <button
            class="resource-browser-tone-section-tab ${!modelsActive ? "is-active" : ""}"
            type="button"
            role="tab"
            aria-selected="${!modelsActive ? "true" : "false"}"
            data-tone-id="${String(tone.id)}"
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
          const previewLabel = isPreviewing ? `${getStopSvg()} Stop` : isLoadingPreview ? "Loading..." : `Preview`;
          
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

    return fetchTone3000Models(tone, this.getSelectedArchitecture() ?? undefined);
  }
  
  private async startPreview(toneId: string, modelId: string, modelUrl: string): Promise<void> {
    if (!this.options) {
      return;
    }
    
    // Keep the currently previewed model active while the next preview downloads.
    if (this.previewState?.active) {
      this.cancelPreview(false);
    }
    
    if (!isTone3000AuthReady()) {
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
    this.renderTone3000List();
    
    if (this.tone3000Status) {
      this.tone3000Status.textContent = "";
    }
  }
  
  private async selectAndImportModel(toneId: string, modelId: string, modelUrl: string, modelName: string): Promise<void> {
    if (!this.options) {
      return;
    }
    
    // Keep current preview active while import is in progress to avoid reverting audio.
    if (this.previewState?.active) {
      this.cancelPreview(false);
    }
    
    if (!isTone3000AuthReady()) {
      showNotification("Import failed", "No Tone3000 session");
      return;
    }
    
    const tone = this.tone3000Tones.find((t) => String(t.id) === toneId);
    if (!tone) {
      showNotification("Import failed", "Tone not found");
      return;
    }

    const modelArchitecture = this.toneModelsCache
      .get(toneId)
      ?.find((model) => String(model.id) === modelId)
      ?.architecture_version;
    
    if (this.tone3000Status) {
      this.tone3000Status.textContent = "Importing...";
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
      const resourceId = await this.importTone3000Resource(
        tone,
        modelId,
        modelName,
        modelArchitecture ?? "",
        buffer,
        isZip,
        resourceType
      );
      
      if (this.tone3000Status) {
        this.tone3000Status.textContent = "";
      }
      
      showNotification("Imported", modelName);

      // Hand the result set to next/prev before closing so the node's controls
      // keep walking the list the user picked from.
      this.captureTone3000NavigationState();

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
    architectureVersion: string,
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
            imageUrl: getTone3000ImageUrl(tone) ?? "",
            architectureVersion: architectureVersion ?? "",
            entryName: entry.name,
            sourceUrl: `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`,
            creatorId: tone.user?.id != null ? String(tone.user.id) : "",
            creatorName: tone.user?.display_name ?? tone.user?.name ?? tone.user?.username ?? "",
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
          imageUrl: getTone3000ImageUrl(tone) ?? "",
          architectureVersion: architectureVersion ?? "",
          sourceUrl: `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`,
          creatorId: tone.user?.id != null ? String(tone.user.id) : "",
          creatorName: tone.user?.display_name ?? tone.user?.name ?? tone.user?.username ?? "",
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

    const hasLibrarySelection = this.activeTab === "library" && Boolean(this.selectedResourceId);
    const hasFolderSelection = this.activeTab === "folder" && Boolean(this.selectedFolderPath);
    this.selectBtn.disabled = !(hasLibrarySelection || hasFolderSelection);
    this.selectBtn.textContent = "OK";
  }

  // ── Folder browser tab ──────────────────────────────────────────

  private getFolderRoots(): FolderRoot[] {
    const raw = uiState.appSettings?.[FOLDER_ROOTS_SETTING];
    if (!Array.isArray(raw)) return [];
    const roots: FolderRoot[] = [];
    for (const item of raw as unknown[]) {
      if (!item || typeof item !== "object") continue;
      const entry = item as { id?: unknown; label?: unknown; path?: unknown };
      if (typeof entry.id !== "string" || typeof entry.path !== "string") continue;
      const label = typeof entry.label === "string" ? entry.label : entry.path;
      roots.push({ id: entry.id, label, path: entry.path });
    }
    return roots;
  }

  private setFolderRoots(roots: FolderRoot[]): void {
    if (!uiState.appSettings) uiState.appSettings = {};
    const serialized = roots as unknown as AppSettingValue;
    uiState.appSettings[FOLDER_ROOTS_SETTING] = serialized;
    setAppSetting(FOLDER_ROOTS_SETTING, serialized);
  }

  private getActiveRootId(): string {
    const raw = uiState.appSettings?.[FOLDER_ACTIVE_ROOT_SETTING];
    return typeof raw === "string" ? raw : "";
  }

  private setActiveRootId(id: string): void {
    if (!uiState.appSettings) uiState.appSettings = {};
    uiState.appSettings[FOLDER_ACTIVE_ROOT_SETTING] = id;
    setAppSetting(FOLDER_ACTIVE_ROOT_SETTING, id);
  }

  private getActiveRoot(): FolderRoot | null {
    const roots = this.getFolderRoots();
    if (!roots.length) return null;
    const activeId = this.getActiveRootId();
    return roots.find((r) => r.id === activeId) ?? roots[0];
  }

  private getFolderLastLocations(): Record<string, FolderLocation> {
    const raw = uiState.appSettings?.[FOLDER_LAST_LOCATIONS_SETTING];
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return {};
    const locations: Record<string, FolderLocation> = {};
    for (const [key, value] of Object.entries(raw as Record<string, unknown>)) {
      if (!value || typeof value !== "object") continue;
      const entry = value as { rootId?: unknown; path?: unknown };
      if (typeof entry.rootId !== "string" || typeof entry.path !== "string") continue;
      locations[key] = { rootId: entry.rootId, path: entry.path };
    }
    return locations;
  }

  private setFolderLastLocations(locations: Record<string, FolderLocation>): void {
    if (!uiState.appSettings) uiState.appSettings = {};
    const serialized = locations as unknown as AppSettingValue;
    uiState.appSettings[FOLDER_LAST_LOCATIONS_SETTING] = serialized;
    setAppSetting(FOLDER_LAST_LOCATIONS_SETTING, serialized);
  }

  /// Records where this effect role was last browsing, so the next IR Cab (or
  /// NAM Amp, etc.) picker reopens there rather than wherever any other node type
  /// happened to leave the shared folder browser.
  private rememberFolderLocation(path: string): void {
    if (!path) return;
    const root = this.getActiveRoot();
    if (!root) return;

    const locations = this.getFolderLastLocations();
    const existing = locations[this.folderContextKey];
    if (existing && existing.rootId === root.id && existing.path === path) {
      return;
    }
    locations[this.folderContextKey] = { rootId: root.id, path };
    this.setFolderLastLocations(locations);
  }

  private normalizeFolderPath(value: string): string {
    return value.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
  }

  private isPathWithinRoot(path: string, rootPath: string): boolean {
    const normalizedPath = this.normalizeFolderPath(path);
    const normalizedRoot = this.normalizeFolderPath(rootPath);
    if (!normalizedPath || !normalizedRoot) return false;
    return normalizedPath === normalizedRoot || normalizedPath.startsWith(`${normalizedRoot}/`);
  }

  /// Where the folder tab should open for the current effect role: the remembered
  /// location if it still lives under a known root, otherwise the active root.
  private resolveFolderStartLocation(): { root: FolderRoot | null; path: string } {
    const roots = this.getFolderRoots();
    if (!roots.length) {
      return { root: null, path: "" };
    }

    const remembered = this.getFolderLastLocations()[this.folderContextKey];
    if (remembered) {
      const rememberedRoot = roots.find((r) => r.id === remembered.rootId);
      if (rememberedRoot) {
        const path = this.isPathWithinRoot(remembered.path, rememberedRoot.path)
          ? remembered.path
          : rememberedRoot.path;
        return { root: rememberedRoot, path };
      }
    }

    const activeRoot = this.getActiveRoot() ?? roots[0];
    return { root: activeRoot, path: activeRoot.path };
  }

  private initFolderTab(): void {
    const { root, path } = this.resolveFolderStartLocation();
    if (root && root.id !== this.getActiveRootId()) {
      this.setActiveRootId(root.id);
    }
    this.renderFolderRootOptions();
    if (!root) {
      this.folderListing = null;
      this.folderCurrentPath = "";
      this.renderFolderPath();
      this.renderFolderList();
      return;
    }
    const target = this.folderCurrentPath && this.folderCurrentPath.length > 0
      ? this.folderCurrentPath
      : path;
    this.requestFolderListing(target);
  }

  private renderFolderRootOptions(): void {
    if (!this.folderRootSelect) return;
    const roots = this.getFolderRoots();
    const activeRoot = this.getActiveRoot();
    if (!roots.length) {
      this.folderRootSelect.innerHTML = `<option value="">No folder selected</option>`;
      this.folderRootSelect.value = "";
    } else {
      this.folderRootSelect.innerHTML = roots
        .map((r) => `<option value="${escapeHtml(r.id)}">${escapeHtml(r.label || r.path)}</option>`)
        .join("");
      this.folderRootSelect.value = activeRoot?.id ?? roots[0].id;
    }
    if (this.folderRemoveBtn) {
      this.folderRemoveBtn.disabled = roots.length === 0;
    }
  }

  private requestAddFolder(): void {
    postMessage({ type: "browseResourceFolder" });
  }

  private addFolderRoot(path: string, name: string): void {
    const normalized = path.replace(/\\/g, "/").toLowerCase();
    const roots = this.getFolderRoots();
    let existing = roots.find((r) => r.path.replace(/\\/g, "/").toLowerCase() === normalized);
    if (!existing) {
      existing = {
        id: `folder-${Date.now()}-${Math.floor(Math.random() * 1e6)}`,
        label: name || path,
        path,
      };
      roots.push(existing);
      this.setFolderRoots(roots);
    }
    this.setActiveRootId(existing.id);
    this.folderCurrentPath = existing.path;
    this.renderFolderRootOptions();
    this.requestFolderListing(existing.path);
  }

  private removeActiveFolderRoot(): void {
    const activeRoot = this.getActiveRoot();
    if (!activeRoot) return;

    void showConfirm(
      `Remove folder "${activeRoot.label || activeRoot.path}" from saved folders?`,
      "Remove Folder"
    ).then((confirmed) => {
      if (!confirmed) return;
       
      const roots = this.getFolderRoots().filter((r) => r.id !== activeRoot.id);
      this.setFolderRoots(roots);

      // Drop any per-effect-type positions that pointed into the removed root.
      const locations = this.getFolderLastLocations();
      const pruned = Object.fromEntries(
        Object.entries(locations).filter(([, location]) => location.rootId !== activeRoot.id),
      );
      if (Object.keys(pruned).length !== Object.keys(locations).length) {
        this.setFolderLastLocations(pruned);
      }

      const next = roots[0] ?? null;
      this.setActiveRootId(next?.id ?? "");
      this.folderCurrentPath = next?.path ?? "";
      this.folderListing = null;
      this.renderFolderRootOptions();
      if (next) {
        this.requestFolderListing(next.path);
      } else {
        this.renderFolderPath();
        this.renderFolderList();
      }
    });
  }

  private onFolderRootChanged(): void {
    const id = this.folderRootSelect?.value ?? "";
    if (!id) return;
    const root = this.getFolderRoots().find((r) => r.id === id);
    if (!root) return;
    this.setActiveRootId(root.id);
    this.folderCurrentPath = root.path;
    this.requestFolderListing(root.path);
  }

  private navigateFolderUp(): void {
    const parent = this.folderListing?.parent ?? "";
    if (!parent) return;
    const activeRoot = this.getActiveRoot();
    if (activeRoot) {
      const rootNorm = activeRoot.path.replace(/\\/g, "/").toLowerCase();
      const currentNorm = (this.folderListing?.path ?? "").replace(/\\/g, "/").toLowerCase();
      if (currentNorm === rootNorm) return;
    }
    this.requestFolderListing(parent);
  }

  private navigateFolderTo(path: string): void {
    this.requestFolderListing(path);
  }

  private requestFolderListing(path: string): void {
    if (!path) return;
    this.folderLoading = true;
    this.folderCurrentPath = path;
    if (this.folderStatus) this.folderStatus.textContent = "Loading…";
    if (this.folderList) {
      this.folderList.innerHTML = `<div class="resource-browser-empty">Loading…</div>`;
    }
    postMessage({ type: "listResourceFolder", path });
  }

  private renderFolderPath(): void {
    if (this.folderPathLabel) {
      this.folderPathLabel.textContent = this.folderListing?.path ?? this.folderCurrentPath ?? "";
    }
    if (this.folderUpBtn) {
      const activeRoot = this.getActiveRoot();
      const atRoot = activeRoot
        ? (this.folderListing?.path ?? "").replace(/\\/g, "/").toLowerCase() === activeRoot.path.replace(/\\/g, "/").toLowerCase()
        : true;
      this.folderUpBtn.disabled = !this.folderListing?.parent || atRoot;
    }
  }

  private folderFileLibraryMatch(file: FolderListingFile): { inLibrary: boolean; id: string } {
    const resources = uiState.resourceLibrary[file.resourceType] ?? [];
    const target = file.path.replace(/\\/g, "/").toLowerCase();
    const match = resources.find((res) => (res.filePath ?? "").replace(/\\/g, "/").toLowerCase() === target);
    if (match) return { inLibrary: true, id: match.id };
    if (file.alreadyInLibrary) return { inLibrary: true, id: file.libraryId ?? "" };
    return { inLibrary: false, id: "" };
  }

  private getFolderFileTags(file: FolderListingFile): string[] {
    const match = this.folderFileLibraryMatch(file);
    if (match.inLibrary && match.id) {
      const resources = uiState.resourceLibrary[file.resourceType] ?? [];
      const resource = findResourceById(resources, match.id);
      if (resource) {
        return getResourceTags(resource);
      }
    }

    const metadata = file.metadata ?? {};
    const tagSources = [metadata.tags, metadata.tag, metadata.toneTags, metadata.categories]
      .filter((value): value is string => typeof value === "string");
    const tags = new Set<string>();
    tagSources.forEach((raw) => {
      splitTagValues(raw).forEach((tag) => tags.add(tag));
    });
    return Array.from(tags);
  }

  private renderFolderTagFilters(availableTags: string[]): void {
    if (!this.folderTagFilterBar) {
      return;
    }

    if (!availableTags.length) {
      this.folderTagFilterBar.innerHTML = "";
      this.folderTagFilterBar.classList.remove("is-active");
      return;
    }

    const selectedTags = Array.from(this.folderTagFilters);
    const clearChip = selectedTags.length
      ? `<button class="preset-tag-filter-chip" type="button" data-tag="__clear__">Clear tags</button>`
      : "";

    const chips = availableTags.map((tag) => {
      const active = this.folderTagFilters.has(tag);
      return `<button class="preset-tag-filter-chip${active ? " is-active" : ""}" type="button" data-tag="${escapeHtml(tag)}">${escapeHtml(tag)}</button>`;
    }).join("");

    this.folderTagFilterBar.innerHTML = clearChip + chips;
    this.folderTagFilterBar.classList.add("is-active");
  }

  private renderFolderList(resetVirtualScroll = false): void {
    if (!this.folderList) return;
    if (this.folderLoading) return;

    const activeRoot = this.getActiveRoot();
    if (!activeRoot) {
      this.folderList.innerHTML = `<div class="resource-browser-empty">No folder selected. Click \u201CAdd Folder\u201D to browse a folder of NAM/IR/WAV files.</div>`;
      if (this.folderStatus) this.folderStatus.textContent = "";
      this.renderFolderTagFilters([]);
      return;
    }

    const listing = this.folderListing;
    if (!listing) {
      this.folderList.innerHTML = `<div class="resource-browser-empty">Select a folder to browse.</div>`;
      this.renderFolderTagFilters([]);
      return;
    }

    const query = (this.folderSearch?.value ?? "").trim().toLowerCase();
    const dirs = query
      ? listing.dirs.filter((d) => d.name.toLowerCase().includes(query))
      : listing.dirs;
    const filesByQuery = query
      ? listing.files.filter((f) => f.name.toLowerCase().includes(query))
      : listing.files;
    const availableTags = Array.from(new Set(filesByQuery.flatMap((file) => this.getFolderFileTags(file))))
      .sort((a, b) => a.localeCompare(b));
    const availableTagSet = new Set(availableTags);
    this.folderTagFilters.forEach((tag) => {
      if (!availableTagSet.has(tag)) {
        this.folderTagFilters.delete(tag);
      }
    });
    this.renderFolderTagFilters(availableTags);
    const files = this.folderTagFilters.size
      ? filesByQuery.filter((file) => {
        const tags = this.getFolderFileTags(file);
        return Array.from(this.folderTagFilters).every((tag) => tags.includes(tag));
      })
      : filesByQuery;
    this.folderVirtualEntries = [
      ...dirs.map((dir): FolderVirtualEntry => ({ key: `dir:${dir.path}`, kind: "dir", dir })),
      ...files.map((file): FolderVirtualEntry => ({ key: `file:${file.path}`, kind: "file", file })),
    ];
    this.rebuildFolderVirtualOffsets();
    if (resetVirtualScroll && this.folderList) {
      this.folderList.scrollTop = 0;
    }

    this.folderNavigationStates.set(this.folderContextKey, {
      resourceType: this.options?.resourceType ?? "nam",
      items: files
        .filter((file) => file.resourceType === this.options?.resourceType)
        .map((file) => {
          const match = this.folderFileLibraryMatch(file);
          return {
            resourceId: match.id || file.libraryId || "",
            filePath: file.path,
          };
        }),
    });
    this.lastNavigationViewByContext.set(this.folderContextKey, "folder");
    document.dispatchEvent(new CustomEvent("resource-browser:navigation-cache-updated", {
      detail: {
        resourceType: this.options?.resourceType ?? "nam",
        view: "folder",
      },
    }));

    if (this.folderStatus) {
      const parts: string[] = [
        `<span class="resource-browser-status-count">${listing.dirs.length} folder${listing.dirs.length === 1 ? "" : "s"}</span>`,
        `<span class="resource-browser-status-count">${files.length} file${files.length === 1 ? "" : "s"}</span>`,
      ];
      if (listing.truncated) parts.push(`<span class="resource-browser-status-note">(truncated)</span>`);
      this.folderStatus.innerHTML = parts.join("");
    }

    if (!this.folderVirtualEntries.length) {
      this.folderList.classList.remove("is-virtualized");
      this.folderList.innerHTML = `<div class="resource-browser-empty">No matching items in this folder.</div>`;
      return;
    }
    this.renderFolderVirtualWindow();
  }

  private getFolderVirtualEntryHeight(entry: FolderVirtualEntry): number {
    const measured = this.folderVirtualHeights.get(entry.key);
    if (measured) return measured;
    if (entry.kind === "dir") return FOLDER_VIRTUAL_ESTIMATED_DIR_HEIGHT;
    return FOLDER_VIRTUAL_ESTIMATED_FILE_HEIGHT + (this.expandedFolderItemPath === entry.file.path ? 140 : 0);
  }

  private rebuildFolderVirtualOffsets(): void {
    this.folderVirtualOffsets = new Array(this.folderVirtualEntries.length);
    let offset = 0;
    this.folderVirtualEntries.forEach((entry, index) => {
      this.folderVirtualOffsets[index] = offset;
      offset += this.getFolderVirtualEntryHeight(entry) + FOLDER_VIRTUAL_GAP;
    });
  }

  private getFolderVirtualTotalHeight(): number {
    if (!this.folderVirtualEntries.length) return 0;
    const lastIndex = this.folderVirtualEntries.length - 1;
    return this.folderVirtualOffsets[lastIndex] + this.getFolderVirtualEntryHeight(this.folderVirtualEntries[lastIndex]);
  }

  private findFolderVirtualIndex(offset: number): number {
    let low = 0;
    let high = this.folderVirtualOffsets.length - 1;
    while (low <= high) {
      const middle = Math.floor((low + high) / 2);
      if (this.folderVirtualOffsets[middle] <= offset) low = middle + 1;
      else high = middle - 1;
    }
    return Math.max(0, high);
  }

  private renderFolderVirtualWindow(): void {
    if (!this.folderList || !this.folderVirtualEntries.length) return;
    const scrollTop = this.folderList.scrollTop;
    const viewportBottom = scrollTop + Math.max(this.folderList.clientHeight, 1);
    const firstVisible = this.findFolderVirtualIndex(scrollTop);
    const lastVisible = this.findFolderVirtualIndex(viewportBottom);
    const start = Math.max(0, firstVisible - FOLDER_VIRTUAL_OVERSCAN);
    const end = Math.min(this.folderVirtualEntries.length, lastVisible + FOLDER_VIRTUAL_OVERSCAN + 1);
    const rows = this.folderVirtualEntries.slice(start, end).map((entry, relativeIndex) => {
      const index = start + relativeIndex;
      return `<div class="resource-browser-virtual-entry" data-virtual-index="${index}" style="transform:translateY(${this.folderVirtualOffsets[index]}px)">${this.renderFolderVirtualEntry(entry)}</div>`;
    }).join("");

    this.folderList.classList.add("is-virtualized");
    this.folderList.innerHTML = `<div class="resource-browser-virtual-spacer" style="height:${this.getFolderVirtualTotalHeight()}px">${rows}</div>`;
    this.queueFolderVirtualMeasurement();
  }

  private renderFolderVirtualEntry(entry: FolderVirtualEntry): string {
    if (entry.kind === "file") return this.renderFolderFileRow(entry.file);
    return `
      <div class="resource-browser-folder-entry" data-kind="dir" data-path="${escapeHtml(entry.dir.path)}">
        <div class="results-item resource-browser-item resource-browser-folder-dir-row">
          <div class="results-item-main resource-browser-item-info">
            <div class="results-item-title resource-browser-item-title">\uD83D\uDCC1 ${escapeHtml(entry.dir.name)}</div>
          </div>
        </div>
      </div>
    `;
  }

  private queueFolderVirtualWindowRender(): void {
    if (this.folderVirtualWindowQueued) return;
    this.folderVirtualWindowQueued = true;
    requestAnimationFrame(() => {
      this.folderVirtualWindowQueued = false;
      this.renderFolderVirtualWindow();
    });
  }

  private queueFolderVirtualMeasurement(): void {
    if (this.folderVirtualMeasureQueued) return;
    this.folderVirtualMeasureQueued = true;
    requestAnimationFrame(() => {
      this.folderVirtualMeasureQueued = false;
      if (!this.folderList) return;
      let layoutChanged = false;
      this.folderList.querySelectorAll<HTMLElement>(".resource-browser-virtual-entry[data-virtual-index]").forEach((element) => {
        const index = Number(element.dataset.virtualIndex);
        const entry = this.folderVirtualEntries[index];
        if (!entry) return;
        const measuredHeight = Math.ceil(element.offsetHeight);
        if (measuredHeight > 0 && this.folderVirtualHeights.get(entry.key) !== measuredHeight) {
          this.folderVirtualHeights.set(entry.key, measuredHeight);
          layoutChanged = true;
        }
      });
      if (layoutChanged) {
        this.rebuildFolderVirtualOffsets();
        this.renderFolderVirtualWindow();
      }
    });
  }

  private renderFolderFileRow(file: FolderListingFile): string {
    const metadata = file.metadata ?? {};
    const typeLabel = file.resourceType === "ir" ? "IR / Cab" : "NAM";
    const match = this.folderFileLibraryMatch(file);
    const badges: string[] = [`<span>${escapeHtml(typeLabel)}</span>`];
    const tags = this.getFolderFileTags(file);

    if (file.resourceType === "nam") {
      const architecture = this.normalizeNamArchitectureBadge(
        metadata.architectureVersion
        || metadata.architecture_version
        || metadata.architecture
        || "",
      );
      if (architecture) badges.push(`<span class="resource-browser-architecture-badge" title="Model architecture">${escapeHtml(architecture)}</span>`);
      const gear = [metadata.gearMake, metadata.gearModel].filter(Boolean).join(" ");
      if (gear) badges.push(`<span class="resource-browser-gear-desc" title="${escapeHtml(gear)}">${escapeHtml(gear)}</span>`);
      const toneType = metadata.toneType ?? "";
      if (toneType) badges.push(`<span class="resource-browser-tone-type">${escapeHtml(toneType.replace(/_/g, " "))}</span>`);
      if (metadata.sampleRate) badges.push(`<span>${escapeHtml(metadata.sampleRate)} Hz</span>`);
    } else {
      if (metadata.sampleRate) badges.push(`<span>${escapeHtml(metadata.sampleRate)} Hz</span>`);
      if (metadata.channels) badges.push(`<span>${escapeHtml(metadata.channels)} ch</span>`);
      if (metadata.durationSec) badges.push(`<span>${escapeHtml(metadata.durationSec)} s</span>`);
    }

    if (file.metadataPending && badges.length <= 1) {
      badges.push(`<span class="resource-browser-meta-pending" title="Reading metadata…">…</span>`);
    }
    if (tags.length) {
      const tagPills = tags
        .map((tag) => `<span class="resource-browser-tag-pill">${escapeHtml(tag)}</span>`)
        .join("");
      badges.push(`<span class="resource-browser-tag-list">${tagPills}</span>`);
    }

    const isFav = Boolean(match.id) && this.isResourceFavorite(match.id);
    const favBtn = `<button class="resource-browser-action-icon-btn resource-browser-item-fav-toggle${isFav ? " is-active" : ""}" type="button" data-path="${escapeHtml(file.path)}" data-resource-type="${escapeHtml(file.resourceType)}" title="${isFav ? "Remove from favourites" : "Add to favourites"}" aria-pressed="${isFav ? "true" : "false"}" aria-label="Toggle favourite">${isFav ? "\u2605" : "\u2606"}</button>`;

    const typeMatches = this.options?.resourceType === file.resourceType;
    const isPreviewing = this.folderPreviewPath === file.path;
    const isSelected = this.selectedFolderPath === file.path;
    const selectPreviewBtn = typeMatches
      ? `<button class="resource-browser-action-icon-btn resource-browser-folder-select-preview${isSelected ? " is-active" : ""}" type="button" data-path="${escapeHtml(file.path)}" title="Select in effect (does not close modal)" aria-label="Select in effect" aria-pressed="${isSelected ? "true" : "false"}">${getPlaySvg()}</button>`
      : "";

    const isExpanded = this.expandedFolderItemPath === file.path;
    const detailsBtn = `<button class="resource-browser-action-icon-btn resource-browser-item-details-btn" type="button" data-path="${escapeHtml(file.path)}" title="${isExpanded ? "Hide details" : "Show details"}" aria-expanded="${isExpanded ? "true" : "false"}" aria-label="File details">\u2139</button>`;
    const editBtn = `<button class="resource-browser-action-icon-btn resource-browser-item-edit-btn" type="button" data-path="${escapeHtml(file.path)}" data-resource-type="${escapeHtml(file.resourceType)}" title="Edit name, category and tags" aria-label="Edit resource"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 1 1 3 3L7 19l-4 1 1-4Z"/></svg></button>`;

    return `
      <div class="resource-browser-folder-entry${isExpanded ? " is-details-expanded" : ""}${isPreviewing ? " is-previewing" : ""}" data-kind="file" data-path="${escapeHtml(file.path)}">
        <div class="results-item resource-browser-item resource-browser-folder-file-row">
          <div class="results-item-main resource-browser-item-info">
            <div class="results-item-title resource-browser-item-title">${escapeHtml(file.name)}</div>
            <div class="results-item-meta resource-browser-item-meta">${badges.join("")}</div>
          </div>
          <div class="resource-browser-item-actions">
            ${favBtn}
            ${editBtn}
            ${detailsBtn}
            ${selectPreviewBtn}
          </div>
        </div>
        ${isExpanded ? this.renderFolderFileDetails(file) : ""}
      </div>
    `;
  }

  private renderFolderFileDetails(file: FolderListingFile): string {
    const metadata = file.metadata ?? {};
    const rows: string[] = [];
    rows.push(`<tr><td class="resource-browser-details-label">File Path</td><td class="resource-browser-details-value resource-browser-details-mono">${escapeHtml(file.path)}</td></tr>`);
    if (typeof file.sizeBytes === "number" && file.sizeBytes > 0) {
      rows.push(`<tr><td class="resource-browser-details-label">Size</td><td class="resource-browser-details-value">${escapeHtml(formatBytes(file.sizeBytes))}</td></tr>`);
    }
    for (const [key, value] of Object.entries(metadata)) {
      if (!value) continue;
      const label = key.replace(/_/g, " ").replace(/([A-Z])/g, " $1").trim();
      rows.push(`<tr><td class="resource-browser-details-label">${escapeHtml(label)}</td><td class="resource-browser-details-value">${escapeHtml(value)}</td></tr>`);
    }
    return `
      <div class="resource-browser-item-details-panel">
        <table class="resource-browser-details-table"><tbody>${rows.join("")}</tbody></table>
      </div>
    `;
  }

  private handleFolderClick(event: Event): void {
    const target = event.target as HTMLElement | null;
    if (!target) return;

    const favBtn = target.closest(".resource-browser-item-fav-toggle") as HTMLButtonElement | null;
    if (favBtn) {
      const path = favBtn.dataset.path ?? "";
      const resourceType = (favBtn.dataset.resourceType ?? "") as ResourceType;
      if (path && (resourceType === "nam" || resourceType === "ir")) {
        this.toggleFolderFavourite(path, resourceType);
      }
      return;
    }

    const previewBtn = target.closest(".resource-browser-folder-select-preview") as HTMLButtonElement | null;
    if (previewBtn) {
      const path = previewBtn.dataset.path ?? "";
      if (path) this.previewFolderFile(path);
      return;
    }

    const detailsBtn = target.closest(".resource-browser-item-details-btn") as HTMLButtonElement | null;
    if (detailsBtn) {
      const path = detailsBtn.dataset.path ?? "";
      if (path) {
        this.expandedFolderItemPath = this.expandedFolderItemPath === path ? null : path;
        this.renderFolderList();
      }
      return;
    }

    const editBtn = target.closest(".resource-browser-item-edit-btn") as HTMLButtonElement | null;
    if (editBtn) {
      const path = editBtn.dataset.path ?? "";
      const resourceType = editBtn.dataset.resourceType as ResourceType;
      if (path && (resourceType === "nam" || resourceType === "ir")) {
        this.openFolderEditPopover(path, resourceType);
      }
      return;
    }

    const fileRow = target.closest(".resource-browser-folder-file-row") as HTMLElement | null;
    if (fileRow) {
      const fileEntry = fileRow.closest('[data-kind="file"]') as HTMLElement | null;
      const path = fileEntry?.dataset.path ?? "";
      if (path && this.options) {
        const file = this.folderListing?.files.find((entry) => entry.path === path);
        if (file && file.resourceType === this.options.resourceType) {
          this.previewFolderFile(path);
        }
      }
      return;
    }

    const dirRow = target.closest('[data-kind="dir"]') as HTMLElement | null;
    if (dirRow) {
      const path = dirRow.dataset.path ?? "";
      if (path) this.navigateFolderTo(path);
    }
  }

  private buildFolderImportPayload(
    path: string,
    resourceType: ResourceType,
    overrides?: { name?: string; category?: string; tags?: string[] },
  ): Record<string, unknown> {
    const listing = this.folderListing;
    const file = listing?.files.find((f) => f.path === path);
    const fileName = file?.name ?? path.split(/[\\/]/).pop() ?? path;
    const category = this.resolveDefaultImportCategory(listing?.name || "Folder");
    const tags = overrides?.tags ?? this.getFolderFileTags(file ?? {
      name: fileName,
      path,
      resourceType,
    });
    return {
      type: "saveLocalLibraryResource",
      resourceType,
      name: (overrides?.name ?? this.folderFileDisplayName(path)).trim(),
      category: (overrides?.category ?? category).trim(),
      tags,
      description: "",
      filePath: path,
      metadata: {
        sourceFolder: this.getActiveRoot()?.path ?? listing?.path ?? "",
        sourceFileName: fileName,
        origin: "folder-browser",
      },
    };
  }

  private toggleFolderFavourite(path: string, resourceType: ResourceType): void {
    const file = this.folderListing?.files.find((f) => f.path === path);
    const match = file ? this.folderFileLibraryMatch(file) : { inLibrary: false, id: "" };
    const currentlyFavorite = Boolean(match.id) && this.isResourceFavorite(match.id);

    if (match.inLibrary && match.id) {
      this.setResourceFavorite(match.id, !currentlyFavorite);
      showNotification(currentlyFavorite ? "Removed from favourites" : "Added to favourites", this.folderFileDisplayName(path));
    } else {
      const pendingNorm = path.replace(/\\/g, "/").toLowerCase();
      this.pendingFolderFavoritePaths.add(pendingNorm);
      postMessage(this.buildFolderImportPayload(path, resourceType));
      showNotification("Adding to favourites", this.folderFileDisplayName(path));
      if (file) {
        file.alreadyInLibrary = true;
      }
    }
    this.renderLibraryList();
    this.renderFolderList();
  }

  private folderFileDisplayName(path: string): string {
    const file = this.folderListing?.files.find((f) => f.path === path);
    const fileName = file?.name ?? path.split(/[\\/]/).pop() ?? path;
    const baseName = fileName.replace(/\.[^.]+$/, "");
    return ((file?.metadata?.namName || baseName) ?? baseName).trim() || baseName;
  }

  private previewFolderFile(path: string): void {
    if (!this.options) return;

    this.selectedFolderPath = path;

    if (this.folderPreviewPath !== path || !this.folderPreviewActive) {
      // We now own the node output; clear any library preview tracking.
      this.libraryPreviewActive = false;
      this.folderPreviewActive = true;
      this.folderPreviewPath = path;

      postMessage({
        type: "updateNodeResource",
        nodeId: this.options.nodeId,
        resourceType: this.options.resourceType,
        resourceId: "",
        filePath: path,
        resourceIndex: this.options.resourceIndex ?? 0,
      });

      showNotification("Previewing", `${this.folderFileDisplayName(path)} - click OK to confirm`);
    }
    this.updateSelectButtonState();
    this.renderFolderList();
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
    const folderState = this.folderNavigationStates.get(contextKey) ?? null;
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

  /// Remembers the Tone3000 result set the user just picked from. Called on a
  /// successful select so navigation only follows a list the user actually chose
  /// from, not merely browsed.
  private captureTone3000NavigationState(): void {
    if (!this.options || !this.tone3000Tones.length) {
      return;
    }

    this.tone3000NavigationStates.set(this.folderContextKey, {
      resourceType: this.options.resourceType,
      tones: [...this.tone3000Tones],
      architecture: this.getSelectedArchitecture(),
      modelsByToneId: new Map(this.toneModelsCache),
    });
    this.lastNavigationViewByContext.set(this.folderContextKey, "tone3000");
    document.dispatchEvent(new CustomEvent("resource-browser:navigation-cache-updated", {
      detail: {
        resourceType: this.options.resourceType,
        view: "tone3000",
      },
    }));
  }

  /// True while next/prev for this context should walk a Tone3000 result set.
  /// Those steps are asynchronous (models are fetched and imported on demand),
  /// so callers must use `stepTone3000Resource` rather than the synchronous
  /// `getAdjacentResourceSelection`.
  public isTone3000NavigationActive(resourceType: ResourceType, options?: NavigationCacheOptions): boolean {
    const contextKey = options?.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
    if (this.lastNavigationViewByContext.get(contextKey) !== "tone3000") {
      return false;
    }

    const state = this.tone3000NavigationStates.get(contextKey);
    return Boolean(state && state.resourceType === resourceType && state.tones.length);
  }

  /**
   * Steps to the neighbouring model in the captured Tone3000 result set,
   * downloading and importing it if it is not in the library yet. Walks tone by
   * tone in result order and wraps around, so next/prev is always available.
   * Returns null when the step cannot be resolved (no session, fetch failure,
   * or a set with nowhere else to go).
   */
  public async stepTone3000Resource(
    resourceType: ResourceType,
    currentResourceId: string,
    offset: number,
    options?: NavigationCacheOptions,
  ): Promise<ResourceNavigationResult | null> {
    if (!this.isTone3000NavigationActive(resourceType, options)) {
      return null;
    }

    const contextKey = options?.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
    const state = this.tone3000NavigationStates.get(contextKey);
    if (!state) {
      return null;
    }

    const direction = offset >= 0 ? 1 : -1;
    const position = this.locateTone3000Position(state, currentResourceId);
    const target = await findAdjacentTone3000Model(
      state.tones,
      position,
      direction,
      (tone) => this.ensureTone3000NavigationModels(state, tone),
    );
    if (!target || String(target.model.id) === position.modelId) {
      return null;
    }

    return this.resolveTone3000ModelResource(state, target.tone, target.model);
  }

  /// The tone/model ids of the loaded resource are recorded in import metadata;
  /// the resource id (`tone3000:<modelId>`) is the fallback for resources
  /// imported before that metadata existed.
  private locateTone3000Position(
    state: Tone3000NavigationState,
    currentResourceId: string,
  ): Tone3000NavigationPosition {
    const resource = findResourceById(uiState.resourceLibrary[state.resourceType] ?? [], currentResourceId);
    const metadata = resource?.metadata ?? {};
    const idParts = currentResourceId.startsWith("tone3000:") ? currentResourceId.split(":") : [];
    const modelId = (metadata.modelId ?? "").trim() || (idParts[1] ?? "");
    const toneId = (metadata.toneId ?? "").trim();

    return locateTone3000Position(state.tones, state.modelsByToneId, toneId, modelId);
  }

  private async ensureTone3000NavigationModels(
    state: Tone3000NavigationState,
    tone: Tone3000Tone,
  ): Promise<Tone3000Model[]> {
    const toneId = String(tone.id);
    const cached = state.modelsByToneId.get(toneId);
    if (cached) {
      return cached;
    }

    if (!isTone3000AuthReady()) {
      state.modelsByToneId.set(toneId, []);
      return [];
    }

    try {
      const models = await fetchTone3000Models(tone, state.architecture ?? undefined);
      state.modelsByToneId.set(toneId, models);
      return models;
    } catch {
      state.modelsByToneId.set(toneId, []);
      return [];
    }
  }

  /// Reuses the already-imported copy when there is one, so stepping back and
  /// forth over a set downloads each model at most once.
  private async resolveTone3000ModelResource(
    state: Tone3000NavigationState,
    tone: Tone3000Tone,
    model: Tone3000Model,
  ): Promise<ResourceNavigationResult | null> {
    const resourceType = state.resourceType;
    const modelId = String(model.id);
    const displayName = model.name?.trim() || tone.title?.trim() || modelId;

    const importedId = this.findImportedTone3000ResourceId(resourceType, modelId);
    if (importedId) {
      return { resourceId: importedId, displayName };
    }

    if (!isTone3000AuthReady()) {
      showNotification("Load failed", "No Tone3000 session");
      return null;
    }

    try {
      const response = await tone3000AuthenticatedFetch(model.model_url);
      if (!response.ok) {
        throw new Error(`Download failed: ${response.status}`);
      }

      const buffer = await response.arrayBuffer();
      const contentType = response.headers.get("content-type") ?? "";
      const isZip = contentType.includes("zip") || model.model_url.toLowerCase().endsWith(".zip");
      const resourceId = await this.importTone3000Resource(
        tone,
        modelId,
        displayName,
        model.architecture_version ?? "",
        buffer,
        isZip,
        resourceType,
      );

      this.tone3000ImportedResourceIds.set(`${resourceType}:${modelId}`, resourceId);
      showNotification("Imported", displayName);
      return { resourceId, displayName };
    } catch (error) {
      showNotification("Load failed", error instanceof Error ? error.message : String(error));
      return null;
    }
  }

  private findImportedTone3000ResourceId(resourceType: ResourceType, modelId: string): string {
    const justImported = this.tone3000ImportedResourceIds.get(`${resourceType}:${modelId}`);
    if (justImported) {
      return justImported;
    }

    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const direct = findResourceById(resources, `tone3000:${modelId}`);
    if (direct) {
      return direct.id;
    }

    const match = resources.find((resource) => resource.metadata?.provider === "tone3000"
      && (resource.metadata?.modelId ?? "") === modelId);
    return match?.id ?? "";
  }

  /// Navigation options describing the list the modal itself is showing.
  private currentModalNavigationOptions(): NavigationCacheOptions {
    return {
      categoryHint: this.options?.libraryCategoryHint ?? this.options?.tone3000CategoryFilter,
      contextKey: this.folderContextKey,
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

  private confirmFolderSelection(path: string): void {
    if (!this.options) return;

    const file = this.folderListing?.files.find((f) => f.path === path);
    const match = file ? this.folderFileLibraryMatch(file) : { inLibrary: false, id: "" };

    // Committing: don't revert the node on close.
    this.folderPreviewActive = false;
    this.folderPreviewPath = null;
    this.libraryPreviewActive = false;

    // If the file is already imported, select it by id as normal.
    if (match.inLibrary && match.id) {
      this.finalizeFolderSelection(match.id, this.folderFileDisplayName(path));
      return;
    }

    // Otherwise import the file into the library (referencing it in place, no
    // copy into app data), then select it once the import completes.
    this.pendingFolderSelectPath = path;
    postMessage(this.buildFolderImportPayload(path, this.options.resourceType));
    showNotification("Importing", `${this.folderFileDisplayName(path)} - selecting...`);
  }

  private finalizeFolderSelection(resourceId: string, displayName: string): void {
    if (!this.options) return;
    this.pendingFolderSelectPath = null;
    this.folderPreviewActive = false;
    this.folderPreviewPath = null;
    this.libraryPreviewActive = false;
    this.options.onSelect(resourceId);
    showNotification("Selected", displayName);
    this.close();
  }

  private confirmSelection(): void {
    if (!this.options) {
      return;
    }

    if (this.activeTab === "folder") {
      if (!this.selectedFolderPath) {
        return;
      }
      this.confirmFolderSelection(this.selectedFolderPath);
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


function sanitizeFilename(raw: string): string {
  const trimmed = raw.trim() || "resource";
  return trimmed.replace(/[^a-z0-9-_.]+/gi, "-");
}

function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  let value = bytes;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

// Type definition for JSZip entries
interface JSZipObject {
  name: string;
  dir: boolean;
  async(type: "arraybuffer"): Promise<ArrayBuffer>;
}
