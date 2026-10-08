/**
 * The shapes the resource browser works in: what it was opened for, what the
 * library and the filesystem hand back, and what it remembers per resource type.
 */

import type { Tone3000Architecture, Tone3000Model, Tone3000Tone } from "../tone3000ApiTypes.js";

export type ResourceBrowserOptions = {
  resourceType: "nam" | "ir";
  currentId?: string;
  /// The node a pick is auditioned on. Without one the browser only picks:
  /// nothing previews through the chain, and File… (which loads straight onto
  /// a node) is hidden.
  nodeId?: string;
  /// Replaces "Select IR Cabinet" / "Select Amp Model".
  title?: string;
  /// Replaces the footer's hint about previewing in the chain.
  hint?: string;
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

export interface PreviewState {
  active: boolean;
  toneId: string;
  modelId: string;
  tempFilePath: string;
  tempResourceId: string;
}

export interface PreviewLoadingState {
  toneId: string;
  modelId: string;
}

export type ResourceType = "nam" | "ir";

export type ResourceBrowserTab = "library" | "folder" | "tone3000";

export interface FolderRoot {
  id: string;
  label: string;
  path: string;
}

export interface FolderLocation {
  rootId: string;
  path: string;
}

export interface FolderListingDir {
  name: string;
  path: string;
}

export interface FolderListingFile {
  name: string;
  path: string;
  resourceType: ResourceType;
  sizeBytes?: number;
  alreadyInLibrary?: boolean;
  libraryId?: string;
  metadata?: Record<string, string>;
  metadataPending?: boolean;
}

export interface FolderListing {
  path: string;
  parent: string;
  name: string;
  dirs: FolderListingDir[];
  files: FolderListingFile[];
  truncated?: boolean;
}

export type FolderVirtualEntry =
  | { key: string; kind: "dir"; dir: FolderListingDir }
  | { key: string; kind: "file"; file: FolderListingFile };

export interface PersistedResourceBrowserState {
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

export interface ResourceImportedDetail {
  id?: string;
  resourceType?: string;
  filePath?: string;
  requestId?: string;
}

export interface ResourceNavigationResult {
  resourceId?: string;
  filePath?: string;
  /// Set for results that are not in the library yet when they are returned
  /// (a Tone3000 model whose import is still landing), so callers can label the
  /// picker without waiting for the library to come back.
  displayName?: string;
}

export interface ResourceNavigationState {
  resourceType: ResourceType;
  items: ResourceNavigationResult[];
}

/**
 * The Tone3000 result set a selection was made from, so the node's next/prev
 * controls keep walking that list once the modal is closed. Only the tone list
 * is captured up front — each tone's models are fetched, and the model itself
 * downloaded and imported, when navigation actually reaches it.
 */
export interface Tone3000NavigationState {
  resourceType: ResourceType;
  tones: Tone3000Tone[];
  architecture: Tone3000Architecture | null;
  modelsByToneId: Map<string, Tone3000Model[]>;
  /// What a model imported while stepping is filed under: the slot of the picker the set
  /// came from, for IRs.
  importCategory?: string;
}

export interface NavigationCacheOptions {
  categoryHint?: string;
  contextKey?: string;
}

export interface LibraryFilterSnapshot {
  query: string;
  category: string;
  architecture: string;
  creator: string;
  tags: string[];
  favoritesOnly: boolean;
}

// Type definition for JSZip entries
export interface JSZipObject {
  name: string;
  dir: boolean;
  async(type: "arraybuffer"): Promise<ArrayBuffer>;
}
