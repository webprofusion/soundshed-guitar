/**
 * Uploading a preset as a community item: the asset upload both its archives
 * go through, and the item record that ties them together.
 *
 * Creating an item does not submit it. Until `/items/{id}/publish` is called it
 * stays a draft only its creator sees, so a pack can gather drafts and submit
 * them with itself.
 *
 * An item remembers the local preset it was published from (`sourcePresetId`), so
 * publishing that preset again updates the item instead of making a second one.
 * Items from before that have no key; they are matched by title, and the first
 * update gives them one.
 */

import { clonePreset } from "../state.js";
import type { Preset } from "../types.js";
import { apiFetch, toneSharingFetch } from "./api.js";
import { getToneSharingHostActions } from "./hostActions.js";
import type { ToneSharingItem } from "./types.js";

/** Uploads one blob as an asset of the given kind and returns its id. */
export async function uploadToneSharingAsset(kind: "item_payload" | "thumbnail", blob: Blob): Promise<string> {
  const mimeType = blob.type || "application/octet-stream";
  const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ kind, mimeType, byteSize: blob.size })
  });

  const uploadResponse = await toneSharingFetch(`/uploads/${init.uploadId}`, {
    method: "PUT",
    headers: { "content-type": mimeType },
    body: blob,
  });
  const uploadPayload = await uploadResponse.json().catch(() => null);
  if (!uploadResponse.ok || uploadPayload?.ok === false) {
    throw new Error(uploadPayload?.error?.message || `Upload failed (${uploadResponse.status})`);
  }

  const complete = await apiFetch<{ assetId: string }>("/uploads/complete", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ uploadId: init.uploadId })
  });
  return complete.assetId;
}

/** True when the preset has the chain a community item needs: input, output and an effect between. */
export function hasPublishableSignalChain(preset: Preset): boolean {
  const graphNodes = preset.graph?.nodes ?? [];
  const hasInput = graphNodes.some((n) => n.type === "input");
  const hasOutput = graphNodes.some((n) => n.type === "output");
  const hasEffect = graphNodes.some((n) => n.type !== "input" && n.type !== "output");
  return hasInput && hasOutput && hasEffect;
}

/** Builds the preset's public and private archives and uploads both; returns their asset ids. */
export async function uploadPresetArchives(preset: Preset): Promise<{ payloadAssetId: string; privatePayloadAssetId: string }> {
  const archiveBlobs = await getToneSharingHostActions().buildToneSharingPresetArchiveBlobs(clonePreset(preset));
  const payloadAssetId = await uploadToneSharingAsset("item_payload", archiveBlobs.publicBlob);
  const privatePayloadAssetId = await uploadToneSharingAsset("item_payload", archiveBlobs.privateBlob);
  return { payloadAssetId, privatePayloadAssetId };
}

export type PresetItemMeta = { title: string; description: string; tags?: string[] };

/**
 * Uploads the preset's public and private archives and creates an item for them, keyed to the
 * preset. When the creator already has an item for this preset (published from another device,
 * say), the server updates that one instead. The item is left as it was, or a draft when new; the
 * caller decides whether to submit it.
 */
export async function createToneSharingPresetItem(preset: Preset, meta: PresetItemMeta): Promise<ToneSharingItem & { updatedExisting: boolean }> {
  const { payloadAssetId, privatePayloadAssetId } = await uploadPresetArchives(preset);

  const result = await apiFetch<{ item: ToneSharingItem; updated?: boolean }>("/items", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      type: "preset",
      title: meta.title,
      description: meta.description,
      tags: meta.tags && meta.tags.length > 0 ? meta.tags : undefined,
      payloadAssetId,
      privatePayloadAssetId,
      sourcePresetId: preset.id,
    })
  });
  return { ...result.item, updatedExisting: result.updated === true };
}

/** Uploads the preset's archives again and puts them, and its details, on the user's existing item. */
export async function updateToneSharingPresetItem(itemId: string, preset: Preset, meta: PresetItemMeta): Promise<ToneSharingItem> {
  const { payloadAssetId, privatePayloadAssetId } = await uploadPresetArchives(preset);
  const result = await apiFetch<{ item: ToneSharingItem }>(`/items/${encodeURIComponent(itemId)}`, {
    method: "PATCH",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      title: meta.title,
      description: meta.description,
      tags: meta.tags ?? [],
      payloadAssetId,
      privatePayloadAssetId,
      sourcePresetId: preset.id,
    })
  });
  return result.item;
}

export type PublishedItemMatch = {
  item: ToneSharingItem;
  /** "preset": published from this very preset. "title": an older item with the same name and no key, a guess to confirm. */
  match: "preset" | "title";
};

const TITLE_MATCH_RANK: Record<string, number> = { approved: 0, pending_review: 1, draft: 2, rejected: 3 };

function normalizeTitle(value: string | null | undefined): string {
  return (value ?? "").trim().toLowerCase();
}

/**
 * The user's item this preset is already published as: the one keyed to it, or else an older
 * unkeyed item with the same title (live ones first). `includeRejected` false leaves rejected
 * items out of the title guess.
 */
export function findPublishedItemForPreset(
  preset: Preset,
  myItems: readonly ToneSharingItem[],
  options: { includeRejected?: boolean } = {},
): PublishedItemMatch | null {
  const keyed = myItems.find((item) => item.sourcePresetId === preset.id);
  if (keyed) {
    return { item: keyed, match: "preset" };
  }
  const title = normalizeTitle(preset.name || preset.id);
  if (!title) {
    return null;
  }
  const candidates = myItems
    .filter((item) => !item.sourcePresetId && normalizeTitle(item.title) === title)
    .filter((item) => options.includeRejected !== false || item.moderationStatus !== "rejected")
    .sort((a, b) => (TITLE_MATCH_RANK[a.moderationStatus ?? ""] ?? 1) - (TITLE_MATCH_RANK[b.moderationStatus ?? ""] ?? 1));
  return candidates[0] ? { item: candidates[0], match: "title" } : null;
}
