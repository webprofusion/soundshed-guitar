/**
 * Acting on someone else's published tone: installing it, deleting your own, and
 * the moderation verdicts an admin can issue.
 */

import { showPrompt } from "../dialogs.js";
import { uiState } from "../state.js";
import { apiFetch, parseApiErrorMessage, toneSharingFetch } from "./api.js";
import { buildPackArchiveFromDetails } from "./archive.js";
import { resolveCreatorProfileHandle } from "./format.js";
import { getToneSharingHostActions } from "./hostActions.js";
import { requestBrowseRefreshAfterInstall } from "./refresh.js";
import { toneSharingState } from "./state.js";
import type { ToneSharingItem, ToneSharingPack, ToneSharingPackDetails } from "./types.js";

export async function moderateTarget(kind: "item" | "pack", id: string, action: "approve" | "reject"): Promise<void> {
  // Cancel still rejects, without a reason, as it did when this was window.prompt().
  const notes = action === "reject"
    ? ((await showPrompt("Optional rejection reason", "", { title: "Reject" })) ?? "").trim()
    : "";
  const endpoint = kind === "item" ? `/items/${id}/moderate` : `/packs/${id}/moderate`;
  await apiFetch(endpoint, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ action, notes: notes || undefined }),
  });
}

function hasInstallStateChanged(beforeInstalledSnapshot: string, beforePresetCount: number): boolean {
  return beforePresetCount !== uiState.presets.length || beforeInstalledSnapshot !== JSON.stringify(toneSharingState.installedPacks);
}

export async function downloadAsset(kind: "item" | "pack", id: string): Promise<void> {
  const installedSnapshotBefore = JSON.stringify(toneSharingState.installedPacks);
  const presetCountBefore = uiState.presets.length;
  const path = kind === "item" ? `/items/${id}/download` : `/packs/${id}/download`;
  const response = await toneSharingFetch(path);

  if (!response.ok && !(kind === "pack" && response.status === 409)) {
    const message = await parseApiErrorMessage(response);
    throw new Error(message);
  }

  const disposition = response.headers.get("content-disposition") || "";
  const fileMatch = disposition.match(/filename="?([^"]+)"?/i);
  const fileName = fileMatch?.[1] || `${kind}-${id}${kind === "pack" ? ".zip" : ""}`;

  if (kind === "pack") {
    let packMeta: ToneSharingPack | null = null;
    try {
      const packResponse = await apiFetch<{ pack: ToneSharingPack }>(`/packs/${id}`);
      packMeta = packResponse.pack;
    } catch {
      packMeta = null;
    }

    let importBlob: Blob;
    let importFileName = fileName;
    if (response.ok) {
      importBlob = await response.blob();
    } else {
      const details = await apiFetch<ToneSharingPackDetails>(`/packs/${id}`);
      const synthesized = await buildPackArchiveFromDetails(details);
      importBlob = synthesized.blob;
      importFileName = synthesized.fileName;
    }

    const importFile = new File([importBlob], importFileName, { type: importBlob.type || "application/zip" });
    await getToneSharingHostActions().importPackWithConfirmation(importFile, {
      source: "toneSharingApi",
      packId: id,
      creatorId: packMeta?.creatorUserId ?? undefined,
      creatorHandle: resolveCreatorProfileHandle((packMeta ?? {}) as unknown as Record<string, unknown>) ?? undefined,
      titleHint: packMeta?.title ?? importFileName.replace(/\.zip$/i, ""),
    });

    if (hasInstallStateChanged(installedSnapshotBefore, presetCountBefore)) {
      await requestBrowseRefreshAfterInstall();
    }
    return;
  }

  const blob = await response.blob();

  let itemMeta: ToneSharingItem | null = null;
  try {
    const itemResponse = await apiFetch<{ item: ToneSharingItem }>(`/items/${id}`);
    itemMeta = itemResponse.item;
  } catch {
    itemMeta = null;
  }

  // Import the preset (and bundled resources) directly with the preset-archive
  // pipeline so failures propagate to the caller with a clear status message.
  const importFile = new File([blob], fileName, { type: blob.type || "application/octet-stream" });
  const importedPresets = await getToneSharingHostActions().importPresetArchive(importFile, {
    source: "toneSharingApi",
    itemId: id,
    creatorId: itemMeta?.creatorUserId ?? undefined,
    creatorHandle: resolveCreatorProfileHandle((itemMeta ?? {}) as unknown as Record<string, unknown>) ?? undefined,
    titleHint: itemMeta?.title ?? fileName.replace(/\.(soundshed\.preset|soundshed\.presets|preset|zip)$/i, ""),
  });

  if (importedPresets.length === 0) {
    throw new Error("Downloaded preset archive contained no importable presets");
  }

  if (hasInstallStateChanged(installedSnapshotBefore, presetCountBefore)) {
    await requestBrowseRefreshAfterInstall();
  }
}

export async function deleteAsset(kind: "item" | "pack", id: string): Promise<void> {
  const path = kind === "item" ? `/items/${id}` : `/packs/${id}`;
  await apiFetch(path, { method: "DELETE" });
}
