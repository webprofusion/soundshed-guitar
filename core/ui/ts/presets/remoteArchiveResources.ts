/**
 * Remote resources in preset archives: the models and IRs an archive names instead of carrying,
 * because their licence or their provider's terms do not let them be passed on. Whoever imports
 * the archive fetches each one from its provider.
 *
 * Archives list them in `remoteResources` (RemoteResourceRef, any provider). Archives written for
 * app versions before that also list the Tone3000 ones in `tone3000Resources`, which those
 * versions read; both are written and either is read.
 */

import { postMessage } from "../bridge.js";
import { appendLog } from "../logging.js";
import { switchMainPanel } from "../navigation.js";
import { licencePolicy, readRemoteAttribution, remoteResourceMetadata } from "../remoteResources.js";
import type { RemoteResourceRef } from "../remoteResources.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { activateLibraryTab } from "../settings.js";
import { uiState } from "../state.js";
import { downloadTone3000ResourceByReference, isTone3000AuthReady, isTone3000ProxyModeEnabled, isTone3000RequestUrlAllowed, saveTone3000ApiKey } from "../tone3000.js";
import type { LibraryResource } from "../types.js";
import { arrayBufferToBase64 } from "../utils.js";
import { sanitizeFilename } from "../archiveUtils.js";
import type { ArchivePreview, PreviewResource, Tone3000ResourceRef } from "./archiveTypes.js";

const TONE3000 = "tone3000";

/** Providers this app can fetch an archive's remote resources from. */
const FETCHABLE_PROVIDERS = new Set([TONE3000]);

/**
 * Whether an export names this resource instead of carrying its file. Tone3000's terms forbid
 * passing its files on, whatever the creator's licence; another provider's file is named when its
 * licence does not allow redistribution and the app knows how to fetch it from there.
 */
export function shouldReferenceRemotely(resource: LibraryResource): boolean {
  const attribution = readRemoteAttribution(resource.id, resource.metadata);
  if (!attribution) {
    return false;
  }
  if (attribution.provider === TONE3000) {
    return true;
  }
  return FETCHABLE_PROVIDERS.has(attribution.provider) && licencePolicy(attribution.licence) !== "redistributable";
}

/** The archive entries for a resource exported by reference: the neutral one, and the legacy one for a Tone3000 file. */
export function buildArchiveRemoteRef(resource: LibraryResource, type: string): { ref: RemoteResourceRef; legacy: Tone3000ResourceRef | null } {
  const attribution = readRemoteAttribution(resource.id, resource.metadata);
  if (!attribution) {
    throw new Error(`Resource "${resource.name || resource.id}" has no provider and cannot be shared by reference.`);
  }
  const metadata = resource.metadata ?? {};
  const ref: RemoteResourceRef = {
    ...attribution,
    id: resource.id,
    type,
    name: resource.name,
    category: resource.category,
    ...(resource.hash ? { hash: resource.hash } : {}),
  };
  if (attribution.provider !== TONE3000) {
    return { ref, legacy: null };
  }
  const toneId = metadata.toneId?.trim() ?? "";
  const modelId = metadata.modelId?.trim() ?? "";
  if (!toneId || !modelId) {
    throw new Error(`Tone3000 resource "${resource.name || resource.id}" is missing toneId/modelId metadata and cannot be shared.`);
  }
  ref.providerData = { toneId, modelId };
  const creatorId = metadata.creatorId?.trim() || undefined;
  const creatorName = metadata.creatorName?.trim() || metadata.authorUsername?.trim() || undefined;
  return {
    ref,
    legacy: { id: resource.id, name: resource.name, category: resource.category, type, toneId, modelId, creatorId, creatorName },
  };
}

function fromLegacyTone3000Ref(legacy: Tone3000ResourceRef): RemoteResourceRef {
  return {
    id: legacy.id,
    type: legacy.type,
    name: legacy.name,
    category: legacy.category,
    provider: TONE3000,
    providerKey: legacy.modelId ?? legacy.id.replace(/^tone3000:/, ""),
    creatorName: legacy.creatorName ?? null,
    providerData: {
      ...(legacy.toneId ? { toneId: legacy.toneId } : {}),
      ...(legacy.modelId ? { modelId: legacy.modelId } : {}),
      ...(legacy.modelUrl ? { modelUrl: legacy.modelUrl } : {}),
      ...(legacy.creatorId ? { creatorId: legacy.creatorId } : {}),
    },
  };
}

/** Every remote resource an archive names, from either list, once each by library id. */
export function readArchiveRemoteRefs(archive: { remoteResources?: RemoteResourceRef[]; tone3000Resources?: Tone3000ResourceRef[] }): RemoteResourceRef[] {
  const refs = new Map<string, RemoteResourceRef>();
  for (const ref of Array.isArray(archive.remoteResources) ? archive.remoteResources : []) {
    if (ref?.id && ref.type && ref.provider) {
      refs.set(ref.id, ref);
    }
  }
  for (const legacy of Array.isArray(archive.tone3000Resources) ? archive.tone3000Resources : []) {
    if (legacy?.id && legacy.type && !refs.has(legacy.id)) {
      refs.set(legacy.id, fromLegacyTone3000Ref(legacy));
    }
  }
  return [...refs.values()];
}

/** Points the archive's ids at resources already in the library, fetching nothing. */
export function mapInstalledRemoteResources(refs: RemoteResourceRef[], idMap: Map<string, string>): void {
  for (const ref of refs) {
    const existing = getLibraryResource(ref.type, ref.id);
    if (existing && !existing.fileMissing) {
      idMap.set(ref.id, existing.id);
    }
  }
}

let tone3000RequirementResolve: ((value: boolean) => void) | null = null;

function closeTone3000RequirementModal(result: boolean): void {
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

async function promptForTone3000ApiKey(resourceCount: number): Promise<boolean> {
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

/** Starts a Tone3000 session from the saved key when there is none. True when models can be fetched. */
async function startTone3000Access(): Promise<boolean> {
  if (isTone3000ProxyModeEnabled() || isTone3000AuthReady()) {
    return true;
  }
  const storedApiKey = typeof uiState.appSettings["tone3000.apiKey"] === "string"
    ? (uiState.appSettings["tone3000.apiKey"] as string).trim()
    : "";
  if (storedApiKey) {
    appendLog("tone3000 session missing; attempting auto-start from saved API key");
    await saveTone3000ApiKey(storedApiKey);
  }
  return isTone3000AuthReady();
}

async function ensureTone3000Access(refCount: number): Promise<void> {
  if (!(await startTone3000Access()) && !(await promptForTone3000ApiKey(refCount))) {
    throw new Error("Tone3000 API key is required to import this shared preset");
  }
}

async function fetchTone3000(ref: RemoteResourceRef): Promise<{ data: string; metadata: Record<string, string> }> {
  const extra = ref.providerData ?? {};
  const modelUrl = extra.modelUrl;
  const buffer = await downloadTone3000ResourceByReference({ toneId: extra.toneId, modelId: extra.modelId ?? ref.providerKey, modelUrl });
  return {
    data: arrayBufferToBase64(buffer),
    metadata: {
      toneId: extra.toneId ?? "",
      creatorId: extra.creatorId ?? "",
      authorUsername: ref.creatorName ?? "",
      modelId: extra.modelId ?? ref.providerKey,
      ...(modelUrl && isTone3000RequestUrlAllowed(modelUrl) ? { modelUrl } : {}),
    },
  };
}

/**
 * A preview's remote resources: those the library has are played from there, and with `preview`
 * the rest are fetched for the engine to stage. Nothing is imported. Unlike an install this never
 * asks for a Tone3000 key; without access, or when a fetch fails, the resource is listed
 * unavailable and the preview plays without it.
 */
export async function prepareArchiveRemoteResourcesForPreview(
  refs: RemoteResourceRef[],
  idMap: Map<string, string>,
  preview: ArchivePreview | undefined,
): Promise<void> {
  mapInstalledRemoteResources(refs, idMap);
  if (!preview) {
    return;
  }
  const missing = refs.filter((ref) => {
    const existing = getLibraryResource(ref.type, ref.id);
    return !(existing && !existing.fileMissing);
  });
  if (missing.length === 0) {
    return;
  }
  const canFetch = missing.some((ref) => ref.provider === TONE3000) && await startTone3000Access();
  const fetched = await Promise.all(missing.map(async (ref): Promise<PreviewResource | null> => {
    if (ref.provider !== TONE3000 || !canFetch) {
      return null;
    }
    try {
      const { data } = await fetchTone3000(ref);
      return { resourceType: ref.type, resourceId: ref.id, name: ref.name ?? ref.id, data };
    } catch (error) {
      const msg = error instanceof Error ? error.message : String(error);
      appendLog(`${ref.provider} preview resource failed (${ref.name ?? ref.id}): ${msg}`);
      return null;
    }
  }));
  fetched.forEach((resource, index) => {
    if (resource) {
      preview.resources.push(resource);
    } else {
      preview.unavailable.push(missing[index].name ?? missing[index].id);
    }
  });
}

/**
 * Brings an archive's remote resources into the library: one already there is reused, the rest
 * are fetched from their providers. Returns the names of those that could not be had (a
 * provider this app cannot fetch from, or a model its creator has since removed or made
 * private), so the presets can still be imported without them. Throws only when Tone3000
 * access is needed and the user gives none.
 */
export async function importArchiveRemoteResources(
  refs: RemoteResourceRef[],
  idMap: Map<string, string>,
  importedResources: Array<{ type: string; id: string }>,
): Promise<string[]> {
  const tone3000Count = refs.filter((ref) => ref.provider === TONE3000).length;
  const needsFetch = refs.some((ref) => {
    const existing = getLibraryResource(ref.type, ref.id);
    return ref.provider === TONE3000 && !(existing && !existing.fileMissing);
  });
  if (needsFetch) {
    await ensureTone3000Access(tone3000Count);
  }

  const unavailable: string[] = [];
  for (const ref of refs) {
    const existing = getLibraryResource(ref.type, ref.id);
    if (existing && !existing.fileMissing) {
      idMap.set(ref.id, existing.id);
      importedResources.push({ type: ref.type, id: existing.id });
      continue;
    }
    if (!FETCHABLE_PROVIDERS.has(ref.provider)) {
      appendLog(`archive resource skipped (${ref.name ?? ref.id}): this app cannot fetch from ${ref.provider}`);
      unavailable.push(ref.name ?? ref.id);
      continue;
    }
    try {
      const fetched = await fetchTone3000(ref);
      const extension = ref.type === "ir" ? ".wav" : ".nam";
      postMessage({
        type: "importRemoteResource",
        provider: ref.provider,
        resourceType: ref.type,
        resourceId: ref.id,
        name: ref.name ?? ref.id,
        description: "",
        category: ref.category ?? "",
        subfolder: "preset-imports",
        fileName: `${sanitizeFilename(ref.name ?? ref.id, "resource")}${extension}`,
        metadata: {
          ...remoteResourceMetadata(ref),
          ...fetched.metadata,
          creatorName: ref.creatorName ?? "",
        },
        data: fetched.data,
      });
      idMap.set(ref.id, ref.id);
      importedResources.push({ type: ref.type, id: ref.id });
      appendLog(`${ref.provider} archive resource imported: ${ref.name ?? ref.id}`);
    } catch (error) {
      const msg = error instanceof Error ? error.message : String(error);
      appendLog(`${ref.provider} archive resource failed (${ref.name ?? ref.id}): ${msg}`);
      unavailable.push(ref.name ?? ref.id);
    }
  }
  return unavailable;
}
