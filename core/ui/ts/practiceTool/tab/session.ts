/**
 * The tab view's session: which score is open, which reference it follows, and
 * the rules for what happens to it when either changes.
 *
 * - Opening a score keeps the current sync points only if it is the same file.
 * - Every reference remembers the tab last synced to it (tabState.ts). Loading
 *   a backing track brings its tab back, or closes the tab if it has none — a
 *   new track is usually a new song. A YouTube video with nothing remembered
 *   keeps the open tab with no sync points, since a video is usually added to
 *   a tab already open.
 * - A project recall replaces all of it with what the project saved.
 *
 * The DOM is panel.ts's; this module calls `requestTabRender` when something
 * on screen should change.
 */

import { getApiBaseUrl } from "../../apiConfig.js";
import { requestPracticeToolTab } from "../../bridge.js";
import { appendLog } from "../../logging.js";
import { showNotification } from "../../notifications.js";
import type { PracticeToolProject, PracticeToolTabBinding, PracticeToolTabSyncPoint } from "../../types.js";
import { base64ToArrayBuffer } from "../../utils.js";
import { createTrackReference, type ReferenceSource } from "./reference.js";
import type { BarSlot } from "./syncPoints.js";
import { sortSyncPoints } from "./syncPoints.js";
import {
  getReferenceKey,
  getReferenceKind,
  getTabBinding,
  getYouTubeReference,
  loadBindingForReference,
  type ReferenceKind,
  setReferenceKind,
  setTabBinding,
  setYouTubeReference,
} from "./tabState.js";
import { applySyncPoints, closeScore, getPlaybackOrder, openScore, setTabReference, showTrack } from "./tabView.js";
import { createYouTubeReference } from "./youtubeReference.js";

let trackReference: ReferenceSource | null = null;
let youtubeReference: ReferenceSource | null = null;
let videoHost: HTMLElement | null = null;
/** The tab id whose bytes alphaTab has (or is parsing). */
let loadedTabId = "";
/** A binding waiting for its file to arrive from the engine. */
let pendingBinding: PracticeToolTabBinding | null = null;
let syncTarget: BarSlot | null = null;
let trackNames: string[] = [];
let scoreTitle = "";
let lastError = "";
let render: () => void = () => undefined;

export function setTabRenderer(fn: () => void): void {
  render = fn;
}

export function requestTabRender(): void {
  render();
}

export function setVideoHost(element: HTMLElement): void {
  videoHost = element;
}

export function getActiveReference(): ReferenceSource | null {
  return getReferenceKind() === "youtube" ? youtubeReference : (trackReference ??= createTrackReference());
}

export function getTrackNames(): readonly string[] {
  return trackNames;
}

export function getScoreTitle(): string {
  return scoreTitle;
}

export function getTabError(): string {
  return lastError;
}

export function getSyncTarget(): BarSlot | null {
  return syncTarget;
}

/** Whether a score's file is still on its way from the engine. */
export function isTabLoading(): boolean {
  return pendingBinding !== null;
}

function pushSync(): void {
  applySyncPoints(getTabBinding()?.syncPoints ?? [], syncTarget);
}

export function setSyncTarget(slot: BarSlot | null): void {
  syncTarget = slot;
  pushSync();
  render();
}

/** Replaces the sync points of the open tab and saves them against the reference. */
export function updateSyncPoints(points: PracticeToolTabSyncPoint[]): void {
  const binding = getTabBinding();
  if (!binding) {
    return;
  }
  setTabBinding({ ...binding, syncPoints: points });
  pushSync();
  render();
}

function connectReference(): void {
  setTabReference(getActiveReference());
}

/** Shows `binding`'s score, asking the engine for the file unless alphaTab already has it. */
function showBinding(binding: PracticeToolTabBinding | null, options: { persist?: boolean } = {}): void {
  if (!binding) {
    pendingBinding = null;
    loadedTabId = "";
    trackNames = [];
    scoreTitle = "";
    syncTarget = null;
    closeScore();
    setTabBinding(null, options);
    render();
    return;
  }
  lastError = "";
  if (binding.tabId === loadedTabId && !pendingBinding) {
    const trackChanged = getTabBinding()?.trackIndex !== binding.trackIndex;
    setTabBinding(binding, options);
    if (trackChanged) {
      showTrack(binding.trackIndex);
    }
    pushSync();
    render();
    return;
  }
  pendingBinding = binding;
  setTabBinding(binding, options);
  requestPracticeToolTab(binding.tabId);
  render();
}

/** The engine's answer with a score's bytes: a file just opened, or one asked for by id. */
export function onTabFileLoaded(payload: { id?: string; fileName?: string; data?: string }): void {
  const id = typeof payload.id === "string" ? payload.id : "";
  const data = typeof payload.data === "string" ? payload.data : "";
  if (!id || !data) {
    return;
  }
  let binding: PracticeToolTabBinding;
  if (pendingBinding?.tabId === id) {
    binding = pendingBinding;
  } else if (pendingBinding) {
    return; // an answer a later request has overtaken
  } else {
    // A file the user just opened: the same score keeps its sync and track.
    const current = getTabBinding();
    binding = current?.tabId === id
      ? current
      : { tabId: id, fileName: payload.fileName || "Tablature", trackIndex: 0, syncPoints: [] };
    setTabBinding(binding);
  }
  pendingBinding = null;
  loadedTabId = id;
  lastError = "";
  trackNames = [];
  syncTarget = null;
  void openScore(new Uint8Array(base64ToArrayBuffer(data)), binding.trackIndex)
    .catch((error: unknown) => onTabError(error instanceof Error ? error.message : String(error)));
  connectReference();
  appendLog(`practice tool tab opened ← ${binding.fileName}`);
  render();
}

export function onTabFileFailed(payload: { id?: string; error?: string }): void {
  if (!pendingBinding || pendingBinding.tabId !== payload.id) {
    return;
  }
  const fileName = pendingBinding.fileName;
  pendingBinding = null;
  lastError = `"${fileName}" is no longer in the library. Open the file again to bring it back.`;
  render();
}

/** alphaTab could not open the score. It keeps showing the one before, which is
 * not the file named in the toolbar, so that goes; the binding stays (its sync
 * points are not thrown away over one failed load), and Close forgets it. */
export function onTabError(message: string): void {
  closeScore();
  loadedTabId = "";
  trackNames = [];
  scoreTitle = "";
  syncTarget = null;
  lastError = message;
  render();
}

/** alphaTab has parsed the score: its tracks are known and the sync points can go in. */
export function onScoreParsed(tracks: string[], title: string): void {
  trackNames = tracks;
  scoreTitle = title;
  render();
}

/** alphaTab has built the score's timing: the sync points can be checked against it and go in. */
export function onScoreTimingReady(): void {
  const binding = getTabBinding();
  const order = getPlaybackOrder();
  if (binding && order.length > 0) {
    // Points for passes this score doesn't have (the file was edited) are dropped.
    const valid = sortSyncPoints(order, binding.syncPoints);
    if (valid.length !== binding.syncPoints.length) {
      setTabBinding({ ...binding, syncPoints: valid });
    }
  }
  pushSync();
  render();
}

export function closeTab(): void {
  showBinding(null);
}

export function selectTrack(trackIndex: number): void {
  const binding = getTabBinding();
  if (binding && trackIndex !== binding.trackIndex) {
    showBinding({ ...binding, trackIndex });
  }
}

/** After the reference changes: its remembered tab, or `fallback` for a synced tab without one. */
function restoreForReference(fallback: "close" | "keep-unsynced"): void {
  const remembered = loadBindingForReference(getReferenceKey());
  if (remembered) {
    showBinding(remembered, { persist: false });
    return;
  }
  const current = getTabBinding();
  // A tab with no sync yet was opened for whatever comes next, so it stays and
  // becomes this reference's; one synced to the previous reference closes,
  // unless the caller says to keep the score and start its sync afresh.
  if (!current || (fallback === "close" && current.syncPoints.length > 0)) {
    showBinding(null, { persist: false });
  } else {
    showBinding({ ...current, syncPoints: [] });
  }
}

/** A backing track finished loading. `recalled` is the project it was loaded for, if any. */
export function onTrackLoaded(recalled: PracticeToolProject | null): void {
  if (recalled) {
    applyProjectTab(recalled);
    return;
  }
  if (getReferenceKind() !== "track") {
    return; // the tab follows a video; the track is just playing along
  }
  restoreForReference("close");
  connectReference();
}

function openVideo(videoId: string, title: string, startSec = 0): void {
  youtubeReference?.dispose();
  youtubeReference = null;
  if (!videoHost) {
    return;
  }
  setYouTubeReference({ videoId, title });
  youtubeReference = createYouTubeReference(videoHost, videoId, {
    relayBase: getApiBaseUrl(),
    startSec,
    onTitle: (newTitle) => {
      const current = getYouTubeReference();
      if (current && current.videoId === videoId && current.title !== newTitle) {
        setYouTubeReference({ videoId, title: newTitle });
        render();
      }
    },
    onError: (message) => showNotification("YouTube video unavailable", message),
  });
}

/** Switches the reference between the backing track and the video. */
export function chooseReferenceKind(kind: ReferenceKind): void {
  if (kind === getReferenceKind()) {
    return;
  }
  setReferenceKind(kind);
  if (kind === "track") {
    youtubeReference?.dispose();
    youtubeReference = null;
    setYouTubeReference(null);
    restoreForReference("keep-unsynced");
  } else if (getYouTubeReference()) {
    restoreForReference("keep-unsynced");
  }
  connectReference();
  render();
}

/** Opens a video as the reference. */
export function useYouTubeVideo(videoId: string, startSec: number): void {
  setReferenceKind("youtube");
  openVideo(videoId, "", startSec);
  restoreForReference("keep-unsynced");
  connectReference();
  render();
}

/** Puts a recalled project's tab and reference in place. */
export function applyProjectTab(project: PracticeToolProject): void {
  if (project.youtube) {
    setReferenceKind("youtube");
    if (getYouTubeReference()?.videoId !== project.youtube.videoId || !youtubeReference) {
      openVideo(project.youtube.videoId, project.youtube.title);
    }
  } else {
    if (getReferenceKind() === "youtube") {
      youtubeReference?.dispose();
      youtubeReference = null;
      setYouTubeReference(null);
    }
    setReferenceKind("track");
  }
  // Saved against the reference too, so reopening it without the project brings this back.
  showBinding(project.tab ?? null);
  connectReference();
  render();
}

/** What a project saves of the tab view. */
export function captureProjectTab(): Pick<PracticeToolProject, "tab" | "youtube"> {
  const binding = getTabBinding();
  const video = getReferenceKind() === "youtube" ? getYouTubeReference() : null;
  return {
    ...(binding ? { tab: { ...binding, syncPoints: binding.syncPoints.map((point) => ({ ...point })) } } : {}),
    ...(video ? { youtube: { ...video } } : {}),
  };
}
