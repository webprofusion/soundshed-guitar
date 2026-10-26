/**
 * Share links in both directions: building one for an item or pack, and opening
 * the panel onto the target a link arrived with.
 */

import { getToneSharingHostActions } from "./hostActions.js";
import { showPrompt } from "../dialogs.js";
import { copyTextToClipboard } from "../utils.js";
import { getApiOrigin } from "./api.js";
import { loadBrowse } from "./browse.js";
import { setUploadStatus } from "./dom.js";
import { updateActiveSharedFilter } from "./feed.js";
import { clearPackDetail } from "./packDetail.js";
import { browseState } from "./state.js";
import type { ToneSharingShareTarget } from "./types.js";

export function buildToneSharingShareLink(target: ToneSharingShareTarget): string {
  const kindPath = target.kind === "pack" ? "pack" : "item";
  return `${getApiOrigin()}/share/${kindPath}/${encodeURIComponent(target.id)}`;
}

export async function copyToneSharingShareLink(target: ToneSharingShareTarget): Promise<string> {
  const link = buildToneSharingShareLink(target);
  try {
    await copyTextToClipboard(link);
    return link;
  } catch {
    const promptResult = await showPrompt("Copy this link by hand:", link, { title: "Tone Sharing Link" });
    if (promptResult === null) {
      throw new Error("Copy cancelled");
    }
    return link;
  }
}

export async function clearActiveSharedFilter(): Promise<void> {
  if (!browseState.activeSharedTarget) {
    return;
  }

  browseState.activeSharedTarget = null;
  updateActiveSharedFilter();
  clearPackDetail();
  await loadBrowse();
}

export function parseShareTargetFromLocation(): ToneSharingShareTarget | null {
  let url: URL;
  try {
    url = new URL(window.location.href);
  } catch {
    return null;
  }

  const itemId = (url.searchParams.get("itemId") ?? "").trim();
  if (itemId) {
    return { kind: "item", id: itemId };
  }

  const packId = (url.searchParams.get("packId") ?? "").trim();
  if (packId) {
    return { kind: "pack", id: packId };
  }

  const match = url.pathname.match(/\/share\/(item|pack)\/([^/?#]+)/i);
  if (match) {
    const kind = match[1].toLowerCase() === "pack" ? "pack" : "item";
    return { kind, id: decodeURIComponent(match[2]) };
  }

  return null;
}

export async function openSharedTargetFromLocation(): Promise<void> {
  const target = parseShareTargetFromLocation();
  if (!target) {
    return;
  }

  browseState.activeSharedTarget = target;
  updateActiveSharedFilter();
  getToneSharingHostActions().switchMainPanel("sharing");
  browseState.mode = target.kind === "pack" ? "packs" : "items";
  await loadBrowse();
}

export function handleToneSharingDeepLink(deepLinkQuery: string): void {
  // Navigate to the Tone Sharing panel and show the specific item/pack so
  // the user can preview, download, etc. rather than auto-importing.
  const params = new URLSearchParams(deepLinkQuery);
  const itemId = params.get("itemId");
  const packId = params.get("packId");

  if (!itemId && !packId) {
    return;
  }

  void (async () => {
    try {
      browseState.activeSharedTarget = itemId ? { kind: "item", id: itemId } : { kind: "pack", id: packId ?? "" };
      updateActiveSharedFilter();
      getToneSharingHostActions().switchMainPanel("sharing");
      browseState.mode = itemId ? "items" : "packs";
      await loadBrowse();
    } catch (error) {
      setUploadStatus(`Failed to load shared tone: ${(error as Error).message}`);
    }
  })();
}
