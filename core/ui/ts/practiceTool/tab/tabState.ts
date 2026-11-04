/**
 * What the tab view has open, and the per-reference autosave of it.
 *
 * Module state rather than `uiState`: nothing outside the Practice Tool reads
 * it, and the only writers are the tab view's own modules. The score bytes are
 * not kept here (alphaTab holds the parsed score); only the binding — which
 * stored file, which track, and the sync points — and which reference it is
 * synced to.
 *
 * The autosave works like the per-file loop store: every reference (a backing
 * track by path, a YouTube video by id) remembers the tab last synced to it, so
 * reopening the track brings its tab back with no project involved.
 */

import { updateAppSetting } from "../../appSettingsStore.js";
import { uiState } from "../../state.js";
import type { AppSettingValue, PracticeToolTabBinding, PracticeToolYouTubeReference } from "../../types.js";
import { sanitizeSyncPoints } from "./syncPoints.js";

const TABS_SETTING_KEY = "practiceTool.tabs";
/** Oldest entries go first once the autosave holds this many references. */
const MAX_REMEMBERED_REFERENCES = 300;

export type ReferenceKind = "track" | "youtube";

let binding: PracticeToolTabBinding | null = null;
let referenceKind: ReferenceKind = "track";
let youtube: PracticeToolYouTubeReference | null = null;
const listeners = new Set<() => void>();

function notify(): void {
  for (const listener of listeners) {
    listener();
  }
}

/** Called whenever the binding, the reference kind or the video changes. */
export function onTabStateChange(listener: () => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function getTabBinding(): PracticeToolTabBinding | null {
  return binding;
}

export function getReferenceKind(): ReferenceKind {
  return referenceKind;
}

export function getYouTubeReference(): PracticeToolYouTubeReference | null {
  return youtube;
}

/** The autosave key of the current reference, or "" when there is none to sync to. */
export function getReferenceKey(): string {
  if (referenceKind === "youtube") {
    return youtube ? `yt:${youtube.videoId}` : "";
  }
  const path = uiState.practiceTool?.filePath ?? "";
  return path ? `track:${path.trim().toLowerCase()}` : "";
}

export function setReferenceKind(kind: ReferenceKind): void {
  if (kind !== referenceKind) {
    referenceKind = kind;
    notify();
  }
}

export function setYouTubeReference(reference: PracticeToolYouTubeReference | null): void {
  youtube = reference ? { ...reference } : null;
  notify();
}

/** Replaces the binding (null closes the tab) and autosaves it against the current reference. */
export function setTabBinding(next: PracticeToolTabBinding | null, options: { persist?: boolean } = {}): void {
  binding = next ? copyBinding(next) : null;
  if (options.persist !== false) {
    persistBindingForReference();
  }
  notify();
}

export function copyBinding(source: PracticeToolTabBinding): PracticeToolTabBinding {
  return {
    tabId: source.tabId,
    fileName: source.fileName,
    trackIndex: source.trackIndex,
    syncPoints: source.syncPoints.map((point) => ({ ...point })),
  };
}

/** Narrows stored data back to a binding, or null when it names no stored file. */
export function parseTabBinding(value: unknown): PracticeToolTabBinding | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return null;
  }
  const record = value as Record<string, unknown>;
  const tabId = typeof record.tabId === "string" ? record.tabId : "";
  if (!tabId) {
    return null;
  }
  const trackIndex = typeof record.trackIndex === "number" && Number.isInteger(record.trackIndex) && record.trackIndex >= 0
    ? record.trackIndex
    : 0;
  return {
    tabId,
    fileName: typeof record.fileName === "string" && record.fileName ? record.fileName : "Tablature",
    trackIndex,
    syncPoints: sanitizeSyncPoints(record.syncPoints),
  };
}

export function parseYouTubeReference(value: unknown): PracticeToolYouTubeReference | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return null;
  }
  const record = value as Record<string, unknown>;
  const videoId = typeof record.videoId === "string" && /^[A-Za-z0-9_-]{11}$/.test(record.videoId) ? record.videoId : "";
  if (!videoId) {
    return null;
  }
  return { videoId, title: typeof record.title === "string" ? record.title.slice(0, 300) : "" };
}

function readRememberedBindings(): Record<string, unknown> {
  const stored = uiState.appSettings?.[TABS_SETTING_KEY];
  return stored && typeof stored === "object" && !Array.isArray(stored)
    ? { ...(stored as unknown as Record<string, unknown>) }
    : {};
}

/** The tab last synced to `key`, if any. */
export function loadBindingForReference(key: string): PracticeToolTabBinding | null {
  return key ? parseTabBinding(readRememberedBindings()[key]) : null;
}

/** Records the current binding against the current reference (or forgets it when closed). */
export function persistBindingForReference(): void {
  const key = getReferenceKey();
  if (!key) {
    return;
  }
  const map = readRememberedBindings();
  delete map[key]; // re-inserted last, so the oldest stay first in line for trimming
  if (binding) {
    map[key] = copyBinding(binding);
  }
  const keys = Object.keys(map);
  for (const stale of keys.slice(0, Math.max(0, keys.length - MAX_REMEMBERED_REFERENCES))) {
    delete map[stale];
  }
  updateAppSetting(TABS_SETTING_KEY, map as unknown as AppSettingValue);
}
