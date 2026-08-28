import type { BlendDefinition, Preset } from "../types.js";
import type { InstalledPackMetadata } from "../toneSharingPanel.js";
export type PresetArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
  metadata?: Record<string, string>;
};

/**
 * Reference to a tone3000-sourced resource that must be re-downloaded by the
 * recipient using their own API key, as redistribution is prohibited by tone3000 terms.
 */
export type Tone3000ResourceRef = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  modelUrl?: string;
  toneId?: string;
  modelId?: string;
  creatorId?: string;
  creatorName?: string;
};

export type PresetArchiveFolder = {
  name: string;
  presetIds?: string[];
  children?: PresetArchiveFolder[];
};

export type PresetArchive = {
  formatVersion: number;
  preset: Preset;
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
  /** tone3000-sourced resources excluded from the archive per their redistribution terms. */
  tone3000Resources?: Tone3000ResourceRef[];
};

export type PresetCollectionArchive = {
  formatVersion: number;
  createdAt: string;
  presets: Preset[];
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
  presetFolders?: PresetArchiveFolder[];
  /** tone3000-sourced resources excluded from the archive per their redistribution terms. */
  tone3000Resources?: Tone3000ResourceRef[];
};

export type ImportPackSource = "zipImport" | "toneSharingApi" | "generatedPack";

export type ImportPackContext = {
  source: ImportPackSource;
  packId?: string;
  itemId?: string;
  creatorId?: string;
  creatorHandle?: string;
  titleHint?: string;
};

export type ImportPackSummary = {
  format: "generatedPack" | "presetArchive";
  title: string;
  presetCount: number;
  resourceCount: number;
  blendCount: number;
  tone3000ResourceCount: number;
  packId?: string;
};

export type ImportPackWithConfirmationOptions = {
  skipConfirmation?: boolean;
};

export type ArchiveImportContext = {
  source: InstalledPackMetadata["source"];
  packId?: string;
  itemId?: string;
  creatorId?: string;
  creatorHandle?: string;
  titleHint?: string;
};

export type ArchiveImportOptions = {
  previewOnly?: boolean;
  suppressNotifications?: boolean;
  topLevelFolderName?: string;
};

export interface GeneratorPackManifest {
  packId?: string;
  packVersion?: string;
  formatVersion?: number;
}

export interface GeneratorResourceIndex {
  items: GeneratorResourceIndexItem[];
}

export interface GeneratorPresetV2 {
  id: string;
  name: string;
  category?: string;
  description?: string;
  tags?: string[];
  global?: { inputTrim?: number; outputTrim?: number };
  graph: {
    nodes: GeneratorPackNode[];
    edges: Array<{ from: string; to: string }>;
  };
}

export interface GeneratorResourceIndexItem {
  resourceId: string;
  resourceType: "nam" | "ir";
  provider: string;
  contentHash: string;
  fileExt: string;
  filePath: string;
  displayName: string;
  originalFileName: string;
}

export interface GeneratorPackNode {
  id: string;
  type: string;
  params?: Record<string, number>;
  resource?: { resourceType: string; resourceId: string };
}
