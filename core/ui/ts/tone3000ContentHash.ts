/**
 * Finding a library model's Tone3000 page by what it is rather than what it is called.
 *
 * The engine gives every NAM model and IR in the library a content hash (metadata.contentHash,
 * core/src/models/ContentHash.h): its weights or its samples, whatever the file is named or its
 * metadata says. Tone3000 gives each of its models the same hash and looks models up by it, so a
 * file from a folder, a friend or an old download finds its tone, and one whose Tone3000 ids have
 * changed (re-uploaded, moved) finds its new ones.
 */

import { postMessage } from "./bridge.js";
import { showNotification } from "./notifications.js";
import type { ResourceType } from "./resourceBrowser/types.js";
import { uiState } from "./state.js";
import type { Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import { chooseContentHashMatch, fetchTone3000Tone, isContentHash, lookupTone3000ModelsByContentHash } from "./tone3000.js";
import { tone3000ResourceMetadata } from "./tone3000Shared.js";
import type { LibraryResource } from "./types.js";
import { findResourceById } from "./utils.js";

export interface Tone3000ContentHashMatch {
  tone: Tone3000Tone;
  model: Tone3000Model;
  /** How many other Tone3000 models share the hash (reposts of the same file). */
  others: number;
}

/** Providers whose rows a match may re-attribute: a local file, or a Tone3000 model whose ids moved. */
const MATCHABLE_PROVIDERS = new Set(["", "local", "tone3000"]);

/** Whether the library row can be looked up on Tone3000 by its content hash. */
export function canLookUpOnTone3000(resource: LibraryResource | undefined): boolean {
  const metadata = resource?.metadata ?? {};
  return isContentHash(metadata.contentHash) && MATCHABLE_PROVIDERS.has(metadata.provider?.trim() ?? "");
}

/**
 * The Tone3000 model with the same content as the row, and its tone, or null when Tone3000 has
 * none it shows. A row that already names a tone prefers a model in that tone.
 */
export async function findTone3000ByContentHash(resource: LibraryResource): Promise<Tone3000ContentHashMatch | null> {
  const contentHash = resource.metadata?.contentHash;
  if (!isContentHash(contentHash)) {
    return null;
  }
  const models = await lookupTone3000ModelsByContentHash(contentHash);
  // Try each model in preference order until one's tone can be read (a repost's tone may be private).
  const first = chooseContentHashMatch(models, resource.metadata?.toneId);
  const ordered = first ? [first, ...models.filter((model) => model !== first)] : [];
  for (const model of ordered) {
    if (model.tone_id == null) {
      continue;
    }
    const tone = await fetchTone3000Tone(model.tone_id);
    if (tone) {
      return { tone, model, others: models.length - 1 };
    }
  }
  return null;
}

/**
 * The row's metadata with a match applied: Tone3000's attribution, licence and ids, as an import
 * from that tone records them, over what the row had. What the engine read from the file itself
 * stays: the content hash, and the architecture version, which the file states exactly.
 */
export function withTone3000Match(existing: Record<string, string>, match: Tone3000ContentHashMatch): Record<string, string> {
  const fromTone3000 = tone3000ResourceMetadata(match.tone, {
    id: match.model.id,
    name: match.model.name,
    modelUrl: match.model.model_url,
    architectureVersion: match.model.architecture_version ?? undefined,
  });
  const kept: Record<string, string> = {};
  for (const key of ["contentHash", "architectureVersion"]) {
    if (existing[key]) {
      kept[key] = existing[key];
    }
  }
  return { ...existing, ...fromTone3000, ...kept, tone3000MatchedBy: "contentHash" };
}

/** Saves a match on the row, locally and in the engine's library. */
export function applyTone3000Match(resourceType: string, resource: LibraryResource, match: Tone3000ContentHashMatch): void {
  const metadata = withTone3000Match(resource.metadata ?? {}, match);
  resource.metadata = metadata;
  postMessage({
    type: "updateLibraryResource",
    resourceType,
    resourceId: resource.id,
    name: resource.name,
    category: resource.category,
    description: resource.description,
    metadata,
  });
}

/**
 * The library details panel's "Find on Tone3000": the row's model at Tone3000 by its content hash,
 * applied to the row when found, with what happened in a notification. `done` redraws the list.
 */
export async function lookUpOnTone3000(button: HTMLButtonElement, resourceType: ResourceType, done: () => void): Promise<void> {
  const resource = findResourceById(uiState.resourceLibrary[resourceType] ?? [], button.dataset.resourceId ?? "");
  if (!resource || !canLookUpOnTone3000(resource) || button.disabled) {
    return;
  }
  button.disabled = true;
  button.textContent = "Looking on Tone3000…";
  try {
    const match = await findTone3000ByContentHash(resource);
    if (!match) {
      showNotification("Not on Tone3000", `no Tone3000 model has the same ${resourceType === "ir" ? "samples" : "weights"} as "${resource.name}"`);
      return;
    }
    applyTone3000Match(resourceType, resource, match);
    const creator = match.tone.user?.username ? ` by ${match.tone.user.username}` : "";
    const reposts = match.others > 0 ? ` (${match.others} other upload${match.others === 1 ? "" : "s"} share it)` : "";
    showNotification("Found on Tone3000", `"${match.tone.title}"${creator}${reposts}`);
  } catch (error) {
    showNotification("Tone3000 lookup failed", error instanceof Error ? error.message : String(error));
  } finally {
    done();
  }
}
