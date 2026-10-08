/**
 * The resource browser's Folder tab: browsing the filesystem for a model or
 * cab file that is not in the library yet.
 *
 * This owns its own DOM, its own listing state, and the virtualised list that
 * renders it — a folder of NAM captures can run to thousands of files, so rows
 * are measured and windowed rather than all built up front.
 *
 * It reaches back into the modal only through `FolderTabHost`. That interface
 * is the whole seam: everything else here is private to the tab.
 */

import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { escapeHtml } from "../utils.js";
import { isDoubleClickExempt } from "./helpers.js";
import { getActiveRoot, getActiveRootId, getFolderLastLocations, getFolderRoots, normalizeFolderPath, rememberFolderLocation, resolveFolderStartLocation, setActiveRootId, setFolderLastLocations, setFolderRoots } from "./folderRoots.js";
import type { FolderRowState } from "./folderRows.js";
import { folderFileLibraryMatch, getFolderFileTags, renderFolderTagFilters } from "./folderRows.js";
import { FolderVirtualList } from "./folderVirtualList.js";
import { DEFAULT_RESOURCE_CONTEXT_KEY } from "./settings.js";
import type { FolderListing, FolderListingFile, FolderVirtualEntry, ResourceBrowserOptions, ResourceNavigationState, ResourceType } from "./types.js";

/**
 * What the Folder tab needs from the modal around it.
 *
 * The modal builds this as an object of closures rather than passing itself,
 * so nothing in `ResourceBrowserModal` has to stop being private just because
 * the tab moved out.
 */
export interface FolderTabHost {
  /** What the modal was opened for: the node, the resource type, the callback. */
  getOptions(): ResourceBrowserOptions | null;
  /** The modal tracks whether a preview is live so it can revert on cancel. */
  setLibraryPreviewActive(active: boolean): void;
  /** Dismiss the whole modal, after a selection has been made. */
  closeModal(): void;
  isResourceFavorite(resourceId: string): boolean;
  setResourceFavorite(resourceId: string, isFavorite: boolean): void;
  /** Redraw the Library tab, whose rows share the favourites this tab toggles. */
  renderLibraryList(): void;
  /** Open the shared edit popover on a resource that is already in the library. */
  openEditPopover(resourceId: string, resourceType: ResourceType): void;
  /** Open the same popover on a file that has not been imported yet. */
  openEditPopoverForValues(options: {
    resourceId: string;
    resourceType: ResourceType;
    name: string;
    category: string;
    tags: string[];
    provider?: string;
    folderPath?: string;
  }): void;
  resolveDefaultImportCategory(fallbackCategory: string): string;
  updateSelectButtonState(): void;
  /**
   * Record that the node's prev/next controls should keep walking this folder
   * rather than the library or a Tone3000 result set.
   */
  rememberNavigationView(contextKey: string, view: "library" | "folder" | "tone3000"): void;
}

export class FolderTab {
  private readonly virtualList = new FolderVirtualList({
    getScrollElement: () => this.folderList,
    getExpandedPath: () => this.expandedFolderItemPath,
    getRowState: () => this.folderRowState(),
  });

  constructor(private readonly host: FolderTabHost) {}

  /**
   * Look up this tab's DOM and bind it. Called once from the modal's own
   * initialize(), so the tab's markup is wired at the same point as the rest.
   */
  /** The slice of tab state a row builder draws from. */
  private folderRowState(): FolderRowState {
    return {
      wantedResourceType: this.host.getOptions()?.resourceType ?? null,
      previewPath: this.folderPreviewPath,
      selectedPath: this.selectedFolderPath,
      expandedPath: this.expandedFolderItemPath,
      isResourceFavorite: (resourceId) => this.host.isResourceFavorite(resourceId),
    };
  }

  initialize(): void {
    this.folderRootSelect = document.getElementById("resource-browser-folder-root") as HTMLSelectElement | null;
    this.folderAddBtn = document.getElementById("resource-browser-folder-add") as HTMLButtonElement | null;
    this.folderRemoveBtn = document.getElementById("resource-browser-folder-remove") as HTMLButtonElement | null;
    this.folderSearch = document.getElementById("resource-browser-folder-search") as HTMLInputElement | null;
    this.folderUpBtn = document.getElementById("resource-browser-folder-up") as HTMLButtonElement | null;
    this.folderPathLabel = document.getElementById("resource-browser-folder-path");
    this.folderTagFilterBar = document.getElementById("resource-browser-folder-tag-filters");
    this.folderStatus = document.getElementById("resource-browser-folder-status");
    this.folderList = document.getElementById("resource-browser-folder-list");

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
    this.folderList?.addEventListener("dblclick", (event) => this.handleFolderDoubleClick(event));
    this.folderList?.addEventListener("scroll", () => this.virtualList.queueWindowRender(), { passive: true });

    document.addEventListener("resource-browser:folder-listing", this.handleFolderListingEvent as EventListener);
    document.addEventListener("resource-browser:folder-metadata", this.handleFolderMetadataEvent as EventListener);
    document.addEventListener("resource-browser:folder-listing-failed", this.handleFolderListingFailedEvent as EventListener);
    document.addEventListener("resource-browser:folder-picked", this.handleFolderPickedEvent as EventListener);
  }

  folderPreviewActive = false;

  folderPreviewPath: string | null = null;

  pendingFolderSelectPath: string | null = null;

  pendingFolderFavoritePaths = new Set<string>();

  selectedFolderPath: string = "";

  // Folder tab elements
  private folderRootSelect: HTMLSelectElement | null = null;

  private folderAddBtn: HTMLButtonElement | null = null;

  private folderRemoveBtn: HTMLButtonElement | null = null;

  private folderSearch: HTMLInputElement | null = null;

  private folderUpBtn: HTMLButtonElement | null = null;

  private folderPathLabel: HTMLElement | null = null;

  private folderTagFilterBar: HTMLElement | null = null;

  private folderStatus: HTMLElement | null = null;

  folderList: HTMLElement | null = null;

  // Folder tab state
  folderCurrentPath = "";

  folderTagFilters: Set<string> = new Set();

  folderListing: FolderListing | null = null;

  private folderLoading = false;

  private folderRenderQueued = false;

  private expandedFolderItemPath: string | null = null;

  // Folder listings and the library/folder preference are tracked per effect-role
  // context, so browsing a folder for one node type doesn't hijack next/prev on another.
  folderNavigationStates: Map<string, ResourceNavigationState> = new Map();

  folderContextKey: string = DEFAULT_RESOURCE_CONTEXT_KEY;

  folderListingFallbackAttempted = false;

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
    this.virtualList.resetMeasurements();
    this.folderList?.scrollTo({ top: 0 });
    this.folderCurrentPath = listing.path;
    this.folderListingFallbackAttempted = false;
    rememberFolderLocation(this.folderContextKey, listing.path);
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
    const activeRoot = getActiveRoot();
    if (activeRoot
      && !this.folderListingFallbackAttempted
      && normalizeFolderPath(this.folderCurrentPath) !== normalizeFolderPath(activeRoot.path)) {
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

  private openFolderEditPopover(path: string, resourceType: ResourceType): void {
    const file = this.folderListing?.files.find((entry) => entry.path === path);
    if (!file) {
      showNotification("Edit failed", "File no longer exists in this folder.");
      return;
    }

    const match = folderFileLibraryMatch(file);
    if (match.inLibrary && match.id) {
      this.host.openEditPopover(match.id, resourceType);
      return;
    }

    const fileName = file.name ?? path.split(/[\\/]/).pop() ?? path;
    const defaultCategory = this.host.resolveDefaultImportCategory((this.folderListing?.name || "Folder").trim() || "Folder");
    this.host.openEditPopoverForValues({
      resourceId: "",
      resourceType,
      name: this.folderFileDisplayName(path) || fileName.replace(/\.[^.]+$/, ""),
      category: defaultCategory,
      tags: getFolderFileTags(file),
      folderPath: path,
    });
  }

  // ── Folder browser tab ──────────────────────────────────────────

  initFolderTab(): void {
    const { root, path } = resolveFolderStartLocation(this.folderContextKey);
    if (root && root.id !== getActiveRootId()) {
      setActiveRootId(root.id);
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
    const roots = getFolderRoots();
    const activeRoot = getActiveRoot();
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
    const roots = getFolderRoots();
    let existing = roots.find((r) => r.path.replace(/\\/g, "/").toLowerCase() === normalized);
    if (!existing) {
      existing = {
        id: `folder-${Date.now()}-${Math.floor(Math.random() * 1e6)}`,
        label: name || path,
        path,
      };
      roots.push(existing);
      setFolderRoots(roots);
    }
    setActiveRootId(existing.id);
    this.folderCurrentPath = existing.path;
    this.renderFolderRootOptions();
    this.requestFolderListing(existing.path);
  }

  private removeActiveFolderRoot(): void {
    const activeRoot = getActiveRoot();
    if (!activeRoot) return;

    void showConfirm(
      `Remove folder "${activeRoot.label || activeRoot.path}" from saved folders?`,
      "Remove Folder"
    ).then((confirmed) => {
      if (!confirmed) return;
       
      const roots = getFolderRoots().filter((r) => r.id !== activeRoot.id);
      setFolderRoots(roots);

      // Drop any per-effect-type positions that pointed into the removed root.
      const locations = getFolderLastLocations();
      const pruned = Object.fromEntries(
        Object.entries(locations).filter(([, location]) => location.rootId !== activeRoot.id),
      );
      if (Object.keys(pruned).length !== Object.keys(locations).length) {
        setFolderLastLocations(pruned);
      }

      const next = roots[0] ?? null;
      setActiveRootId(next?.id ?? "");
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
    const root = getFolderRoots().find((r) => r.id === id);
    if (!root) return;
    setActiveRootId(root.id);
    this.folderCurrentPath = root.path;
    this.requestFolderListing(root.path);
  }

  private navigateFolderUp(): void {
    const parent = this.folderListing?.parent ?? "";
    if (!parent) return;
    const activeRoot = getActiveRoot();
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
      const activeRoot = getActiveRoot();
      const atRoot = activeRoot
        ? (this.folderListing?.path ?? "").replace(/\\/g, "/").toLowerCase() === activeRoot.path.replace(/\\/g, "/").toLowerCase()
        : true;
      this.folderUpBtn.disabled = !this.folderListing?.parent || atRoot;
    }
  }

  renderFolderList(resetVirtualScroll = false): void {
    if (!this.folderList) return;
    if (this.folderLoading) return;

    const activeRoot = getActiveRoot();
    if (!activeRoot) {
      this.folderList.innerHTML = `<div class="resource-browser-empty">No folder selected. Click \u201CAdd Folder\u201D to browse a folder of NAM/IR/WAV files.</div>`;
      if (this.folderStatus) this.folderStatus.textContent = "";
      renderFolderTagFilters(this.folderTagFilterBar, [], this.folderTagFilters);
      return;
    }

    const listing = this.folderListing;
    if (!listing) {
      this.folderList.innerHTML = `<div class="resource-browser-empty">Select a folder to browse.</div>`;
      renderFolderTagFilters(this.folderTagFilterBar, [], this.folderTagFilters);
      return;
    }

    const query = (this.folderSearch?.value ?? "").trim().toLowerCase();
    const dirs = query
      ? listing.dirs.filter((d) => d.name.toLowerCase().includes(query))
      : listing.dirs;
    const filesByQuery = query
      ? listing.files.filter((f) => f.name.toLowerCase().includes(query))
      : listing.files;
    const availableTags = Array.from(new Set(filesByQuery.flatMap((file) => getFolderFileTags(file))))
      .sort((a, b) => a.localeCompare(b));
    const availableTagSet = new Set(availableTags);
    this.folderTagFilters.forEach((tag) => {
      if (!availableTagSet.has(tag)) {
        this.folderTagFilters.delete(tag);
      }
    });
    renderFolderTagFilters(this.folderTagFilterBar, availableTags, this.folderTagFilters);
    const files = this.folderTagFilters.size
      ? filesByQuery.filter((file) => {
        const tags = getFolderFileTags(file);
        return Array.from(this.folderTagFilters).every((tag) => tags.includes(tag));
      })
      : filesByQuery;
    this.virtualList.setEntries([
      ...dirs.map((dir): FolderVirtualEntry => ({ key: `dir:${dir.path}`, kind: "dir", dir })),
      ...files.map((file): FolderVirtualEntry => ({ key: `file:${file.path}`, kind: "file", file })),
    ]);
    if (resetVirtualScroll && this.folderList) {
      this.folderList.scrollTop = 0;
    }

    this.folderNavigationStates.set(this.folderContextKey, {
      resourceType: this.host.getOptions()?.resourceType ?? "nam",
      items: files
        .filter((file) => file.resourceType === this.host.getOptions()?.resourceType)
        .map((file) => {
          const match = folderFileLibraryMatch(file);
          return {
            resourceId: match.id || file.libraryId || "",
            filePath: file.path,
          };
        }),
    });
    this.host.rememberNavigationView(this.folderContextKey, "folder");
    document.dispatchEvent(new CustomEvent("resource-browser:navigation-cache-updated", {
      detail: {
        resourceType: this.host.getOptions()?.resourceType ?? "nam",
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

    if (!this.virtualList.length) {
      this.folderList.classList.remove("is-virtualized");
      this.folderList.innerHTML = `<div class="resource-browser-empty">No matching items in this folder.</div>`;
      return;
    }
    this.virtualList.renderWindow();
  }

  private handleFolderClick(event: Event): void {
    const options = this.host.getOptions();
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
      if (path && options) {
        const file = this.folderListing?.files.find((entry) => entry.path === path);
        if (file && file.resourceType === options.resourceType) {
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

  /// Double clicking a file takes it, the same as the library tab. Directories
  /// already open on the first click, so their second click belongs to
  /// whatever is now under the pointer in the new listing - leave it alone.
  private handleFolderDoubleClick(event: Event): void {
    const options = this.host.getOptions();
    const target = event.target as HTMLElement | null;
    if (!target || !options) {
      return;
    }
    if (isDoubleClickExempt(target) || target.closest('[data-kind="dir"]')) {
      return;
    }

    const fileEntry = target.closest('[data-kind="file"]') as HTMLElement | null;
    const path = fileEntry?.dataset.path ?? this.selectedFolderPath;
    if (!path) {
      return;
    }

    const file = this.folderListing?.files.find((entry) => entry.path === path);
    if (!file || file.resourceType !== options.resourceType) {
      return;
    }

    this.selectedFolderPath = path;
    this.confirmFolderSelection(path);
  }

  buildFolderImportPayload(
    path: string,
    resourceType: ResourceType,
    overrides?: { name?: string; category?: string; tags?: string[] },
  ): Record<string, unknown> {
    const listing = this.folderListing;
    const file = listing?.files.find((f) => f.path === path);
    const fileName = file?.name ?? path.split(/[\\/]/).pop() ?? path;
    const category = this.host.resolveDefaultImportCategory(listing?.name || "Folder");
    const tags = overrides?.tags ?? getFolderFileTags(file ?? {
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
        sourceFolder: getActiveRoot()?.path ?? listing?.path ?? "",
        sourceFileName: fileName,
        origin: "folder-browser",
      },
    };
  }

  private toggleFolderFavourite(path: string, resourceType: ResourceType): void {
    const file = this.folderListing?.files.find((f) => f.path === path);
    const match = file ? folderFileLibraryMatch(file) : { inLibrary: false, id: "" };
    const currentlyFavorite = Boolean(match.id) && this.host.isResourceFavorite(match.id);

    if (match.inLibrary && match.id) {
      this.host.setResourceFavorite(match.id, !currentlyFavorite);
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
    this.host.renderLibraryList();
    this.renderFolderList();
  }

  folderFileDisplayName(path: string): string {
    const file = this.folderListing?.files.find((f) => f.path === path);
    const fileName = file?.name ?? path.split(/[\\/]/).pop() ?? path;
    const baseName = fileName.replace(/\.[^.]+$/, "");
    return ((file?.metadata?.namName || baseName) ?? baseName).trim() || baseName;
  }

  private previewFolderFile(path: string): void {
    const options = this.host.getOptions();
    if (!options) return;

    this.selectedFolderPath = path;

    // Without a node to audition on, a click only selects.
    if (options.nodeId && (this.folderPreviewPath !== path || !this.folderPreviewActive)) {
      // We now own the node output; clear any library preview tracking.
      this.host.setLibraryPreviewActive(false);
      this.folderPreviewActive = true;
      this.folderPreviewPath = path;

      postMessage({
        type: "updateNodeResource",
        nodeId: options.nodeId,
        resourceType: options.resourceType,
        resourceId: "",
        filePath: path,
        resourceIndex: options.resourceIndex ?? 0,
      });

      showNotification("Previewing", `${this.folderFileDisplayName(path)} - click OK to confirm`);
    }
    this.host.updateSelectButtonState();
    this.applyFolderSelectionHighlight();
  }

  /// The folder tab's equivalent of applyLibrarySelectionHighlight: patch the
  /// rows that changed rather than rebuilding the virtual window, so the row
  /// under the pointer survives to receive its dblclick.
  private applyFolderSelectionHighlight(): void {
    if (!this.folderList) {
      return;
    }

    this.folderList.querySelectorAll<HTMLElement>('[data-kind="file"][data-path]').forEach((entry) => {
      const path = entry.dataset.path ?? "";
      entry.classList.toggle("is-previewing", Boolean(path) && this.folderPreviewPath === path);
      const selectBtn = entry.querySelector(".resource-browser-folder-select-preview") as HTMLElement | null;
      const isSelected = Boolean(path) && this.selectedFolderPath === path;
      selectBtn?.classList.toggle("is-active", isSelected);
      selectBtn?.setAttribute("aria-pressed", isSelected ? "true" : "false");
    });
  }

  confirmFolderSelection(path: string): void {
    const options = this.host.getOptions();
    if (!options) return;

    const file = this.folderListing?.files.find((f) => f.path === path);
    const match = file ? folderFileLibraryMatch(file) : { inLibrary: false, id: "" };

    // Committing: don't revert the node on close.
    this.folderPreviewActive = false;
    this.folderPreviewPath = null;
    this.host.setLibraryPreviewActive(false);

    // If the file is already imported, select it by id as normal.
    if (match.inLibrary && match.id) {
      this.finalizeFolderSelection(match.id, this.folderFileDisplayName(path));
      return;
    }

    // Otherwise import the file into the library (referencing it in place, no
    // copy into app data), then select it once the import completes.
    this.pendingFolderSelectPath = path;
    postMessage(this.buildFolderImportPayload(path, options.resourceType));
    showNotification("Importing", `${this.folderFileDisplayName(path)} - selecting...`);
  }

  finalizeFolderSelection(resourceId: string, displayName: string): void {
    const options = this.host.getOptions();
    if (!options) return;
    this.pendingFolderSelectPath = null;
    this.folderPreviewActive = false;
    this.folderPreviewPath = null;
    this.host.setLibraryPreviewActive(false);
    options.onSelect(resourceId);
    showNotification("Selected", displayName);
    this.host.closeModal();
  }
}
