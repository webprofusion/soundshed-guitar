import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { clonePreset, uiState } from "../state.js";
import { arrayBufferToBase64, sha256HexFromBase64 } from "../utils.js";
import { buildArchiveFileNameWithHash, generateResourceId, requestResourceData, sanitizeFilename } from "../archiveUtils.js";
import type { BlendDefinition, Preset, ResourceRef, SignalGraph, ToneSharingOriginMetadata } from "../types.js";
import { migratePresetNodeTypes } from "../presetV2.js";
import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { registerInstalledToneSharingPack } from "../toneSharingPanel.js";
import type { InstalledPackMetadata } from "../toneSharingPanel.js";
import {
  downloadTone3000ResourceByReference,
  isTone3000AuthReady,
  isTone3000ProxyModeEnabled,
  saveTone3000ApiKey,
} from "../tone3000.js";
import { switchMainPanel } from "../navigation.js";
import { activateLibraryTab } from "../settings.js";
import { getLibraryResource, getLibraryResourceByHash } from "../resourceLibrary.js";
import type { ArchiveImportContext, ArchiveImportOptions, GeneratorPackManifest, GeneratorPresetV2, GeneratorResourceIndex, ImportPackContext, ImportPackSource, ImportPackSummary, ImportPackWithConfirmationOptions, PresetArchive, PresetArchiveFolder, PresetArchiveResource, PresetCollectionArchive, Tone3000ResourceRef } from "./archiveTypes.js";
import { PRESET_FOLDER_ALL_ID, applyImportedPresetFolders, assignImportedPresetsToTopLevelFolder, buildArchivePresetFoldersForExport, getPresetFolderExportName, getPresetsForFolderId } from "./folderArchive.js";
import { sanitizePresetForArchive, getPresetArchiveSessionState } from "./sanitize.js";
import { requestPresetFromBackend } from "./fetch.js";
import { requestPresetLibraryRefresh } from "./refresh.js";
import { cachePresetInMemory } from "./cache.js";
export function buildPresetImportSignature(preset: Preset): string {
  const json = JSON.stringify(preset ?? null);
  let hash = 5381;
  for (let i = 0; i < json.length; i += 1) {
    hash = (((hash << 5) + hash) ^ json.charCodeAt(i)) >>> 0;
  }
  return hash.toString(16).padStart(8, "0");
}

export function buildPresetImportSignatureMap(presets: Preset[]): Record<string, string> {
  const entries = presets
    .filter((preset) => typeof preset.id === "string" && preset.id.trim().length > 0)
    .map((preset) => [preset.id, buildPresetImportSignature(preset)] as const);
  return Object.fromEntries(entries);
}

export function buildImportSummaryMessage(summary: ImportPackSummary): string {
  const proxyModeEnabled = isTone3000ProxyModeEnabled();
  const lines = [
    `Import pack "${summary.title}"?`,
    `Format: ${summary.format === "generatedPack" ? "Generated Pack" : "Preset Archive"}`,
    `Presets: ${summary.presetCount}`,
    `Resources: ${summary.resourceCount}`,
    `Blends: ${summary.blendCount}`,
  ];
  if (summary.tone3000ResourceCount > 0) {
    if (proxyModeEnabled) {
      lines.push(`Tone3000 resources: ${summary.tone3000ResourceCount}`);
    } else {
      lines.push(`Tone3000 resources: ${summary.tone3000ResourceCount} (requires your Tone3000 API key)`);
    }
  }
  if (summary.packId) {
    lines.push(`Pack ID: ${summary.packId}`);
  }
  lines.push("This will import resources and presets into your local library.");
  return lines.join("\n");
}

export async function inspectImportPack(file: File, context: ImportPackContext): Promise<ImportPackSummary> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);
  const manifestEntry = zip.file("pack-manifest.json");
  if (manifestEntry) {
    const indexEntry = zip.file("resources/indexes/resources-index.json");

    let manifest: GeneratorPackManifest = {};
    try {
      manifest = JSON.parse(await manifestEntry.async("text")) as GeneratorPackManifest;
    } catch {
      manifest = {};
    }

    let generatedResourceCount = 0;
    if (indexEntry) {
      try {
        const parsed = JSON.parse(await indexEntry.async("text")) as GeneratorResourceIndex;
        generatedResourceCount = Array.isArray(parsed.items) ? parsed.items.length : 0;
      } catch {
        generatedResourceCount = 0;
      }
    }

    const presetCount = Object.keys(zip.files).filter(
      (name) => name.startsWith("presets/") && name.endsWith(".json") && !zip.files[name].dir,
    ).length;

    return {
      format: "generatedPack",
      title: context.titleHint ?? manifest.packId ?? file.name.replace(/\.zip$/i, ""),
      presetCount,
      resourceCount: generatedResourceCount,
      blendCount: 0,
      tone3000ResourceCount: 0,
      packId: context.packId ?? manifest.packId,
    };
  }

  const presetEntry = zip.file("preset.json");
  const presetsEntry = zip.file("presets.json");
  if (!presetEntry && !presetsEntry) {
    throw new Error("Archive is missing preset.json, presets.json, or pack-manifest.json");
  }

  let presetCount = 0;
  let resourceCount = 0;
  let blendCount = 0;
  let tone3000ResourceCount = 0;

  if (presetEntry) {
    const archive = JSON.parse(await presetEntry.async("text")) as PresetArchive;
    presetCount = archive.preset ? 1 : 0;
    resourceCount = Array.isArray(archive.resources) ? archive.resources.length : 0;
    blendCount = Array.isArray(archive.blends) ? archive.blends.length : 0;
    tone3000ResourceCount = Array.isArray(archive.tone3000Resources) ? archive.tone3000Resources.length : 0;
  } else if (presetsEntry) {
    const archive = JSON.parse(await presetsEntry.async("text")) as PresetCollectionArchive;
    presetCount = Array.isArray(archive.presets) ? archive.presets.length : 0;
    resourceCount = Array.isArray(archive.resources) ? archive.resources.length : 0;
    blendCount = Array.isArray(archive.blends) ? archive.blends.length : 0;
    tone3000ResourceCount = Array.isArray(archive.tone3000Resources) ? archive.tone3000Resources.length : 0;
  }

  return {
    format: "presetArchive",
    title: context.titleHint ?? file.name.replace(/\.zip$/i, ""),
    presetCount,
    resourceCount,
    blendCount,
    tone3000ResourceCount,
    packId: context.packId,
  };
}

export function toInstalledPackSource(source: ImportPackSource, format: ImportPackSummary["format"]): InstalledPackMetadata["source"] {
  if (source === "toneSharingApi") {
    return "toneSharingApi";
  }
  if (format === "generatedPack" || source === "generatedPack") {
    return "generatedPack";
  }
  return "zipImport";
}

export async function importPackWithConfirmation(file: File, context: ImportPackContext = { source: "zipImport" }): Promise<void> {
  await importPackWithConfirmationOptions(file, context, {});
}

export async function importPackWithConfirmationOptions(
  file: File,
  context: ImportPackContext,
  options: ImportPackWithConfirmationOptions,
): Promise<void> {
  try {
    const summary = await inspectImportPack(file, context);
    if (!options.skipConfirmation) {
      const confirmed = await showConfirm(buildImportSummaryMessage(summary), "Import Pack");
      if (!confirmed) {
        return;
      }
    }

    const installedSource = toInstalledPackSource(context.source, summary.format);
    if (summary.format === "generatedPack") {
      await importGeneratedPack(file, {
        source: installedSource,
        packId: context.packId,
        titleHint: summary.title,
      });
      return;
    }

    await importPresetArchive(file, {
      source: installedSource,
      packId: context.packId,
      titleHint: summary.title,
    }, {
      topLevelFolderName: summary.title,
    });
  } catch (error) {
    showNotification("Import failed", error instanceof Error ? error.message : String(error));
  }
}

export function findDroppedPresetPackCandidate(files: File[]): File | null {
  return files.find((file) => {
    const lowerName = file.name.trim().toLowerCase();
    return lowerName.endsWith(".soundshed.preset")
      || lowerName.endsWith(".soundshed.presets")
      || lowerName.endsWith(".zip")
      || file.type === "application/zip";
  }) ?? null;
}

export async function isPresetArchiveDropCandidate(file: File): Promise<boolean> {
  const lowerName = file.name.trim().toLowerCase();
  if (lowerName.endsWith(".soundshed.preset") || lowerName.endsWith(".soundshed.presets")) {
    return true;
  }
  if (!lowerName.endsWith(".zip")) {
    return false;
  }

  const zipLib = window.JSZip;
  if (!zipLib) {
    return false;
  }

  try {
    const zip = await zipLib.loadAsync(await file.arrayBuffer());
    if (zip.file("pack-manifest.json")) {
      return false;
    }
    return Boolean(zip.file("preset.json") || zip.file("presets.json"));
  } catch {
    return false;
  }
}

export async function startPresetArchiveSessionFromFile(file: File): Promise<boolean> {
  if (!(await isPresetArchiveDropCandidate(file))) {
    return false;
  }

  showNotification("Loading preset archive session", file.name);
  const data = arrayBufferToBase64(await file.arrayBuffer());
  postMessage({
    type: "startPresetArchiveSession",
    fileName: file.name,
    data,
  });
  return true;
}

export async function handleDroppedPresetPack(files: File[]): Promise<boolean> {
  const file = findDroppedPresetPackCandidate(files);
  if (!file) {
    return false;
  }

  let summary: ImportPackSummary;
  try {
    summary = await inspectImportPack(file, { source: "zipImport" });
  } catch {
    return false;
  }

  const canUseArchiveSession = summary.format === "presetArchive" && await isPresetArchiveDropCandidate(file);
  if (!canUseArchiveSession) {
    const confirmed = await showConfirm(
      `Import pack "${summary.title}" into your local library?\n\nTemporary session-only editing is available for preset archive packs.`,
      "Preset Pack Drop",
    );
    if (confirmed) {
      await importPackWithConfirmationOptions(file, { source: "zipImport" }, { skipConfirmation: true });
    }
    return true;
  }

  const shouldImport = await showConfirm(
    `Import pack "${summary.title}" into your local library?\n\nSelect Cancel to choose temporary session mode instead.`,
    "Preset Pack Drop",
  );
  if (shouldImport) {
    await importPackWithConfirmationOptions(file, { source: "zipImport" }, { skipConfirmation: true });
    return true;
  }

  const useTemporarySession = await showConfirm(
    `Switch to temporary session mode with only presets from "${summary.title}"?\n\nYour normal library stays unchanged and returns after restart.`,
    "Use Temporary Session",
  );
  if (useTemporarySession) {
    await startPresetArchiveSessionFromFile(file);
  }
  return true;
}

export function getPresetGraphs(preset: Preset | null | undefined): SignalGraph[] {
  const graphs: SignalGraph[] = [];
  if (preset?.graph) {
    graphs.push(preset.graph);
  }
  if (Array.isArray(preset?.scenes)) {
    preset.scenes.forEach((scene) => {
      if (scene?.graph) {
        graphs.push(scene.graph);
      }
    });
  }
  return graphs;
}

export function hasAnyGraphNodes(preset: Preset | null | undefined): boolean {
  return getPresetGraphs(preset).some((graph) => Array.isArray(graph.nodes) && graph.nodes.length > 0);
}

export function remapPresetArchiveGraphReferences(
  preset: Preset,
  resourceIdMap: Map<string, string>,
  blendIdMap: Map<string, string>,
): void {
  getPresetGraphs(preset).forEach((graph) => {
    graph.nodes?.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => {
          const resourceId = res.resourceId ?? res.id;
          if (resourceId) {
            const mapped = resourceIdMap.get(resourceId) ?? resourceId;
            res.resourceId = mapped;
            res.id = mapped;
          }
        });
      }

      if (node.config?.blendId) {
        node.config.blendId = blendIdMap.get(node.config.blendId) ?? node.config.blendId;
      }
    });
  });
}

export function remapPresetResourceReferences(preset: Preset, idMap: Map<string, string>): void {
  getPresetGraphs(preset).forEach((graph) => {
    graph.nodes?.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => {
          const resourceId = res.resourceId ?? res.id;
          if (resourceId) {
            const mapped = idMap.get(resourceId) ?? idMap.get(resourceId.split("__").pop() ?? "");
            if (mapped) {
              res.resourceId = mapped;
              res.id = mapped;
            }
          }
        });
      }
    });
  });

  if (preset.audioFxModelId) {
    const mapped = idMap.get(preset.audioFxModelId) ?? idMap.get(preset.audioFxModelId.split("__").pop() ?? "");
    if (mapped) {
      preset.audioFxModelId = mapped;
    }
  }
  if (preset.irId) {
    const mapped = idMap.get(preset.irId) ?? idMap.get(preset.irId.split("__").pop() ?? "");
    if (mapped) {
      preset.irId = mapped;
    }
  }

  if (Array.isArray(preset.attachments)) {
    preset.attachments = preset.attachments.map((attachment) => {
      if (attachment.id) {
        const mapped = idMap.get(attachment.id) ?? idMap.get(attachment.id.split("__").pop() ?? "");
        if (mapped) {
          return { ...attachment, id: mapped };
        }
      }
      return attachment;
    });
  }
}

export function remapBlendResourceReferences(blends: BlendDefinition[], idMap: Map<string, string>): void {
  blends.forEach((blend) => {
    const remapModel = (id: string) => idMap.get(id) ?? idMap.get(id.split("__").pop() ?? "") ?? id;
    if (Array.isArray(blend.models)) {
      blend.models = blend.models.map(remapModel);
    }
    if (Array.isArray(blend.modelMappings)) {
      blend.modelMappings = blend.modelMappings.map((mapping) => ({
        ...mapping,
        id: remapModel(mapping.id),
      }));
    }
  });
}

export function collectPresetBlendIds(preset: Preset): string[] {
  const ids = new Set<string>();
  getPresetGraphs(preset).forEach((graph) => {
    graph.nodes?.forEach((node) => {
      const blendId = node.config?.blendId ?? "";
      if (blendId) {
        ids.add(blendId);
      }
    });
  });

  return Array.from(ids);
}

export function collectPresetResourceRefs(preset: Preset, blendDefs: BlendDefinition[]): ResourceRef[] {
  const refs: ResourceRef[] = [];
  const seen = new Set<string>();

  const addRef = (type: string | undefined, id: string | undefined, filePath?: string): void => {
    if (!type || !id) {
      return;
    }
    const key = `${type}:${id}`;
    if (seen.has(key)) {
      return;
    }
    seen.add(key);
    refs.push({ type, id, filePath });
  };

  getPresetGraphs(preset).forEach((graph) => {
    graph.nodes?.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => addRef(res.resourceType ?? res.type, res.resourceId ?? res.id, res.filePath));
      }
    });
  });

  if (preset.audioFxModelId) {
    addRef("nam", preset.audioFxModelId);
  }
  if (preset.irId) {
    addRef("ir", preset.irId);
  }

  preset.attachments?.forEach((attachment) => {
    if (!attachment.id) {
      return;
    }
    const type = attachment.type === "audiofx" ? "nam" : attachment.type === "ir" ? "ir" : attachment.type;
    addRef(type, attachment.id, attachment.filePath);
  });

  blendDefs.forEach((blend) => {
    (blend.models ?? []).forEach((modelId) => addRef("nam", modelId));
    (blend.modelMappings ?? []).forEach((mapping) => addRef("nam", mapping.id));
  });

  return refs;
}

export async function exportPresetCollectionArchive(presets: Preset[], archiveName: string, sourceFolderId: string): Promise<void> {
  if (!presets.length) {
    showNotification("Export failed", "No presets to export");
    return;
  }

  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Export failed", "Archive library not available");
    return;
  }

  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    showNotification("Export failed", "Unable to create archive");
    return;
  }

  const exportSourcePresets: Preset[] = [];
  let unresolvedPresetCount = 0;
  for (const listedPreset of presets) {
    const cachedPreset = uiState.presetCache.get(listedPreset.id) ?? listedPreset;
    if (hasAnyGraphNodes(cachedPreset)) {
      exportSourcePresets.push(clonePreset(cachedPreset));
      continue;
    }

    try {
      const fetchedPreset = await requestPresetFromBackend(listedPreset.id);
      exportSourcePresets.push(clonePreset(fetchedPreset));
    } catch {
      // Fall back to whatever is available so metadata/name still exports.
      exportSourcePresets.push(clonePreset(cachedPreset));
      unresolvedPresetCount += 1;
    }
  }

  const blendIds = new Set<string>();
  exportSourcePresets.forEach((preset) => {
    collectPresetBlendIds(preset).forEach((id) => blendIds.add(id));
  });
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.has(blend.id));
  const refMap = new Map<string, ResourceRef>();
  exportSourcePresets.forEach((preset) => {
    collectPresetResourceRefs(preset, blendDefs).forEach((ref) => {
      const resourceType = ref.resourceType ?? ref.type ?? "";
      const resourceId = ref.resourceId ?? ref.id ?? "";
      if (!resourceType || !resourceId) {
        return;
      }
      refMap.set(`${resourceType}:${resourceId}`, ref);
    });
  });

  const exportResources: PresetArchiveResource[] = [];
  const idMap = new Map<string, string>();
  let missingCount = 0;

  for (const ref of refMap.values()) {
    const resourceType = ref.resourceType ?? ref.type ?? "";
    const resourceId = ref.resourceId ?? ref.id ?? "";
    if (!resourceType || !resourceId) {
      continue;
    }
    const resource = getLibraryResource(resourceType, resourceId);
    if (!resource) {
      missingCount += 1;
      continue;
    }
    const data = await requestResourceData(resourceType, resourceId);
    if (!data) {
      missingCount += 1;
      continue;
    }
    const hash = await sha256HexFromBase64(data);
    const fileName = buildArchiveFileNameWithHash(resource, resourceType, hash);
    resourcesFolder.file(fileName, data, { base64: true });

    idMap.set(resourceId, hash);
    if (resourceId.includes("__")) {
      idMap.set(resourceId.split("__").pop()!, hash);
    }

    exportResources.push({
      id: hash,
      name: resource.name,
      category: resource.category,
      type: resourceType,
      fileName,
      hash,
      ...(resource.metadata && Object.keys(resource.metadata).length > 0 ? { metadata: { ...resource.metadata } } : {}),
    });
  }

  const exportPresets = exportSourcePresets.map((preset) => {
    const sanitized = sanitizePresetForArchive(preset);
    remapPresetResourceReferences(sanitized, idMap);
    return sanitized;
  });

  const clonedBlends = JSON.parse(JSON.stringify(blendDefs)) as BlendDefinition[];
  remapBlendResourceReferences(clonedBlends, idMap);

  const presetFolders = buildArchivePresetFoldersForExport(sourceFolderId, exportPresets);
  const archive: PresetCollectionArchive = {
    formatVersion: 1,
    createdAt: new Date().toISOString(),
    presets: exportPresets,
    resources: exportResources,
    blends: clonedBlends,
    ...(presetFolders.length > 0 ? { presetFolders } : {}),
  };

  zip.file("presets.json", JSON.stringify(archive, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);

  if (missingCount > 0) {
    showNotification("Export warning", `${missingCount} resources could not be read`);
  }
  if (unresolvedPresetCount > 0) {
    showNotification("Export warning", `${unresolvedPresetCount} presets could not be fully resolved before export`);
  }

  const normalizeArchiveExportBaseName = (raw: string, fallback: string): string => {
    let normalized = sanitizeFilename(raw, fallback);
    const suffixPattern = /\.(?:soundshed\.(?:preset|presets)|zip)$/i;
    while (suffixPattern.test(normalized)) {
      normalized = normalized.replace(suffixPattern, "");
    }
    normalized = normalized.replace(/\.+$/, "");
    return normalized || fallback;
  };

  postMessage({
    type: "savePresetArchive",
    fileName: `${normalizeArchiveExportBaseName(archiveName, "presets")}.soundshed.presets`,
    data,
  });
}

export async function exportActivePresetFolderArchive(): Promise<void> {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const presets = getPresetsForFolderId(activeFolderId);
  const archiveName = getPresetFolderExportName(activeFolderId);
  await exportPresetCollectionArchive(presets, archiveName, activeFolderId);
}

export async function exportAllPresetsArchive(): Promise<void> {
  const presets = uiState.presets.slice();
  await exportPresetCollectionArchive(presets, "All-Presets", PRESET_FOLDER_ALL_ID);
}

export async function exportPresetArchiveSession(): Promise<void> {
  const session = getPresetArchiveSessionState();
  if (!session) {
    showNotification("No archive session", "Drop a preset archive to start a session.");
    return;
  }
  await exportPresetCollectionArchive(
    uiState.presets.slice(),
    session.archiveName?.replace(/\.[^./\\]+$/g, "") || "Preset-Archive-Session",
    PRESET_FOLDER_ALL_ID,
  );
}

export async function exportSelectedPresetCollectionArchive(): Promise<void> {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  if (activeFolderId === PRESET_FOLDER_ALL_ID) {
    await exportAllPresetsArchive();
    return;
  }
  await exportActivePresetFolderArchive();
}

export async function buildPresetArchiveBlob(preset: Preset): Promise<Blob> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    throw new Error("Unable to create archive");
  }

  const blendIds = collectPresetBlendIds(preset);
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.includes(blend.id));
  const resourceRefs = collectPresetResourceRefs(preset, blendDefs);
  const exportResources: PresetArchiveResource[] = [];
  const idMap = new Map<string, string>();

  for (const ref of resourceRefs) {
    const resourceType = ref.resourceType ?? ref.type ?? "";
    const resourceId = ref.resourceId ?? ref.id ?? "";
    if (!resourceType || !resourceId) continue;
    const resource = getLibraryResource(resourceType, resourceId);
    if (!resource) continue;
    const data = await requestResourceData(resourceType, resourceId);
    if (!data) continue;
    const hash = await sha256HexFromBase64(data);
    const fileName = buildArchiveFileNameWithHash(resource, resourceType, hash);
    resourcesFolder.file(fileName, data, { base64: true });

    idMap.set(resourceId, hash);
    if (resourceId.includes("__")) {
      idMap.set(resourceId.split("__").pop()!, hash);
    }

    exportResources.push({
      id: hash,
      name: resource.name,
      category: resource.category,
      type: resourceType,
      fileName,
      hash,
      ...(resource.metadata && Object.keys(resource.metadata).length > 0 ? { metadata: { ...resource.metadata } } : {}),
    });
  }

  const sanitizedPreset = sanitizePresetForArchive(preset);
  const clonedBlends = JSON.parse(JSON.stringify(blendDefs)) as BlendDefinition[];

  remapPresetResourceReferences(sanitizedPreset, idMap);
  remapBlendResourceReferences(clonedBlends, idMap);

  const archive: PresetArchive = {
    formatVersion: 1,
    preset: sanitizedPreset,
    resources: exportResources,
    blends: clonedBlends,
  };

  zip.file("preset.json", JSON.stringify(archive, null, 2));
  return zip.generateAsync({ type: "blob" });
}

export async function buildToneSharingPresetArchiveBlobs(preset: Preset): Promise<{ publicBlob: Blob; privateBlob: Blob }> {
  const privateBlob = await buildPresetArchiveBlob(preset);
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    throw new Error("Unable to create archive");
  }

  const blendIds = collectPresetBlendIds(preset);
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.includes(blend.id));
  const resourceRefs = collectPresetResourceRefs(preset, blendDefs);
  const exportResources: PresetArchiveResource[] = [];
  const exportTone3000Resources: Tone3000ResourceRef[] = [];
  const idMap = new Map<string, string>();

  for (const ref of resourceRefs) {
    const resourceType = ref.resourceType ?? ref.type ?? "";
    const resourceId = ref.resourceId ?? ref.id ?? "";
    if (!resourceType || !resourceId) {
      continue;
    }

    const resource = getLibraryResource(resourceType, resourceId);
    if (!resource) {
      continue;
    }

    if (resource.metadata?.provider === "tone3000") {
      const toneId = resource.metadata.toneId?.trim() ?? "";
      const modelId = resource.metadata.modelId?.trim() ?? "";
      if (!toneId || !modelId) {
        throw new Error(`Tone3000 resource "${resource.name || resource.id}" is missing toneId/modelId metadata and cannot be shared.`);
      }

      exportTone3000Resources.push({
        id: resource.id,
        name: resource.name,
        category: resource.category,
        type: resourceType,
        toneId,
        modelId,
        creatorId: resource.metadata.creatorId?.trim() || undefined,
        creatorName: resource.metadata.creatorName?.trim() || resource.metadata.authorUsername?.trim() || undefined,
      });
      continue;
    }

    const data = await requestResourceData(resourceType, resourceId);
    if (!data) {
      continue;
    }

    const hash = await sha256HexFromBase64(data);
    const fileName = buildArchiveFileNameWithHash(resource, resourceType, hash);
    resourcesFolder.file(fileName, data, { base64: true });

    idMap.set(resourceId, hash);
    if (resourceId.includes("__")) {
      idMap.set(resourceId.split("__").pop()!, hash);
    }

    exportResources.push({
      id: hash,
      name: resource.name,
      category: resource.category,
      type: resourceType,
      fileName,
      hash,
    });
  }

  const sanitizedPreset = sanitizePresetForArchive(preset);
  const clonedBlends = JSON.parse(JSON.stringify(blendDefs)) as BlendDefinition[];

  remapPresetResourceReferences(sanitizedPreset, idMap);
  remapBlendResourceReferences(clonedBlends, idMap);

  const archive: PresetArchive = {
    formatVersion: 1,
    preset: sanitizedPreset,
    resources: exportResources,
    blends: clonedBlends,
    ...(exportTone3000Resources.length > 0 ? { tone3000Resources: exportTone3000Resources } : {}),
  };

  zip.file("preset.json", JSON.stringify(archive, null, 2));
  return {
    publicBlob: await zip.generateAsync({ type: "blob" }),
    privateBlob,
  };
}

export let tone3000RequirementResolve: ((value: boolean) => void) | null = null;

export function closeTone3000RequirementModal(result: boolean): void {
  const modal = document.getElementById("tone3000-required-modal") as HTMLElement | null;
  const status = document.getElementById("tone3000-required-status") as HTMLElement | null;
  if (modal) {
    modal.style.display = "none";
  }
  if (status) {
    status.textContent = "";
  }
  if (tone3000RequirementResolve) {
    tone3000RequirementResolve(result);
    tone3000RequirementResolve = null;
  }
}

export async function promptForTone3000ApiKey(resourceCount: number): Promise<boolean> {
  const modal = document.getElementById("tone3000-required-modal") as HTMLElement | null;
  const message = document.getElementById("tone3000-required-message") as HTMLElement | null;
  const status = document.getElementById("tone3000-required-status") as HTMLElement | null;
  const input = document.getElementById("tone3000-required-api-key") as HTMLInputElement | null;
  const saveButton = document.getElementById("tone3000-required-save") as HTMLButtonElement | null;
  const settingsButton = document.getElementById("tone3000-required-open-settings") as HTMLButtonElement | null;
  const cancelButton = document.getElementById("tone3000-required-cancel") as HTMLButtonElement | null;
  const closeButton = document.getElementById("tone3000-required-close") as HTMLButtonElement | null;

  if (!modal || !message || !status || !input || !saveButton || !settingsButton || !cancelButton || !closeButton) {
    throw new Error("Tone3000 access modal is not available");
  }

  if (tone3000RequirementResolve) {
    tone3000RequirementResolve(false);
    tone3000RequirementResolve = null;
  }

  message.textContent = `${resourceCount} Tone3000 resource(s) in this shared preset require your own Tone3000 API key before import can continue.`;
  status.textContent = "";
  input.value = "";
  modal.style.display = "flex";

  return new Promise<boolean>((resolve) => {
    tone3000RequirementResolve = resolve;

    saveButton.onclick = async () => {
      const apiKey = input.value.trim();
      if (!apiKey) {
        status.textContent = "Enter your Tone3000 API key to continue.";
        return;
      }
      status.textContent = "Starting Tone3000 session...";
      saveButton.disabled = true;
      try {
        const saved = await saveTone3000ApiKey(apiKey);
        if (!saved) {
          status.textContent = "Tone3000 authentication failed. Check the API key and try again.";
          return;
        }
        closeTone3000RequirementModal(true);
      } finally {
        saveButton.disabled = false;
      }
    };

    settingsButton.onclick = () => {
      switchMainPanel("settings");
      activateLibraryTab("tone3000");
      status.textContent = "Opened Settings › Library › Tone3000.";
    };

    cancelButton.onclick = () => closeTone3000RequirementModal(false);
    closeButton.onclick = () => closeTone3000RequirementModal(false);
    modal.onmousedown = (event) => {
      if (event.target === modal) {
        closeTone3000RequirementModal(false);
      }
    };
  });
}

export async function exportCurrentPresetArchive(): Promise<void> {
  const presetId = uiState.activePresetId ?? "";
  const preset = uiState.presetCache.get(presetId) ?? null;
  if (!preset) {
    showNotification("Export failed", "No preset selected");
    return;
  }

  let blob: Blob;
  try {
    blob = await buildPresetArchiveBlob(preset);
  } catch (error) {
    showNotification("Export failed", (error as Error).message);
    return;
  }

  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  const normalizeArchiveExportBaseName = (raw: string, fallback: string): string => {
    let normalized = sanitizeFilename(raw, fallback);
    const suffixPattern = /\.(?:soundshed\.(?:preset|presets)|zip)$/i;
    while (suffixPattern.test(normalized)) {
      normalized = normalized.replace(suffixPattern, "");
    }
    normalized = normalized.replace(/\.+$/, "");
    return normalized || fallback;
  };

  postMessage({
    type: "savePresetArchive",
    fileName: `${normalizeArchiveExportBaseName(preset.name || preset.id || "preset", "preset")}.soundshed.preset`,
    data,
  });
}

export function resolveImportedPresetName(
  preset: Pick<Preset, "name">,
  context: ArchiveImportContext,
): string {
  const apiTitle = context.titleHint?.trim();
  if (context.source === "toneSharingApi" && context.itemId && apiTitle) {
    return apiTitle;
  }
  return preset.name?.trim() || "Imported Preset";
}

export function normalizeToneSharingOrigin(value: unknown): ToneSharingOriginMetadata | undefined {
  if (!value || typeof value !== "object") {
    return undefined;
  }
  const origin = value as Record<string, unknown>;
  const source = origin.source === "toneSharingApi" ? "toneSharingApi" : null;
  const itemId = typeof origin.itemId === "string" ? origin.itemId.trim() : "";
  if (!source || !itemId) {
    return undefined;
  }
  return {
    source,
    itemId,
    originalPresetId: typeof origin.originalPresetId === "string" ? origin.originalPresetId : undefined,
    importedAt: typeof origin.importedAt === "string" ? origin.importedAt : undefined,
    importedFromPackId: typeof origin.importedFromPackId === "string" ? origin.importedFromPackId : undefined,
    creatorId: typeof origin.creatorId === "string" ? origin.creatorId : undefined,
    creatorHandle: typeof origin.creatorHandle === "string" ? origin.creatorHandle : undefined,
    republishBlocked: origin.republishBlocked !== false,
  };
}

export function createToneSharingOrigin(
  itemId: string,
  sourcePresetId: string,
  options?: { packId?: string; creatorId?: string; creatorHandle?: string },
): ToneSharingOriginMetadata {
  return {
    source: "toneSharingApi",
    itemId,
    originalPresetId: sourcePresetId,
    importedAt: new Date().toISOString(),
    importedFromPackId: options?.packId,
    creatorId: options?.creatorId,
    creatorHandle: options?.creatorHandle,
    republishBlocked: true,
  };
}

export function getToneSharingOriginMetadata(preset: Preset | null | undefined): ToneSharingOriginMetadata | undefined {
  return normalizeToneSharingOrigin(preset?.toneSharingOrigin);
}

export function buildInstalledPackEntryId(file: File, context: ArchiveImportContext): string {
  if (context.source === "toneSharingApi" && context.itemId) {
    return `tone-sharing-api:item:${context.itemId}`;
  }
  if (context.source === "toneSharingApi" && context.packId) {
    return `tone-sharing-api:${context.packId}`;
  }
  if (context.source === "generatedPack" && context.packId) {
    return `generated:${context.packId}`;
  }
  const base = (context.titleHint || file.name).trim().toLowerCase();
  return `${context.source}:${base}:${file.size}`;
}

export function normalizeArchiveResourceKey(value: string): string {
  return value
    .toLowerCase()
    .replace(/^resources\//, "")
    .replace(/[%20]+/g, " ")
    .replace(/[\s_-]+/g, "-")
    .replace(/-+/g, "-")
    .trim();
}

export function resolveArchiveResourceEntry(
  resource: PresetArchiveResource,
  fileMap: Map<string, JSZipObject>,
  normalizedFileMap: Map<string, JSZipObject>,
): JSZipObject | undefined {
  const declaredName = (resource.fileName ?? "").trim();
  if (!declaredName) {
    return undefined;
  }

  const direct = fileMap.get(declaredName) ?? fileMap.get(declaredName.replace(/^resources\//, ""));
  if (direct) {
    return direct;
  }

  const normalized = normalizedFileMap.get(normalizeArchiveResourceKey(declaredName));
  if (normalized) {
    return normalized;
  }

  const hash = (resource.hash ?? "").trim().toLowerCase();
  if (!hash) {
    return undefined;
  }

  const extension = declaredName.includes(".") ? `.${declaredName.split(".").pop()?.toLowerCase() ?? ""}` : "";
  for (const [name, entry] of fileMap.entries()) {
    const lowerName = name.toLowerCase();
    if (!lowerName.includes(hash)) {
      continue;
    }
    if (extension && !lowerName.endsWith(extension)) {
      continue;
    }
    return entry;
  }

  return undefined;
}

export function buildImportResourceFileName(resource: PresetArchiveResource): string {
  const maxFileNameLength = 120;
  const declaredName = (resource.fileName ?? "").trim();
  const dotIndex = declaredName.lastIndexOf(".");
  const declaredExt = dotIndex > 0 && dotIndex < declaredName.length - 1
    ? declaredName.slice(dotIndex).toLowerCase()
    : "";
  const ext = declaredExt || (resource.type === "ir" ? ".wav" : resource.type === "nam" ? ".nam" : ".bin");
  const hashPrefix = sanitizeFilename((resource.hash ?? "").trim().toLowerCase(), "resource");
  const baseSeed = sanitizeFilename(resource.name || resource.id || "resource", "resource");
  const maxBaseLength = Math.max(16, maxFileNameLength - hashPrefix.length - ext.length - 1);
  const trimmedBase = baseSeed.slice(0, maxBaseLength).replace(/[-_.]+$/, "") || "resource";
  return `${hashPrefix}-${trimmedBase}${ext}`;
}

/**
 * Download tone3000 resources referenced in a preset archive using the user's
 * own authenticated session. Redistribution of tone3000 files is prohibited by
 * their terms, so archives carry only a model URL rather than file bytes.
 */
export async function importTone3000ArchiveResources(
  refs: Tone3000ResourceRef[],
  idMap: Map<string, string>,
  importedResources: Array<{ type: string; id: string }>,
): Promise<void> {
  const proxyModeEnabled = isTone3000ProxyModeEnabled();
  if (!proxyModeEnabled && !isTone3000AuthReady()) {
    const storedApiKey = typeof uiState.appSettings["tone3000.apiKey"] === "string"
      ? (uiState.appSettings["tone3000.apiKey"] as string).trim()
      : "";

    if (storedApiKey) {
      appendLog("tone3000 session missing; attempting auto-start from saved API key");
      await saveTone3000ApiKey(storedApiKey);
    }

    if (!isTone3000AuthReady()) {
      const granted = await promptForTone3000ApiKey(refs.length);
      if (!granted) {
        throw new Error("Tone3000 API key is required to import this shared preset");
      }
    }
  }

  let succeeded = 0;
  let failed = 0;

  for (const ref of refs) {
    // If the resource was previously imported, reuse it.
    const existing = getLibraryResource(ref.type, ref.id);
    if (existing && !existing.fileMissing) {
      idMap.set(ref.id, existing.id);
      importedResources.push({ type: ref.type, id: existing.id });
      succeeded += 1;
      continue;
    }

    try {
      const buffer = await downloadTone3000ResourceByReference({
        toneId: ref.toneId,
        modelId: ref.modelId,
        modelUrl: ref.modelUrl,
      });
      const data = arrayBufferToBase64(buffer);
      const extension = ref.type === "ir" ? ".wav" : ".nam";
      const fileName = `${sanitizeFilename(ref.name ?? ref.id, "resource")}${extension}`;

      postMessage({
        type: "importRemoteResource",
        provider: "tone3000",
        resourceType: ref.type,
        resourceId: ref.id,
        name: ref.name ?? ref.id,
        description: "",
        category: ref.category ?? "",
        subfolder: "preset-imports",
        fileName,
        metadata: {
          provider: "tone3000",
          toneId: ref.toneId ?? "",
          creatorId: ref.creatorId ?? "",
          creatorName: ref.creatorName ?? "",
          authorUsername: ref.creatorName ?? "",
          modelId: ref.modelId ?? "",
          ...(ref.modelUrl ? { modelUrl: ref.modelUrl } : {}),
        },
        data,
      });

      idMap.set(ref.id, ref.id);
      importedResources.push({ type: ref.type, id: ref.id });
      succeeded += 1;
      appendLog(`tone3000 archive resource imported: ${ref.name ?? ref.id}`);
    } catch (error) {
      const msg = error instanceof Error ? error.message : String(error);
      appendLog(`tone3000 archive resource failed (${ref.name ?? ref.id}): ${msg}`);
      failed += 1;
    }
  }

  if (failed > 0) {
    throw new Error(`Tone3000 resource download incomplete: ${succeeded} succeeded, ${failed} failed.`);
  }
}

export async function importPresetArchive(
  file: File,
  context: ArchiveImportContext = { source: "zipImport" },
  options: ArchiveImportOptions = {},
): Promise<Preset[]> {
  const previewOnly = Boolean(options.previewOnly);
  const suppressNotifications = Boolean(options.suppressNotifications);
  const notifyImportError = (title: string, message: string): void => {
    if (!suppressNotifications) {
      showNotification(title, message);
    }
  };

  const zipLib = window.JSZip;
  if (!zipLib) {
    notifyImportError("Import failed", "Archive library not available");
    return [];
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);
  const presetEntry = zip.file("preset.json");
  const presetsEntry = zip.file("presets.json");
  if (!presetEntry && !presetsEntry) {
    notifyImportError("Import failed", "Archive is missing preset.json or presets.json");
    return [];
  }

  let resourcesToImport: PresetArchiveResource[] = [];
  let tone3000ResourcesToImport: Tone3000ResourceRef[] = [];
  let blends: BlendDefinition[] = [];
  let presetsToImport: Preset[] = [];
  let presetFoldersToImport: PresetArchiveFolder[] = [];

  if (presetEntry) {
    const presetText = await presetEntry.async("text");
    const archive = JSON.parse(presetText) as PresetArchive;
    if (!archive.preset) {
      notifyImportError("Import failed", "Archive has no preset data");
      return [];
    }
    resourcesToImport = archive.resources ?? [];
    tone3000ResourcesToImport = archive.tone3000Resources ?? [];
    blends = archive.blends ?? [];
    presetsToImport = [archive.preset];
  } else if (presetsEntry) {
    const presetsText = await presetsEntry.async("text");
    const archive = JSON.parse(presetsText) as PresetCollectionArchive;
    if (!Array.isArray(archive.presets) || archive.presets.length === 0) {
      notifyImportError("Import failed", "Archive has no presets data");
      return [];
    }
    resourcesToImport = archive.resources ?? [];
    tone3000ResourcesToImport = archive.tone3000Resources ?? [];
    blends = archive.blends ?? [];
    presetsToImport = archive.presets;
    presetFoldersToImport = archive.presetFolders ?? [];
  }

  const zipFiles = Object.values(zip.files) as JSZipObject[];
  const fileMap = new Map<string, JSZipObject>();
  const normalizedFileMap = new Map<string, JSZipObject>();
  zipFiles.forEach((entry) => {
    if (!entry.dir) {
      const name = entry.name.replace(/^resources\//, "");
      fileMap.set(name, entry);
      normalizedFileMap.set(normalizeArchiveResourceKey(name), entry);
    }
  });

  const idMap = new Map<string, string>();
  const importedResources: Array<{ type: string; id: string }> = [];
  for (const resource of resourcesToImport) {
    const fileName = resource.fileName ?? "";
    const existing = getLibraryResourceByHash(resource.type, resource.hash);
    if (existing) {
      idMap.set(resource.id, existing.id);
      importedResources.push({ type: resource.type, id: existing.id });
      continue;
    }
    const entry = resolveArchiveResourceEntry(resource, fileMap, normalizedFileMap);
    if (!entry) {
      continue;
    }
    const dataBuffer = await entry.async("arraybuffer");
    const data = arrayBufferToBase64(dataBuffer);
    const importFileName = buildImportResourceFileName(resource);
    const newId = generateResourceId(fileName);
    idMap.set(resource.id, newId);
    importedResources.push({ type: resource.type, id: newId });

    postMessage({
      type: "importRemoteResource",
      provider: "presetArchive",
      resourceType: resource.type,
      resourceId: newId,
      name: resource.name ?? fileName,
      description: "",
      category: resource.category ?? "",
      subfolder: "preset-imports",
      fileName: importFileName,
      hash: resource.hash ?? "",
      metadata: {
        ...(resource.metadata ?? {}),
        archiveProvider: "presetArchive",
        sourceFile: fileName,
      },
      data,
    });
  }

  // Download tone3000-sourced resources using the user's own authenticated session.
  if (tone3000ResourcesToImport.length > 0) {
    await importTone3000ArchiveResources(tone3000ResourcesToImport, idMap, importedResources);
  }

  const blendIdMap = new Map<string, string>();
  const presetIdMap = new Map<string, string>();
  blends.forEach((blend) => {
    const newBlendId = generateResourceId(blend.id || blend.name || "blend");
    blendIdMap.set(blend.id, newBlendId);

    const remapModel = (id: string) => idMap.get(id) ?? id;
    const models = (blend.models ?? []).map(remapModel);
    const modelMappings = (blend.modelMappings ?? []).map((mapping) => ({
      ...mapping,
      id: remapModel(mapping.id),
    }));

    postMessage({
      type: "saveBlendDefinition",
      blend: {
        ...blend,
        id: newBlendId,
        models,
        modelMappings,
      },
    });
  });

  const importedPresets: Preset[] = [];
  for (const sourcePreset of presetsToImport) {
    const importedPreset = sanitizePresetForArchive(sourcePreset);
    migratePresetNodeTypes(importedPreset);
    const sourcePresetId = importedPreset.id || importedPreset.name || "preset";
    const archiveOrigin = getToneSharingOriginMetadata(importedPreset);
    importedPreset.id = generateResourceId(sourcePresetId);
    presetIdMap.set(sourcePresetId, importedPreset.id);
    importedPreset.name = resolveImportedPresetName(importedPreset, context) || "Imported Preset";
    const contextOrigin = context.source === "toneSharingApi" && context.itemId
      ? createToneSharingOrigin(context.itemId, sourcePresetId, {
          packId: context.packId,
          creatorId: context.creatorId,
          creatorHandle: context.creatorHandle,
        })
      : undefined;
    const resolvedOrigin = archiveOrigin ?? contextOrigin;
    if (resolvedOrigin) {
      importedPreset.toneSharingOrigin = {
        ...resolvedOrigin,
        originalPresetId: resolvedOrigin.originalPresetId ?? sourcePresetId,
        importedAt: resolvedOrigin.importedAt ?? new Date().toISOString(),
        importedFromPackId: resolvedOrigin.importedFromPackId ?? context.packId,
        creatorId: resolvedOrigin.creatorId ?? context.creatorId,
        creatorHandle: resolvedOrigin.creatorHandle ?? context.creatorHandle,
        republishBlocked: resolvedOrigin.republishBlocked !== false,
      };
    }

    remapPresetArchiveGraphReferences(importedPreset, idMap, blendIdMap);

    if (importedPreset.audioFxModelId) {
      importedPreset.audioFxModelId = idMap.get(importedPreset.audioFxModelId) ?? importedPreset.audioFxModelId;
    }
    if (importedPreset.irId) {
      importedPreset.irId = idMap.get(importedPreset.irId) ?? importedPreset.irId;
    }

    if (Array.isArray(importedPreset.attachments)) {
      importedPreset.attachments = importedPreset.attachments.map((attachment) => ({
        ...attachment,
        id: attachment.id ? (idMap.get(attachment.id) ?? attachment.id) : attachment.id,
      }));
    }

    if (!previewOnly) {
      postMessage({
        type: "savePreset",
        presetId: importedPreset.id,
        name: importedPreset.name,
        category: importedPreset.category,
        description: importedPreset.description,
        includeGlobalSignalChain: false,
        preset: importedPreset,
      });

      cachePresetInMemory(importedPreset);
      uiState.presets = [importedPreset, ...uiState.presets.filter((preset) => preset.id !== importedPreset.id)];
      uiState.presetCache.set(importedPreset.id, importedPreset);
    }
    importedPresets.push(importedPreset);
  }

  if (importedPresets.length === 0) {
    notifyImportError("Import failed", "No presets were imported");
    return [];
  }

  if (previewOnly) {
    return importedPresets;
  }

  const latestPreset = importedPresets[importedPresets.length - 1];
  uiState.activePresetId = latestPreset.id;
  const importedPresetIds = importedPresets.map((preset) => preset.id);
  const topLevelFolderName = options.topLevelFolderName?.trim();
  
  if (presetFoldersToImport.length > 0) {
    // Archive has folder structure: recreate it under a top-level folder
    const folderName = topLevelFolderName || file.name.replace(/\.soundshed\.presets$/i, "").replace(/\.zip$/i, "") || "Imported Pack";
    applyImportedPresetFolders(presetFoldersToImport, presetIdMap, importedPresetIds, folderName);
  } else if (topLevelFolderName) {
    // No folder structure in archive, but top-level folder name was provided
    assignImportedPresetsToTopLevelFolder(topLevelFolderName, importedPresetIds);
  }
  requestPresetLibraryRefresh(latestPreset);
  const uniqueResources = Array.from(new Map(importedResources.map((entry) => [`${entry.type}:${entry.id}`, entry])).values());
  registerInstalledToneSharingPack({
    id: buildInstalledPackEntryId(file, context),
    title: context.titleHint ?? file.name.replace(/\.zip$/i, ""),
    source: context.source,
    importedAt: new Date().toISOString(),
    packId: context.packId,
    archiveFileName: file.name,
    presetIds: importedPresetIds,
    presetSignatures: buildPresetImportSignatureMap(importedPresets),
    resources: uniqueResources,
  });
  showNotification(importedPresets.length === 1 ? "Preset imported" : "Presets imported", importedPresets.length === 1
    ? (latestPreset.name ?? "")
    : `${importedPresets.length} presets imported`);
  requestPresetLibraryRefresh();

  return importedPresets;
}

export async function importGeneratedPack(file: File, context: ArchiveImportContext = { source: "generatedPack" }): Promise<void> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Import failed", "Archive library not available");
    return;
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);

  // Detect generator pack by presence of pack-manifest.json; fall back to preset archive.
  const manifestEntry = zip.file("pack-manifest.json");
  if (!manifestEntry) {
    await importPresetArchive(file, context);
    return;
  }

  let manifest: GeneratorPackManifest;
  try {
    manifest = JSON.parse(await manifestEntry.async("text")) as GeneratorPackManifest;
  } catch {
    showNotification("Import failed", "Invalid pack manifest");
    return;
  }

  const indexEntry = zip.file("resources/indexes/resources-index.json");
  if (!indexEntry) {
    showNotification("Import failed", "Pack is missing resource index");
    return;
  }

  let index: GeneratorResourceIndex;
  try {
    index = JSON.parse(await indexEntry.async("text")) as GeneratorResourceIndex;
  } catch {
    showNotification("Import failed", "Invalid resource index");
    return;
  }

  // Import resource blobs first so they are on disk before presets are saved.
  let resourcesSkipped = 0;
  const importedResources: Array<{ type: string; id: string }> = [];
  for (const item of index.items) {
    const blobEntry = zip.file(item.filePath);
    if (!blobEntry) {
      resourcesSkipped++;
      continue;
    }
    const data = arrayBufferToBase64(await blobEntry.async("arraybuffer"));
    postMessage({
      type: "importRemoteResource",
      provider: item.provider || "generated",
      resourceType: item.resourceType,
      resourceId: item.resourceId,
      name: item.displayName,
      description: "",
      category: "",
      subfolder: `generated/${item.resourceType}`,
      fileName: item.originalFileName,
      hash: item.contentHash,
      data,
    });
    importedResources.push({ type: item.resourceType, id: item.resourceId });
  }

  // Import each preset JSON from the presets/ directory.
  const presetEntryNames = Object.keys(zip.files).filter(
    (name) => name.startsWith("presets/") && name.endsWith(".json") && !zip.files[name].dir
  );

  const importedPresets: Preset[] = [];
  for (const entryName of presetEntryNames) {
    const entry = zip.file(entryName);
    if (!entry) continue;

    let genPreset: GeneratorPresetV2;
    try {
      genPreset = JSON.parse(await entry.async("text")) as GeneratorPresetV2;
    } catch {
      continue;
    }

    const importedId = generateResourceId(genPreset.id || genPreset.name || "preset");
    const importedName = genPreset.name ?? "Generated Preset";

    const appPreset: Preset = {
      id: importedId,
      name: importedName,
      category: genPreset.category ?? "Generated",
      description: genPreset.description ?? "",
      tags: genPreset.tags,
      formatVersion: 2,
      globals: {
        inputTrim: genPreset.global?.inputTrim ?? 0,
        outputTrim: genPreset.global?.outputTrim ?? 0,
        masterVolume: 1,
        autoLevelInput: false,
        autoLevelOutput: false,
      },
      graph: {
        nodes: genPreset.graph.nodes.map((node) => ({
          id: node.id,
          type: node.type,
          displayName: node.type,
          category: "",
          bypassed: false,
          params: (node.params ?? {}) as Record<string, number>,
          config: {},
          resources: node.resource
            ? [{ resourceType: node.resource.resourceType, resourceId: node.resource.resourceId }]
            : undefined,
        })),
        edges: genPreset.graph.edges.map((edge) => ({
          from: edge.from,
          to: edge.to,
          fromPort: 0,
          toPort: 0,
          gain: 1,
        })),
      },
    };

    postMessage({
      type: "savePreset",
      presetId: importedId,
      name: importedName,
      category: appPreset.category,
      description: appPreset.description,
      includeGlobalSignalChain: false,
      preset: appPreset,
    });

    cachePresetInMemory(appPreset);
    uiState.presets = [appPreset, ...uiState.presets.filter((preset) => preset.id !== appPreset.id)];
    uiState.presetCache.set(importedId, appPreset);
    importedPresets.push(appPreset);
  }

  if (!importedPresets.length) {
    showNotification("Import failed", "No presets found in pack");
    return;
  }

  const latestPreset = importedPresets[0];
  uiState.activePresetId = latestPreset.id;
  assignImportedPresetsToTopLevelFolder(
    context.titleHint?.trim() || manifest.packId?.trim() || file.name.replace(/\.zip$/i, ""),
    importedPresets.map((preset) => preset.id),
  );
  requestPresetLibraryRefresh(latestPreset);
  registerInstalledToneSharingPack({
    id: buildInstalledPackEntryId(file, {
      ...context,
      packId: context.packId ?? manifest.packId,
    }),
    title: context.titleHint ?? manifest.packId ?? file.name.replace(/\.zip$/i, ""),
    source: context.source,
    importedAt: new Date().toISOString(),
    packId: context.packId ?? manifest.packId,
    archiveFileName: file.name,
    presetIds: importedPresets.map((preset) => preset.id),
    presetSignatures: buildPresetImportSignatureMap(importedPresets),
    resources: Array.from(new Map(importedResources.map((entry) => [`${entry.type}:${entry.id}`, entry])).values()),
  });
  const packLabel = manifest.packId ?? file.name;
  const suffix = resourcesSkipped > 0 ? ` (${resourcesSkipped} resource file${resourcesSkipped !== 1 ? "s" : ""} missing in pack)` : "";
  showNotification(
    importedPresets.length === 1 ? "Preset imported" : `${importedPresets.length} presets imported`,
    `${packLabel}${suffix}`
  );
  requestPresetLibraryRefresh();
}
