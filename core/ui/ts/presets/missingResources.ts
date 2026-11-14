/**
 * Models and IRs a loaded preset or DAW project names that the library no longer has.
 *
 * A project keeps a model as a reference to the library, not as the file. Each reference also
 * carries the `source` the engine recorded when it was saved (ResourceRef.source): the model's
 * name and what its provider needs to find it again. When the library has lost one, the slot is
 * labelled with that name, and a Tone3000 model can be downloaded again under the same library
 * id, so the preset picks it up as it was.
 */

import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { getPresetSceneGraphs } from "../presetScenes.js";
import type { RemoteResourceRef } from "../remoteResources.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { getActivePresetForRender, uiState } from "../state.js";
import type { LibraryResource, Preset } from "../types.js";
import { importArchiveRemoteResources } from "./remoteArchiveResources.js";

const TONE3000 = "tone3000";

export interface MissingPresetResource {
  type: string;
  id: string;
  name: string;
  source: Record<string, string>;
}

function text(values: Record<string, string> | undefined, key: string): string {
  const value = values?.[key];
  return typeof value === "string" ? value.trim() : "";
}

/** A library resource, with its file, that a saved source describes (as the engine matches it). */
function findBySource(type: string, source: Record<string, string>): LibraryResource | undefined {
  const resources = (uiState.resourceLibrary[type] ?? []).filter((resource) => !resource.fileMissing);
  const provider = text(source, "provider");
  const providerKey = text(source, "providerKey");
  const contentHash = text(source, "contentHash");
  const hash = text(source, "hash");
  if (provider && providerKey) {
    const found = resources.find((resource) => text(resource.metadata, "provider") === provider
      && text(resource.metadata, "providerKey") === providerKey);
    if (found) return found;
  }
  if (hash) {
    const found = resources.find((resource) => (resource.hash ?? "") === hash);
    if (found) return found;
  }
  if (contentHash) {
    return resources.find((resource) => text(resource.metadata, "contentHash") === contentHash);
  }
  return undefined;
}

/**
 * The model and IR references in these presets (their graphs and every scene's) whose library
 * entry, or its file, is gone and that no other library copy matches. Once each by type and id.
 * A reference by file path alone is left out: the UI cannot tell whether the file is there.
 */
export function findMissingPresetResources(presets: Array<Preset | null | undefined>): MissingPresetResource[] {
  const missing = new Map<string, MissingPresetResource>();
  for (const preset of presets) {
    for (const graph of getPresetSceneGraphs(preset)) {
      for (const node of graph.nodes) {
        for (const ref of node.resources ?? []) {
          const type = ref.resourceType ?? ref.type ?? "";
          const id = ref.resourceId ?? ref.id ?? "";
          if ((type !== "nam" && type !== "ir") || !id || ref.filePath) {
            continue;
          }
          const existing = getLibraryResource(type, id);
          if (existing && !existing.fileMissing) {
            continue;
          }
          const source = ref.source ?? {};
          if (findBySource(type, source)) {
            continue;
          }
          const key = `${type}:${id}`;
          if (!missing.has(key)) {
            missing.set(key, { type, id, name: text(source, "name") || existing?.name || id, source });
          }
        }
      }
    }
  }
  return [...missing.values()];
}

/** The reference to fetch a missing resource from its provider by, or null when it cannot be. */
export function remoteRefForMissingResource(missing: MissingPresetResource): RemoteResourceRef | null {
  const { source } = missing;
  if (text(source, "provider") !== TONE3000) {
    return null;
  }
  const toneId = text(source, "toneId");
  const modelId = text(source, "modelId");
  const contentHash = text(source, "contentHash");
  if (!(toneId && modelId) && !contentHash) {
    return null;
  }
  return {
    id: missing.id,
    type: missing.type,
    name: missing.name,
    category: text(source, "category") || undefined,
    provider: TONE3000,
    providerKey: text(source, "providerKey") || modelId,
    creatorName: text(source, "creatorName") || null,
    ...(text(source, "hash") ? { hash: text(source, "hash") } : {}),
    ...(contentHash ? { contentHash } : {}),
    providerData: {
      ...(toneId ? { toneId } : {}),
      ...(modelId ? { modelId } : {}),
    },
  };
}

/** The presets running now: the one shown, and every mixer slot. */
function runningPresets(): Array<Preset | null | undefined> {
  const presets: Array<Preset | null | undefined> = [getActivePresetForRender()];
  for (const id of uiState.mixer?.activePresetIds ?? []) {
    presets.push(uiState.presetCache.get(id));
  }
  return presets;
}

/** Missing resources already reported this session, by type and id, so each is raised once. */
const reported = new Set<string>();
let offering = false;

function listNames(names: string[]): string {
  const shown = names.slice(0, 6).map((name) => `• ${name}`);
  if (names.length > shown.length) {
    shown.push(`• and ${names.length - shown.length} more`);
  }
  return shown.join("\n");
}

/**
 * Tells the user about models and IRs the running presets need and the library lacks, once each,
 * and offers to download the Tone3000 ones again. Called whenever the engine's state arrives.
 */
export async function offerMissingPresetResources(): Promise<void> {
  // Before the library arrives every reference looks missing.
  if (offering || Object.keys(uiState.resourceLibrary).length === 0) {
    return;
  }
  const missing = findMissingPresetResources(runningPresets())
    .filter((resource) => !reported.has(`${resource.type}:${resource.id}`));
  if (missing.length === 0) {
    return;
  }
  for (const resource of missing) {
    reported.add(`${resource.type}:${resource.id}`);
    appendLog(`missing ${resource.type} "${resource.id}" (${resource.name})`);
  }

  const fetchable = isFeatureEnabled(Features.Tone3000)
    ? missing.map((resource) => remoteRefForMissingResource(resource)).filter((ref): ref is RemoteResourceRef => ref !== null)
    : [];
  const unfetchable = missing.filter((resource) => !fetchable.some((ref) => ref.id === resource.id && ref.type === resource.type));

  if (unfetchable.length > 0) {
    showNotification(
      unfetchable.length === 1 ? "A model is missing" : `${unfetchable.length} models are missing`,
      unfetchable.map((resource) => resource.name).join(", "),
    );
  }
  if (fetchable.length === 0) {
    return;
  }

  offering = true;
  try {
    const names = fetchable.map((ref) => ref.name ?? ref.id);
    const download = await showConfirm(
      `${fetchable.length === 1 ? "This tone uses a Tone3000 model" : `This tone uses ${fetchable.length} Tone3000 models`} that ${fetchable.length === 1 ? "is" : "are"} no longer in your library:\n\n${listNames(names)}\n\nDownload ${fetchable.length === 1 ? "it" : "them"} again from Tone3000?`,
      "Missing Tone3000 models",
      { ok: "Download", cancel: "Not now" },
    );
    if (!download) {
      return;
    }
    const unavailable = await importArchiveRemoteResources(fetchable, new Map(), []);
    // The imports are queued ahead of this, so the rebuilt slots find them.
    postMessage({ type: "reloadPresetResources" });
    if (unavailable.length > 0) {
      showNotification("Some models could not be downloaded", unavailable.join(", "));
    } else {
      showNotification("Models downloaded", names.join(", "));
    }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    appendLog(`missing model download failed: ${message}`);
    showNotification("Download failed", message);
  } finally {
    offering = false;
  }
}
