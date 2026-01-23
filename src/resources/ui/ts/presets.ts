import { appendLog } from "./logging.js";
import { clearNotification, showNotification } from "./notifications.js";
import { renderPresetDetails, renderPresetList, renderMixerPanel } from "./views.js";
import { clonePreset, uiState } from "./state.js";
import { buildAttachments, buildAttachmentsFromPreset, getDefaultPresets, initializeDataLibraries, REMOTE_BASE_URL } from "./dataLibraries.js";
import { arrayBufferToBase64, isRemoteUrl, resolveAttachmentUrl, sha256HexFromBase64 } from "./utils.js";
import { buildArchiveFileName, generateResourceId, requestResourceData, sanitizeFilename } from "./archiveUtils.js";
import type { Preset, Attachment, BlendDefinition, ResourceRef, LibraryResource } from "./types.js";
import { bindDemoAudioControls } from "./demoAudio.js";
import { postMessage } from "./bridge.js";
import { renderSignalPathBar } from "./signalPath.js";

const presetDropdown = document.getElementById("preset-dropdown") as HTMLSelectElement | null;
const prevPresetBtn = document.getElementById("prev-preset");
const nextPresetBtn = document.getElementById("next-preset");
const presetSearchElement = document.getElementById("preset-search") as HTMLInputElement | null;

export function bindLoadButtons(): void {
  const loadModelBtn = document.getElementById("load-model-btn");
  const loadIRBtn = document.getElementById("load-ir-btn");

  if (loadModelBtn) {
    loadModelBtn.addEventListener("click", () => {
      postMessage({ type: "browseModel" });
      appendLog("browseModel → requested");
    });
  }

  if (loadIRBtn) {
    loadIRBtn.addEventListener("click", () => {
      postMessage({ type: "browseIR" });
      appendLog("browseIR → requested");
    });
  }
}

export function loadModelFromPath(filePath: string): void {
  postMessage({
    type: "loadModel",
    filePath,
  });
  appendLog(`loadModel → ${filePath}`);
}

export function loadIRFromPath(filePath: string): void {
  postMessage({
    type: "loadIR",
    filePath,
  });
  appendLog(`loadIR → ${filePath}`);
}

export function requestSignalPathTest(): void {
  clearNotification();
  postMessage({
    type: "runSignalPathTest",
    frequency: 440,
    duration: 1.0,
  });
}

function renderPresetUI(preset: Preset | null): void {
  renderPresetList(uiState.filteredPresets, uiState.activePresetId, async (presetId) => {
    await applyPresetFromLibrary(presetId);
  });

  renderPresetDetails(preset, {
    onPresetSelected: async (presetId) => {
      await applyPresetFromLibrary(presetId);
    },
    onApplyPreset: async (presetId) => {
      await applyPresetFromLibrary(presetId);
    },
    onRequestSignalTest: requestSignalPathTest,
    onBindLoadButtons: bindLoadButtons,
  });
  bindDemoAudioControls();
  renderSignalPathBar();
}

export function renderActivePreset(): void {
  const active = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  renderPresetUI(active);
  renderMixerPanel();
}

export function filterPresets(query: string): void {
  const normalized = query.trim().toLowerCase();
  if (!normalized) {
    uiState.filteredPresets = uiState.presets.slice();
  } else {
    uiState.filteredPresets = uiState.presets.filter((preset) => {
      const tokens = [preset.name, preset.category, preset.description];
      return tokens.some((token) => token && token.toLowerCase().includes(normalized));
    });
  }
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

async function loadPresetMetadata(presetId: string): Promise<Preset> {
  if (uiState.presetCache.has(presetId)) {
    return clonePreset(uiState.presetCache.get(presetId) ?? null) as Preset;
  }

  const localPreset = uiState.presets.find((preset) => preset.id === presetId);
  if (localPreset) {
    uiState.presetCache.set(localPreset.id, localPreset);
    return clonePreset(localPreset) as Preset;
  }

  if (!REMOTE_BASE_URL) {
    throw new Error("Remote preset service is not configured.");
  }

  const baseUrl = REMOTE_BASE_URL.replace(/\/$/, "");
  const response = await fetch(`${baseUrl}/presets/${encodeURIComponent(presetId)}`);
  if (!response.ok) {
    throw new Error(`Failed to fetch preset ${presetId}: ${response.status}`);
  }

  const data = await response.json();
  const preset = Array.isArray(data) ? data[0] : data;
  if (!preset) {
    throw new Error(`Preset ${presetId} not found`);
  }

  uiState.presetCache.set(preset.id, preset);
  return clonePreset(preset) as Preset;
}

async function enrichAttachment(attachment: Attachment): Promise<Attachment> {
  if (attachment.data) {
    return attachment;
  }

  const url = resolveAttachmentUrl(attachment, REMOTE_BASE_URL);
  if (!url || !isRemoteUrl(url)) {
    return attachment;
  }

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to fetch attachment from ${url}`);
  }

  const buffer = await response.arrayBuffer();
  return { ...attachment, data: arrayBufferToBase64(buffer) };
}

export async function applyPresetFromLibrary(presetId: string): Promise<void> {
  try {
    clearNotification();
    const preset = await loadPresetMetadata(presetId);
    const attachments = await Promise.all((preset.attachments ?? []).map(enrichAttachment));
    const presetPayload: Preset = {
      ...preset,
      attachments,
    };

    uiState.presetCache.set(presetPayload.id, clonePreset(presetPayload));
    uiState.activePresetId = presetPayload.id;
    renderPresetUI(clonePreset(presetPayload));
    updatePresetActionButtons();
    postMessage({
      type: "loadPreset",
      preset: presetPayload,
    });
  } catch (error) {
    console.error("Failed to apply preset", error);
    showNotification("Failed to apply preset", error instanceof Error ? error.message : "Unknown error");
  }
}

export function savePresetToLocalStorage(preset: Preset): void {
  try {
    const savedPresets = JSON.parse(localStorage.getItem("guitarfx_user_presets") || "[]") as Preset[];
    const existingIndex = savedPresets.findIndex((p) => p.id === preset.id);
    if (existingIndex >= 0) {
      savedPresets[existingIndex] = preset;
    } else {
      savedPresets.push(preset);
    }
    localStorage.setItem("guitarfx_user_presets", JSON.stringify(savedPresets));
    console.log(`Preset '${preset.name}' saved to localStorage`);
  } catch (error) {
    console.error("Failed to save preset to localStorage", error);
  }
}

export function loadPresetsFromLocalStorage(): Preset[] {
  try {
    const savedPresets = JSON.parse(localStorage.getItem("guitarfx_user_presets") || "[]") as Preset[];
    console.log(`Loaded ${savedPresets.length} user presets from localStorage`);
    return savedPresets;
  } catch (error) {
    console.error("Failed to load presets from localStorage", error);
    return [];
  }
}

export async function loadPresetIndex(): Promise<void> {
  try {
    if (!REMOTE_BASE_URL) {
      throw new Error("Remote preset service disabled");
    }

    const response = await fetch(`${REMOTE_BASE_URL.replace(/\/$/, "")}/presets`);
    if (!response.ok) {
      throw new Error(`Failed to fetch presets index: ${response.status}`);
    }

    const data = await response.json();
    const presets = Array.isArray(data) ? data : data.presets ?? [];
    const basePresets = presets.length ? presets : getDefaultPresets();
    const userPresets = loadPresetsFromLocalStorage();
    uiState.presets = [...basePresets, ...userPresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  } catch (error) {
    console.error("Failed to load preset index", error);
    const basePresets = getDefaultPresets();
    const userPresets = loadPresetsFromLocalStorage();
    uiState.presets = [...basePresets, ...userPresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  }
}

export async function initializePresets(): Promise<void> {
  await initializeDataLibraries();

  if (REMOTE_BASE_URL) {
    await loadPresetIndex();
  } else {
    const basePresets = getDefaultPresets();
    const userPresets = loadPresetsFromLocalStorage();
    uiState.presets = [...basePresets, ...userPresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => uiState.presetCache.set(preset.id, preset));
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  }

  populatePresetDropdown();
  postMessage({ type: "requestState" });
}

export function populatePresetDropdown(): void {
  if (!presetDropdown) return;

  presetDropdown.innerHTML = "";

  uiState.presets.forEach((preset) => {
    const option = document.createElement("option");
    option.value = preset.id;
    option.textContent = preset.name;
    if (preset.id === uiState.activePresetId) {
      option.selected = true;
    }
    presetDropdown.appendChild(option);
  });
}

export function updatePresetDropdownSelection(): void {
  if (!presetDropdown || !uiState.activePresetId) return;
  presetDropdown.value = uiState.activePresetId;
}

function getActivePresetIndex(): number {
  if (!uiState.activePresetId) return -1;
  return uiState.presets.findIndex((p) => p.id === uiState.activePresetId);
}

export async function selectPreviousPreset(): Promise<void> {
  if (!uiState.presets.length) return;

  let index = getActivePresetIndex();
  if (index <= 0) {
    index = uiState.presets.length - 1;
  } else {
    index--;
  }

  const preset = uiState.presets[index];
  if (preset) {
    await applyPresetFromLibrary(preset.id);
    updatePresetDropdownSelection();
  }
}

export async function selectNextPreset(): Promise<void> {
  if (!uiState.presets.length) return;

  let index = getActivePresetIndex();
  if (index < 0 || index >= uiState.presets.length - 1) {
    index = 0;
  } else {
    index++;
  }

  const preset = uiState.presets[index];
  if (preset) {
    await applyPresetFromLibrary(preset.id);
    updatePresetDropdownSelection();
  }
}

export function initializePresetControls(): void {
  if (presetDropdown) {
    presetDropdown.addEventListener("change", async (event) => {
      const presetId = (event.target as HTMLSelectElement).value;
      if (presetId) {
        await applyPresetFromLibrary(presetId);
      }
    });
  }

  if (prevPresetBtn) {
    prevPresetBtn.addEventListener("click", async () => {
      await selectPreviousPreset();
    });
  }

  if (nextPresetBtn) {
    nextPresetBtn.addEventListener("click", async () => {
      await selectNextPreset();
    });
  }

  if (presetSearchElement) {
    presetSearchElement.addEventListener("input", (event) => {
      filterPresets((event.target as HTMLInputElement).value ?? "");
    });
  }
}

// Save preset modal helpers
export function openSavePresetModal(): void {
  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;

  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;

  if (nameInput) nameInput.value = "";
  if (categoryInput) categoryInput.value = "User";
  if (descriptionInput) descriptionInput.value = "";

  modal.style.display = "flex";
}

export function closeSavePresetModal(): void {
  const modal = document.getElementById("save-preset-modal");
  if (modal) {
    modal.style.display = "none";
    // Clear editing state
    delete modal.dataset.editingPresetId;
  }
}

export function saveCurrentPreset(): void {
  const modal = document.getElementById("save-preset-modal");
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;

  const name = nameInput?.value?.trim() || "";
  const category = categoryInput?.value?.trim() || "User";
  const description = descriptionInput?.value?.trim() || "";

  if (!name) {
    showNotification("Error", "Preset name is required");
    return;
  }

  // Check if we're editing an existing preset
  const editingPresetId = modal?.dataset.editingPresetId;

  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  const baseAttachments = buildAttachmentsFromPreset(activePreset ?? {} as Preset);

  if (editingPresetId) {
    // Editing existing preset
    const existingPreset = uiState.presetCache.get(editingPresetId);
    if (existingPreset) {
      const updatedPreset: Preset = {
        ...existingPreset,
        name,
        category,
        description,
        attachments: baseAttachments,
      };

      savePresetToLocalStorage(updatedPreset);
      uiState.presetCache.set(editingPresetId, updatedPreset);
      const index = uiState.presets.findIndex((p) => p.id === editingPresetId);
      if (index >= 0) {
        uiState.presets[index] = updatedPreset;
      }
      uiState.filteredPresets = uiState.presets.slice();
      populatePresetDropdown();
      renderPresetUI(clonePreset(updatedPreset));
      closeSavePresetModal();
      showNotification("Preset updated", name);
      updatePresetActionButtons();
      return;
    }
  }

  // Creating new preset
  const basePreset = clonePreset(activePreset ?? ({} as Preset));
  const newPreset: Preset = {
    ...basePreset,
    id: `${Date.now()}`,
    name,
    category,
    description,
    attachments: baseAttachments,
  };

  savePresetToLocalStorage(newPreset);
  uiState.presets.unshift(newPreset);
  uiState.filteredPresets = uiState.presets.slice();
  uiState.presetCache.set(newPreset.id, newPreset);
  uiState.activePresetId = newPreset.id;
  populatePresetDropdown();
  renderPresetUI(clonePreset(newPreset));
  closeSavePresetModal();
  showNotification("Preset saved", newPreset.name);
  updatePresetActionButtons();
}

export function initializeSavePresetModal(): void {
  const closeBtn = document.getElementById("save-preset-modal-close");
  const cancelBtn = document.getElementById("save-preset-cancel");
  const confirmBtn = document.getElementById("save-preset-confirm");
  const modal = document.getElementById("save-preset-modal");

  if (closeBtn) {
    closeBtn.addEventListener("click", closeSavePresetModal);
  }

  if (cancelBtn) {
    cancelBtn.addEventListener("click", closeSavePresetModal);
  }

  if (confirmBtn) {
    confirmBtn.addEventListener("click", () => {
      saveCurrentPreset();
    });
  }

  if (modal) {
    modal.addEventListener("click", (event) => {
      if (event.target === modal) {
        closeSavePresetModal();
      }
    });
  }
}

export function initializeSaveAsButton(): void {
  const saveAsBtn = document.getElementById("preset-save-as-btn");
  if (saveAsBtn) {
    saveAsBtn.addEventListener("click", openSavePresetModal);
  }
}

type PresetArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
};

type PresetArchive = {
  formatVersion: number;
  preset: Preset;
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
};

function getLibraryResource(resourceType: string, resourceId: string): LibraryResource | undefined {
  const resources = uiState.resourceLibrary[resourceType] ?? [];
  return resources.find((res) => res.id === resourceId);
}

function getLibraryResourceByHash(resourceType: string, hash?: string): LibraryResource | undefined {
  if (!hash) {
    return undefined;
  }
  const resources = uiState.resourceLibrary[resourceType] ?? [];
  return resources.find((res) => res.hash?.toLowerCase() === hash.toLowerCase());
}

function collectPresetBlendIds(preset: Preset): string[] {
  if (!preset.graph?.nodes) {
    return [];
  }

  const ids = new Set<string>();
  preset.graph.nodes.forEach((node) => {
    const blendId = node.config?.blendId ?? "";
    if (blendId) {
      ids.add(blendId);
    }
  });

  return Array.from(ids);
}

function collectPresetResourceRefs(preset: Preset, blendDefs: BlendDefinition[]): ResourceRef[] {
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

  if (preset.graph?.nodes) {
    preset.graph.nodes.forEach((node) => {
      if (node.resource) {
        addRef(node.resource.type, node.resource.id, node.resource.filePath);
      }
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => addRef(res.type, res.id, res.filePath));
      }
    });
  }

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
  });

  return refs;
}

async function exportCurrentPresetArchive(): Promise<void> {
  const presetId = uiState.activePresetId ?? "";
  const preset = uiState.presetCache.get(presetId) ?? null;
  if (!preset) {
    showNotification("Export failed", "No preset selected");
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

  const blendIds = collectPresetBlendIds(preset);
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.includes(blend.id));
  const resourceRefs = collectPresetResourceRefs(preset, blendDefs);
  const exportResources: PresetArchiveResource[] = [];

  for (const ref of resourceRefs) {
    const resource = getLibraryResource(ref.type, ref.id);
    if (!resource) {
      continue;
    }
    const fileName = buildArchiveFileName(resource, ref.type);
    const data = await requestResourceData(ref.type, ref.id);
    if (!data) {
      continue;
    }
    const hash = await sha256HexFromBase64(data);
    resourcesFolder.file(fileName, data, { base64: true });
    exportResources.push({
      id: resource.id,
      name: resource.name,
      category: resource.category,
      type: ref.type,
      fileName,
      hash,
    });
  }

  const archive: PresetArchive = {
    formatVersion: 1,
    preset: clonePreset(preset),
    resources: exportResources,
    blends: blendDefs,
  };

  zip.file("preset.json", JSON.stringify(archive, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  postMessage({
    type: "savePresetArchive",
    fileName: `${sanitizeFilename(preset.name || preset.id || "preset")}.soundshed.zip`,
    data,
  });
}

async function importPresetArchive(file: File): Promise<void> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Import failed", "Archive library not available");
    return;
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);
  const presetEntry = zip.file("preset.json");
  if (!presetEntry) {
    showNotification("Import failed", "Archive is missing preset.json");
    return;
  }

  const presetText = await presetEntry.async("text");
  const archive = JSON.parse(presetText) as PresetArchive;
  if (!archive.preset) {
    showNotification("Import failed", "Archive has no preset data");
    return;
  }

  const zipFiles = Object.values(zip.files) as JSZipObject[];
  const fileMap = new Map<string, JSZipObject>();
  zipFiles.forEach((entry) => {
    if (!entry.dir) {
      const name = entry.name.replace(/^resources\//, "");
      fileMap.set(name, entry);
    }
  });

  const idMap = new Map<string, string>();
  const resourcesToImport = archive.resources ?? [];
  for (const resource of resourcesToImport) {
    const fileName = resource.fileName ?? "";
    const existing = getLibraryResourceByHash(resource.type, resource.hash);
    if (existing) {
      idMap.set(resource.id, existing.id);
      continue;
    }
    const entry = fileMap.get(fileName);
    if (!entry) {
      continue;
    }
    const dataBuffer = await entry.async("arraybuffer");
    const data = arrayBufferToBase64(dataBuffer);
    const newId = generateResourceId(fileName);
    idMap.set(resource.id, newId);

    postMessage({
      type: "importRemoteResource",
      provider: "presetArchive",
      resourceType: resource.type,
      resourceId: newId,
      name: resource.name ?? fileName,
      description: "",
      category: resource.category ?? "",
      subfolder: "preset-imports",
      fileName,
      hash: resource.hash ?? "",
      metadata: {
        provider: "presetArchive",
        sourceFile: fileName,
      },
      data,
    });
  }

  const blendIdMap = new Map<string, string>();
  const blends = archive.blends ?? [];
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

  const importedPreset = clonePreset(archive.preset);
  importedPreset.id = generateResourceId(importedPreset.id || importedPreset.name || "preset");
  importedPreset.name = importedPreset.name?.endsWith(" (Imported)")
    ? importedPreset.name
    : `${importedPreset.name || "Imported Preset"} (Imported)`;

  if (importedPreset.graph?.nodes) {
    importedPreset.graph.nodes.forEach((node) => {
      if (node.resource?.id) {
        node.resource.id = idMap.get(node.resource.id) ?? node.resource.id;
      }
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => {
          res.id = idMap.get(res.id) ?? res.id;
        });
      }
      if (node.config?.blendId) {
        node.config.blendId = blendIdMap.get(node.config.blendId) ?? node.config.blendId;
      }
    });
  }

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

  savePresetToLocalStorage(importedPreset);
  uiState.presets.unshift(importedPreset);
  uiState.filteredPresets = uiState.presets.slice();
  uiState.presetCache.set(importedPreset.id, importedPreset);
  uiState.activePresetId = importedPreset.id;
  populatePresetDropdown();
  renderPresetUI(clonePreset(importedPreset));
  updatePresetDropdownSelection();
  showNotification("Preset imported", importedPreset.name ?? "");
  updatePresetActionButtons();
}

// Delete preset from localStorage
export function deletePresetFromLocalStorage(presetId: string): boolean {
  try {
    const savedPresets = JSON.parse(localStorage.getItem("guitarfx_user_presets") || "[]") as Preset[];
    const index = savedPresets.findIndex((p) => p.id === presetId);
    if (index >= 0) {
      savedPresets.splice(index, 1);
      localStorage.setItem("guitarfx_user_presets", JSON.stringify(savedPresets));
      console.log(`Preset '${presetId}' deleted from localStorage`);
      return true;
    }
    return false;
  } catch (error) {
    console.error("Failed to delete preset from localStorage", error);
    return false;
  }
}

// Check if preset is a user preset (can be edited/deleted)
export function isUserPreset(presetId: string | null): boolean {
  if (!presetId) return false;
  const preset = uiState.presetCache.get(presetId);
  if (!preset) return false;
  // User presets have numeric IDs (timestamps) or start with "user-"
  return /^\d+$/.test(presetId) || presetId.startsWith("user-");
}

// Delete current preset
export function deleteCurrentPreset(): void {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!isUserPreset(activePresetId)) {
    showNotification("Error", "Cannot delete factory presets");
    return;
  }

  const preset = uiState.presetCache.get(activePresetId);
  const presetName = preset?.name ?? "Unknown";

  if (!confirm(`Are you sure you want to delete "${presetName}"?`)) {
    return;
  }

  if (deletePresetFromLocalStorage(activePresetId)) {
    // Remove from UI state
    const index = uiState.presets.findIndex((p) => p.id === activePresetId);
    if (index >= 0) {
      uiState.presets.splice(index, 1);
    }
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presetCache.delete(activePresetId);

    // Select first preset if available
    if (uiState.presets.length > 0) {
      uiState.activePresetId = uiState.presets[0].id;
      applyPresetFromLibrary(uiState.activePresetId);
    } else {
      uiState.activePresetId = null;
    }

    populatePresetDropdown();
    renderActivePreset();
    showNotification("Preset deleted", presetName);
  } else {
    showNotification("Error", "Failed to delete preset");
  }
}

// Save (overwrite) current preset
export function saveOverwriteCurrentPreset(): void {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!isUserPreset(activePresetId)) {
    showNotification("Error", "Cannot overwrite factory presets. Use 'Save As' instead.");
    return;
  }

  const existingPreset = uiState.presetCache.get(activePresetId);
  if (!existingPreset) {
    showNotification("Error", "Preset not found");
    return;
  }

  // Build updated preset with current parameters from graph nodes
  const baseAttachments = buildAttachmentsFromPreset(existingPreset);

  const updatedPreset: Preset = {
    ...existingPreset,
    attachments: baseAttachments,
  };

  // Save to localStorage
  savePresetToLocalStorage(updatedPreset);

  // Update cache
  uiState.presetCache.set(activePresetId, updatedPreset);
  const index = uiState.presets.findIndex((p) => p.id === activePresetId);
  if (index >= 0) {
    uiState.presets[index] = updatedPreset;
  }

  showNotification("Preset saved", existingPreset.name);
}

// Open edit preset modal (reuses save modal with pre-filled data)
export function openEditPresetModal(): void {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!isUserPreset(activePresetId)) {
    showNotification("Error", "Cannot edit factory presets. Use 'Save As' instead.");
    return;
  }

  const preset = uiState.presetCache.get(activePresetId);
  if (!preset) {
    showNotification("Error", "Preset not found");
    return;
  }

  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;

  // Pre-fill with existing preset data
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;

  if (nameInput) nameInput.value = preset.name;
  if (categoryInput) categoryInput.value = preset.category || "User";
  if (descriptionInput) descriptionInput.value = preset.description || "";

  // Store that we're editing, not creating
  modal.dataset.editingPresetId = activePresetId;

  modal.style.display = "flex";
}

// Update preset action button states
export function updatePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn") as HTMLButtonElement | null;
  const saveBtn = document.getElementById("preset-save-btn") as HTMLButtonElement | null;
  const deleteBtn = document.getElementById("preset-delete-btn") as HTMLButtonElement | null;
  const exportBtn = document.getElementById("preset-export-btn") as HTMLButtonElement | null;

  const canModify = isUserPreset(uiState.activePresetId);

  if (editBtn) {
    editBtn.disabled = !canModify;
    editBtn.title = canModify ? "Edit Preset" : "Cannot edit factory presets";
  }
  if (saveBtn) {
    saveBtn.disabled = !canModify;
    saveBtn.title = canModify ? "Save Preset" : "Cannot overwrite factory presets";
  }
  if (deleteBtn) {
    deleteBtn.disabled = !canModify;
    deleteBtn.title = canModify ? "Delete Preset" : "Cannot delete factory presets";
  }
  if (exportBtn) {
    exportBtn.disabled = !uiState.activePresetId;
    exportBtn.title = uiState.activePresetId ? "Export Preset" : "No preset to export";
  }
}

// Initialize preset action buttons
export function initializePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn");
  const saveBtn = document.getElementById("preset-save-btn");
  const deleteBtn = document.getElementById("preset-delete-btn");
  const exportBtn = document.getElementById("preset-export-btn");
  const importBtn = document.getElementById("preset-import-btn");
  const importInput = document.getElementById("preset-import-input") as HTMLInputElement | null;

  if (editBtn) {
    editBtn.addEventListener("click", openEditPresetModal);
  }

  if (saveBtn) {
    saveBtn.addEventListener("click", saveOverwriteCurrentPreset);
  }

  if (deleteBtn) {
    deleteBtn.addEventListener("click", deleteCurrentPreset);
  }

  if (exportBtn) {
    exportBtn.addEventListener("click", () => void exportCurrentPresetArchive());
  }

  if (importBtn) {
    importBtn.addEventListener("click", () => importInput?.click());
  }

  if (importInput) {
    importInput.addEventListener("change", () => {
      const file = importInput.files?.[0];
      importInput.value = "";
      if (file) {
        void importPresetArchive(file);
      }
    });
  }

  // Initial state
  updatePresetActionButtons();
}
