/**
 * The Folder tab's "Find on Tone3000": a file in a folder looked up on Tone3000 by its content
 * hash, as the Library tab looks up a row (tone3000ContentHash.ts).
 *
 * A file in the library already has its hash on its row. One that is not asks the engine to hash
 * it (`hashResourceFile`, answered by `resourceFileContentHash`): the folder scan does not hash
 * every file, since a lookup is rare and hashing a .nam means parsing it again.
 *
 * What a match finds is kept for the session by path, so the details panel shows it after a
 * rescan, and is written into the library entry when the file is in the library, or carried into
 * the import when the file is added later (`folderFileTone3000Metadata`).
 *
 * "Look up details" (`lookUpFolderFiles`) does the same for the listed files, ten at a time; the
 * Folder tab then groups the files that share a tone (folderGroups.ts).
 */

import { postMessage } from "../bridge.js";
import { showNotification } from "../notifications.js";
import type { IncomingPayload } from "../messages/types.js";
import { uiState } from "../state.js";
import { isContentHash } from "../tone3000.js";
import type { Tone3000Tone } from "../tone3000ApiTypes.js";
import type { Tone3000ContentHashMatch } from "../tone3000ContentHash.js";
import { applyTone3000Match, findTone3000ByHash, withTone3000Match } from "../tone3000ContentHash.js";
import { findResourceById } from "../utils.js";
import { folderFileLibraryMatch } from "./folderRows.js";
import type { FolderListingFile } from "./types.js";

const HASH_TIMEOUT_MS = 30_000;

const matchesByPath = new Map<string, Record<string, string>>();
/** Files looked up this session, found or not, so a batch does not ask about them again. */
const lookedUpPaths = new Set<string>();
const hashesByPath = new Map<string, string>();
const pendingHashes = new Map<string, Array<(hash: string) => void>>();

function pathKey(path: string): string {
  return path.replace(/\\/g, "/").toLowerCase();
}

/** The Tone3000 details a lookup found for the file this session, or undefined. */
export function folderFileTone3000Metadata(path: string): Record<string, string> | undefined {
  return matchesByPath.get(pathKey(path));
}

/** The Folder tab offers a lookup for a NAM capture or a WAV IR, the files Tone3000 hashes. */
export function canLookUpFolderFile(file: FolderListingFile): boolean {
  return file.resourceType === "nam" || /\.wav$/i.test(file.name);
}

/** `resourceFileContentHash`: the engine's answer to `hashResourceFile`. */
export function onResourceFileContentHash(payload: IncomingPayload): void {
  const message = payload as { path?: string; contentHash?: string };
  const key = pathKey(message.path ?? "");
  const hash = isContentHash(message.contentHash) ? message.contentHash : "";
  if (hash) {
    hashesByPath.set(key, hash);
  }
  const waiting = pendingHashes.get(key) ?? [];
  pendingHashes.delete(key);
  waiting.forEach((resolve) => resolve(hash));
}

/** The file's content hash: its library row's, one already worked out, or the engine's. "" when it has none. */
function contentHashOf(file: FolderListingFile): Promise<string> {
  const match = folderFileLibraryMatch(file);
  const row = match.id ? findResourceById(uiState.resourceLibrary[file.resourceType] ?? [], match.id) : undefined;
  const known = row?.metadata?.contentHash ?? file.metadata?.contentHash ?? hashesByPath.get(pathKey(file.path));
  if (isContentHash(known)) {
    return Promise.resolve(known);
  }
  const key = pathKey(file.path);
  return new Promise((resolve) => {
    const waiting = pendingHashes.get(key);
    if (waiting) {
      waiting.push(resolve);
      return;
    }
    pendingHashes.set(key, [resolve]);
    postMessage({ type: "hashResourceFile", path: file.path, resourceType: file.resourceType });
    setTimeout(() => {
      const timedOut = pendingHashes.get(key);
      if (timedOut) {
        pendingHashes.delete(key);
        timedOut.forEach((done) => done(""));
      }
    }, HASH_TIMEOUT_MS);
  });
}

/** What one lookup came to. */
export type FolderLookupResult =
  | { kind: "found"; match: Tone3000ContentHashMatch }
  | { kind: "none" }
  | { kind: "unreadable" };

/**
 * Looks the file up by its content hash and keeps what it finds: on the library row when the file
 * is in the library, and for the session by path either way. A file found or not found is not
 * looked up again by a batch this session. Throws when the lookup itself fails (no network).
 */
async function lookUpFolderFile(file: FolderListingFile, toneCache?: Map<string, Tone3000Tone | null>): Promise<FolderLookupResult> {
  const contentHash = await contentHashOf(file);
  if (!contentHash) {
    // Not offered again by a batch either: it would fail the same way.
    lookedUpPaths.add(pathKey(file.path));
    return { kind: "unreadable" };
  }
  const libraryMatch = folderFileLibraryMatch(file);
  const row = libraryMatch.id ? findResourceById(uiState.resourceLibrary[file.resourceType] ?? [], libraryMatch.id) : undefined;
  const match = await findTone3000ByHash(contentHash, row?.metadata?.toneId, toneCache);
  lookedUpPaths.add(pathKey(file.path));
  if (!match) {
    return { kind: "none" };
  }
  matchesByPath.set(pathKey(file.path), withTone3000Match({ contentHash }, match));
  if (row) {
    applyTone3000Match(file.resourceType, row, match);
  }
  return { kind: "found", match };
}

/**
 * Looks the file up on Tone3000 by its content hash (the details panel's "Find on Tone3000").
 * What happened goes in a notification; `done` redraws the list.
 */
export async function lookUpFolderFileOnTone3000(button: HTMLButtonElement, file: FolderListingFile, done: () => void): Promise<void> {
  if (button.disabled) {
    return;
  }
  button.disabled = true;
  button.textContent = "Looking on Tone3000…";
  const what = file.resourceType === "ir" ? "samples" : "weights";
  try {
    const result = await lookUpFolderFile(file);
    if (result.kind === "unreadable") {
      showNotification("Tone3000 lookup unavailable", `"${file.name}" could not be read to work out its ${what}`);
      return;
    }
    if (result.kind === "none") {
      showNotification("Not on Tone3000", `no Tone3000 model has the same ${what} as "${file.name}"`);
      return;
    }
    const { match } = result;
    const creator = match.tone.user?.username ? ` by ${match.tone.user.username}` : "";
    const reposts = match.others > 0 ? ` (${match.others} other upload${match.others === 1 ? "" : "s"} share it)` : "";
    showNotification("Found on Tone3000", `"${match.tone.title}"${creator}${reposts}`);
  } catch (error) {
    showNotification("Tone3000 lookup failed", error instanceof Error ? error.message : String(error));
  } finally {
    done();
  }
}

/** The most files one "Look up details" looks up; the rest wait for the next click. */
export const FOLDER_LOOKUP_BATCH = 10;

/**
 * Whether the batch would look this file up: one whose content can be looked up, that has no
 * tone details yet (its own lookup's, or its library entry's), and that was not looked up already
 * this session.
 */
export function needsFolderLookup(file: FolderListingFile): boolean {
  if (!canLookUpFolderFile(file) || lookedUpPaths.has(pathKey(file.path)) || matchesByPath.has(pathKey(file.path))) {
    return false;
  }
  const libraryMatch = folderFileLibraryMatch(file);
  const row = libraryMatch.id ? findResourceById(uiState.resourceLibrary[file.resourceType] ?? [], libraryMatch.id) : undefined;
  return !row?.metadata?.toneId;
}

let batchRunning = false;

export function isFolderLookupRunning(): boolean {
  return batchRunning;
}

/**
 * The Folder tab's "Look up details": the first FOLDER_LOOKUP_BATCH of `files` that still need
 * it, one after another. Each file found calls `onProgress` so the list regroups as it goes (a
 * later file of a tone already shown joins that tone's group). One notification sums it up.
 */
export async function lookUpFolderFiles(
  files: FolderListingFile[],
  onProgress: (done: number, total: number) => void,
): Promise<{ found: number; looked: number; remaining: number }> {
  const wanted = files.filter(needsFolderLookup);
  const batch = wanted.slice(0, FOLDER_LOOKUP_BATCH);
  const remaining = wanted.length - batch.length;
  if (!batch.length || batchRunning) {
    return { found: 0, looked: 0, remaining };
  }
  batchRunning = true;
  const toneCache = new Map<string, Tone3000Tone | null>();
  let found = 0;
  let looked = 0;
  let failure = "";
  try {
    onProgress(0, batch.length);
    for (const file of batch) {
      try {
        const result = await lookUpFolderFile(file, toneCache);
        if (result.kind === "found") {
          found += 1;
        }
      } catch (error) {
        // No network, or the service refused: stop rather than fail the same way ten times.
        failure = error instanceof Error ? error.message : String(error);
        break;
      }
      looked += 1;
      onProgress(looked, batch.length);
    }
  } finally {
    batchRunning = false;
  }
  if (failure) {
    showNotification("Lookup failed", failure);
  } else {
    const more = remaining > 0 ? ` Click again for the next ${Math.min(remaining, FOLDER_LOOKUP_BATCH)}.` : "";
    showNotification(
      found ? "Details found" : "No details found",
      `${found} of ${looked} file${looked === 1 ? "" : "s"} matched a published tone.${more}`,
    );
  }
  return { found, looked, remaining: remaining + (batch.length - looked) };
}

/** Test hook: forgets every match and hash. */
export function resetFolderTone3000ForTests(): void {
  matchesByPath.clear();
  hashesByPath.clear();
  pendingHashes.clear();
  lookedUpPaths.clear();
  batchRunning = false;
}
