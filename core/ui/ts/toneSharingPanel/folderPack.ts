/**
 * A pack started from a preset folder (right-click a folder in the preset
 * chooser → Create Pack…).
 *
 * A pack holds community items, and a local preset is not one until it has been
 * uploaded. Each preset in the folder is matched to one of the user's own items —
 * the item it was installed from, or else one with the same title — and the rest
 * are uploaded as draft items when the pack is saved. Drafts reach moderators only
 * when the pack is published, which submits them with it.
 *
 * Matching by title is what stops a second Create Pack on the same folder from
 * uploading every preset again.
 */

import { summarizeSignalChainIssues, validatePresetSignalChain } from "../signalPath/chainValidation.js";
import type { Preset } from "../types.js";
import { escapeHtml } from "../utils.js";
import { apiFetch } from "./api.js";
import { element } from "./dom.js";
import { createToneSharingPresetItem, hasPublishableSignalChain } from "./itemUpload.js";
import { getPresetToneSharingOrigin } from "./presetLinks.js";
import { toneSharingState } from "./state.js";
import type { ToneSharingItem } from "./types.js";

export type FolderPackEntry = {
  preset: Preset;
  /** The user's item this preset already is, or null until it is uploaded. */
  itemId: string | null;
  /** Why the preset cannot go in the pack, or null when it can. */
  blockedReason: string | null;
};

type FolderPackSession = {
  folderName: string;
  entries: FolderPackEntry[];
};

let session: FolderPackSession | null = null;

const ITEM_STATUS_RANK: Record<string, number> = { approved: 0, pending: 1, draft: 2 };

function normalizeTitle(value: string | null | undefined): string {
  return (value ?? "").trim().toLowerCase();
}

function isUsableItem(item: ToneSharingItem): boolean {
  return normalizeTitle(item.moderationStatus) !== "rejected";
}

/**
 * Pairs each preset with the user's item it already is. A preset installed from
 * someone else's item can't be re-uploaded as the user's own, so it is blocked.
 */
export function matchFolderPresetsToItems(presets: Preset[], myItems: ToneSharingItem[]): FolderPackEntry[] {
  const usable = myItems.filter(isUsableItem);
  const ownIds = new Set(usable.map((item) => item.id));
  const byTitle = new Map<string, ToneSharingItem>();
  for (const item of usable) {
    const key = normalizeTitle(item.title);
    const current = byTitle.get(key);
    const rank = ITEM_STATUS_RANK[normalizeTitle(item.moderationStatus)] ?? 1;
    const currentRank = current ? ITEM_STATUS_RANK[normalizeTitle(current.moderationStatus)] ?? 1 : Infinity;
    if (key && rank < currentRank) {
      byTitle.set(key, item);
    }
  }

  return presets.map((preset) => {
    const origin = getPresetToneSharingOrigin(preset);
    if (origin) {
      return ownIds.has(origin.itemId)
        ? { preset, itemId: origin.itemId, blockedReason: null }
        : { preset, itemId: null, blockedReason: "From Tone Sharing" };
    }
    const match = byTitle.get(normalizeTitle(preset.name || preset.id));
    return { preset, itemId: match?.id ?? null, blockedReason: null };
  });
}

/**
 * Loads the user's items fresh and matches the folder's presets against them.
 * The chooser's list holds summaries without the Tone Sharing origin, so each
 * preset is loaded in full first; one that fails to load is matched as listed.
 */
export async function startFolderPackSession(
  folderName: string,
  presets: Preset[],
  loadPreset: (presetId: string) => Promise<Preset>,
): Promise<void> {
  const [data, fullPresets] = await Promise.all([
    apiFetch<{ items: ToneSharingItem[] }>("/items/me/list"),
    Promise.all(presets.map((preset) => loadPreset(preset.id).catch(() => preset))),
  ]);
  toneSharingState.myItems = data.items;
  session = { folderName, entries: matchFolderPresetsToItems(fullPresets, data.items) };
}

export function getFolderPackSession(): { folderName: string; entries: readonly FolderPackEntry[] } | null {
  return session;
}

export function clearFolderPackSession(): void {
  session = null;
}

export function renderFolderPackSelection(): void {
  const host = element<HTMLElement>("tone-sharing-pack-items");
  if (!host || !session) {
    return;
  }
  if (!session.entries.length) {
    host.innerHTML = `<div class="tone-sharing-select-item">This folder has no presets.</div>`;
    return;
  }

  host.innerHTML = session.entries
    .map((entry) => {
      const hint = entry.blockedReason ?? (entry.itemId ? "On Tone Sharing" : "Uploads as a draft");
      const blocked = entry.blockedReason !== null;
      return `
        <label class="tone-sharing-select-item${blocked ? " is-blocked" : ""}">
          <input type="checkbox" data-pack-preset-id="${escapeHtml(entry.preset.id)}" ${blocked ? `disabled data-pack-blocked="true"` : "checked"} />
          <span>${escapeHtml(entry.preset.name || entry.preset.id)}</span>
          <span class="tone-sharing-select-item-hint">${escapeHtml(hint)}</span>
        </label>
      `;
    })
    .join("");
}

function checkedEntries(): FolderPackEntry[] {
  if (!session) {
    return [];
  }
  const checkedIds = new Set(
    Array.from(document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[data-pack-preset-id]:checked"))
      .map((input) => input.dataset.packPresetId ?? ""),
  );
  return session.entries.filter((entry) => !entry.blockedReason && checkedIds.has(entry.preset.id));
}

/** How many of the checked presets still need uploading. */
export function countFolderPackUploads(): number {
  return checkedEntries().filter((entry) => !entry.itemId).length;
}

/**
 * The item ids for the checked presets, uploading each one that has no item yet
 * as a draft. Every preset is checked before anything is uploaded, so one with an
 * empty chain stops the save instead of leaving half a folder uploaded. An upload
 * that succeeds is remembered, so retrying after a failure carries on from where
 * it stopped.
 */
export async function resolveFolderPackItemIds(onProgress: (message: string) => void): Promise<string[]> {
  if (!session) {
    return [];
  }
  const entries = checkedEntries();
  const pending = entries.filter((entry) => !entry.itemId);

  const unpublishable = pending.find((entry) => !hasPublishableSignalChain(entry.preset));
  if (unpublishable) {
    throw new Error(`"${unpublishable.preset.name || unpublishable.preset.id}" has no effects in its signal chain. Uncheck it to continue.`);
  }
  for (const entry of pending) {
    const chainProblem = summarizeSignalChainIssues(validatePresetSignalChain(entry.preset));
    if (chainProblem) {
      throw new Error(`"${entry.preset.name || entry.preset.id}" can't be published: ${chainProblem} Fix it or uncheck it to continue.`);
    }
  }

  let uploaded = 0;
  for (const entry of pending) {
    const preset = entry.preset;
    uploaded += 1;
    onProgress(`Uploading preset ${uploaded} of ${pending.length}: ${preset.name || preset.id}`);
    const item = await createToneSharingPresetItem(preset, {
      title: preset.name || preset.id,
      description: preset.description ?? "",
      tags: preset.tags ?? [],
    });
    entry.itemId = item.id;
    toneSharingState.myItems = [...toneSharingState.myItems, { ...item, moderationStatus: item.moderationStatus ?? "draft" }];
  }

  return entries.map((entry) => entry.itemId).filter((id): id is string => Boolean(id));
}
