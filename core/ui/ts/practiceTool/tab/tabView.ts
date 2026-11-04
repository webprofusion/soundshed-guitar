/**
 * The tab view: one alphaTab instance rendering the open score, its cursor
 * driven by the reference (practiceTool/tab/reference.ts).
 *
 * alphaTab runs in its "external media" player mode. It makes no sound: the
 * reference does, and this module tells alphaTab where the reference is —
 * `updatePosition` every animation frame while it plays, once on each pause or
 * seek — and alphaTab maps that time onto the score through the sync points.
 * In the other direction alphaTab asks the reference to seek when a beat is
 * clicked, through the handler below.
 *
 * alphaTab also pauses and rewinds its output by itself when the score ends,
 * which would stop and rewind a backing track that runs on past the last bar.
 * So its play/pause/seek requests reach the reference only within a moment of
 * the user pressing on the score; everything else it asks for is ignored.
 */

import type { AlphaTabApi, model, synth } from "@coderline/alphatab";
import type { PracticeToolTabSyncPoint } from "../types.js";
import { type AlphaTabNamespace, alphaTabVendorUrl, loadAlphaTab } from "./alphaTabLoader.js";
import type { ReferenceSource } from "./reference.js";
import { type BarSlot, buildPlaybackOrder, slotForBarNearTick } from "./syncPoints.js";

export type StaveChoice = "tab" | "scoretab" | "score";

export interface TabViewCallbacks {
  /** The score parsed: its track names and title. */
  onScoreLoaded: (tracks: string[], title: string) => void;
  /** The score's timing is built: `getPlaybackOrder()` now describes this score
   * (until then it is empty, or still the previous score's). */
  onTimingReady: () => void;
  onRendered: () => void;
  onError: (message: string) => void;
  /** A bar pass clicked while sync editing is on (no seek happens then). */
  onBarPicked: (slot: BarSlot) => void;
}

/** How long after a press on the score alphaTab's seeks are taken as the user's. */
const USER_GESTURE_WINDOW_MS = 1500;

let namespace: AlphaTabNamespace | null = null;
let api: AlphaTabApi | null = null;
let surface: HTMLElement | null = null;
let markerLayer: HTMLElement | null = null;
let callbacks: TabViewCallbacks | null = null;
let reference: ReferenceSource | null = null;
let unsubscribeReference: (() => void) | null = null;
let referenceWasPlaying = false;
let frame: number | null = null;
let gestureUntil = 0;
let syncEditing = false;
let pendingTrackIndex = 0;
let markers: { points: PracticeToolTabSyncPoint[]; target: BarSlot | null } = { points: [], target: null };
let isVisible: () => boolean = () => true;
let staveChoice: StaveChoice = "tab";

function staveProfile(at: AlphaTabNamespace, choice: StaveChoice): number {
  return choice === "scoretab" ? at.StaveProfile.ScoreTab : choice === "score" ? at.StaveProfile.Score : at.StaveProfile.Tab;
}

function externalOutput(): synth.IExternalMediaSynthOutput | null {
  return (api?.player?.output as synth.IExternalMediaSynthOutput | undefined) ?? null;
}

function alphaTabIsPlaying(): boolean {
  return Boolean(api && namespace && api.playerState === namespace.synth.PlayerState.Playing);
}

function userIsActing(): boolean {
  return performance.now() < gestureUntil;
}

const mediaHandler: synth.IExternalMediaHandler = {
  get backingTrackDuration() {
    return reference?.isAvailable() ? reference.durationMs() : 0;
  },
  // Speed and volume belong to the reference's own controls (the faders, or
  // YouTube's player); alphaTab's are left at 1 so its time is media time.
  get playbackRate() {
    return 1;
  },
  set playbackRate(_value: number) {},
  get masterVolume() {
    return 1;
  },
  set masterVolume(_value: number) {},
  seekTo(time: number) {
    if (reference && userIsActing() && !syncEditing) {
      reference.seekMs(time);
    }
  },
  play() {
    if (reference && userIsActing()) {
      reference.play();
    }
  },
  pause() {
    if (reference && userIsActing()) {
      reference.pause();
    }
  },
};

/** Tells alphaTab where the reference is now. */
function pushPosition(): void {
  const output = externalOutput();
  if (output && reference?.isAvailable() && api?.score) {
    output.updatePosition(reference.positionMs());
  }
}

function stopFollowing(): void {
  if (frame !== null) {
    cancelAnimationFrame(frame);
    frame = null;
  }
}

function startFollowing(): void {
  if (frame !== null) {
    return;
  }
  const step = () => {
    if (!reference?.isPlaying()) {
      frame = null;
      return;
    }
    // Off screen (another panel, another Jam section) the frame is skipped but
    // the loop stays alive, so the cursor is right the moment it is shown.
    if (isVisible()) {
      pushPosition();
    }
    frame = requestAnimationFrame(step);
  };
  frame = requestAnimationFrame(step);
}

/** A play, pause, seek or load on the reference. */
function onReferenceChange(): void {
  const playing = Boolean(reference?.isPlaying());
  if (playing !== referenceWasPlaying && api?.score) {
    // Mirrors the reference into alphaTab's own state, which runs its cursor
    // animation. Its play() calls back into the handler, which ignores it (no
    // user gesture), so this cannot loop back to the reference.
    if (playing && !alphaTabIsPlaying()) {
      api.play();
    } else if (!playing && alphaTabIsPlaying()) {
      api.pause();
    }
  }
  referenceWasPlaying = playing;
  // After the mirroring: alphaTab's pause() leaves its time at the start of the
  // beat it stopped in, which would park the cursor up to a beat behind.
  pushPosition();
  if (playing) {
    startFollowing();
  } else {
    stopFollowing();
  }
}

/** Puts a flag on each synced bar, and outlines the bar the next tap sets. */
function drawMarkers(): void {
  if (!markerLayer) {
    return;
  }
  markerLayer.replaceChildren();
  const lookup = api?.boundsLookup;
  if (!lookup) {
    return;
  }
  for (const point of markers.points) {
    const bounds = lookup.findMasterBarByIndex(point.barIndex)?.visualBounds;
    if (!bounds) {
      continue;
    }
    const flag = document.createElement("div");
    flag.className = "practice-tool-tab-sync-flag";
    flag.style.left = `${bounds.x}px`;
    flag.style.top = `${bounds.y}px`;
    flag.style.height = `${bounds.h}px`;
    if (point.barOccurence > 0) {
      flag.dataset.pass = String(point.barOccurence + 1);
    }
    markerLayer.appendChild(flag);
  }
  if (markers.target && syncEditing) {
    const bounds = lookup.findMasterBarByIndex(markers.target.barIndex)?.visualBounds;
    if (bounds) {
      const box = document.createElement("div");
      box.className = "practice-tool-tab-sync-target";
      box.style.left = `${bounds.x}px`;
      box.style.top = `${bounds.y}px`;
      box.style.width = `${bounds.w}px`;
      box.style.height = `${bounds.h}px`;
      markerLayer.appendChild(box);
    }
  }
}

async function ensureApi(): Promise<AlphaTabApi> {
  if (api) {
    return api;
  }
  if (!surface || !callbacks) {
    throw new Error("The tab view is not mounted");
  }
  namespace = await loadAlphaTab();
  const at = namespace;
  const viewport = surface.parentElement ?? surface;
  const created = new at.AlphaTabApi(surface, {
    core: {
      scriptFile: alphaTabVendorUrl("alphaTab.min.js"),
      // Only the web formats are vendored (scripts/copy-vendor.js).
      smuflFontSources: new Map([
        [at.FontFileFormat.Woff2, alphaTabVendorUrl("font/Bravura.woff2")],
        [at.FontFileFormat.Woff, alphaTabVendorUrl("font/Bravura.woff")],
      ]),
      logLevel: at.LogLevel.Warning,
      includeNoteBounds: false,
    },
    display: {
      layoutMode: at.LayoutMode.Page,
      staveProfile: staveProfile(at, staveChoice),
      scale: 0.9,
    },
    player: {
      playerMode: at.PlayerMode.EnabledExternalMedia,
      enableCursor: true,
      enableAnimatedBeatCursor: true,
      enableUserInteraction: true,
      enablePlaybackRangeSelection: false,
      scrollElement: viewport,
      scrollMode: at.ScrollMode.Continuous,
    },
  });
  api = created;

  created.scoreLoaded.on((score) => {
    // load() was asked for the remembered track; a file that has fewer tracks
    // now (edited since) falls back to its first.
    if (pendingTrackIndex >= score.tracks.length && score.tracks[0]) {
      created.renderTracks([score.tracks[0]]);
    }
    callbacks?.onScoreLoaded(score.tracks.map((track) => track.name || `Track ${track.index + 1}`), score.title);
  });
  created.midiLoaded.on(() => {
    // Fired from inside alphaTab's loadMidiForScore, which afterwards hands the
    // player the sync points the score had before this; a microtask runs once it
    // has returned, so the points pushed here are the ones that stay.
    queueMicrotask(() => {
      if (api === created) {
        callbacks?.onTimingReady();
        pushPosition();
      }
    });
  });
  // Not renderFinished: with the render worker, the bounds the flags are placed
  // by only arrive with postRenderFinished.
  created.postRenderFinished.on(() => {
    drawMarkers();
    callbacks?.onRendered();
  });
  created.error.on((error) => {
    callbacks?.onError(error?.message ? error.message : "The file could not be read as tablature");
  });
  created.playerReady.on(() => {
    const output = externalOutput();
    if (output) {
      output.handler = mediaHandler;
    }
    referenceWasPlaying = false;
    onReferenceChange();
  });
  created.beatMouseDown.on((beat: model.Beat) => {
    if (!syncEditing || !created.tickCache) {
      return;
    }
    const order = buildPlaybackOrder(created.tickCache.masterBars);
    const slot = slotForBarNearTick(order, beat.voice.bar.masterBar.index, created.tickPosition);
    if (slot) {
      callbacks?.onBarPicked(slot);
    }
  });

  markerLayer = document.createElement("div");
  markerLayer.className = "practice-tool-tab-sync-layer";
  surface.appendChild(markerLayer);
  return created;
}

/** Binds the view to its element. `visible` says whether it is on screen now. */
export function mountTabView(element: HTMLElement, viewCallbacks: TabViewCallbacks, visible: () => boolean): void {
  surface = element;
  callbacks = viewCallbacks;
  isVisible = visible;
  // Capture phase: alphaTab handles the press itself and may stop it.
  (element.parentElement ?? element).addEventListener("pointerdown", () => {
    gestureUntil = performance.now() + USER_GESTURE_WINDOW_MS;
  }, { capture: true });
}

/** Parses and renders a score file. Errors come back through `onError`. */
export async function openScore(bytes: Uint8Array, trackIndex: number): Promise<void> {
  const created = await ensureApi();
  pendingTrackIndex = trackIndex;
  if (!created.load(bytes, [trackIndex])) {
    callbacks?.onError("The file could not be read as tablature");
  }
}

export function closeScore(): void {
  stopFollowing();
  if (api) {
    api.destroy();
    api = null;
  }
  markerLayer = null;
  surface?.replaceChildren();
  markers = { points: [], target: null };
}

export function hasScore(): boolean {
  return Boolean(api?.score);
}

export function showTrack(trackIndex: number): void {
  const track = api?.score?.tracks[trackIndex];
  if (api && track) {
    api.renderTracks([track]);
  }
}

/** Tab, notation or both. Before the first score this only sets what it will open with. */
export function setStaveChoice(choice: StaveChoice): void {
  if (choice === staveChoice) {
    return;
  }
  staveChoice = choice;
  if (!api || !namespace) {
    return;
  }
  api.settings.display.staveProfile = staveProfile(namespace, choice);
  api.updateSettings();
  if (api.score) {
    api.render();
  }
}

/** Swaps the reference the cursor follows (null: none). */
export function setTabReference(next: ReferenceSource | null): void {
  unsubscribeReference?.();
  unsubscribeReference = null;
  stopFollowing();
  reference = next;
  referenceWasPlaying = false;
  if (next) {
    unsubscribeReference = next.subscribe(onReferenceChange);
  }
  if (alphaTabIsPlaying()) {
    api?.pause();
  }
  onReferenceChange();
}

/** Hands the sync points to alphaTab and redraws their flags. */
export function applySyncPoints(points: readonly PracticeToolTabSyncPoint[], target: BarSlot | null): void {
  markers = { points: points.map((point) => ({ ...point })), target };
  const score = api?.score;
  if (score) {
    score.applyFlatSyncPoints(markers.points);
    api?.updateSyncPoints();
    pushPosition();
  }
  drawMarkers();
}

/** Sync editing: a click on a bar picks it instead of seeking there. */
export function setSyncEditing(on: boolean): void {
  syncEditing = on;
  if (api) {
    api.settings.player.enableSeekToClick = !on;
  }
  drawMarkers();
}

/** Every pass through every bar, in the order the score plays. Empty before a score loads. */
export function getPlaybackOrder(): BarSlot[] {
  return api?.tickCache ? buildPlaybackOrder(api.tickCache.masterBars) : [];
}

/** Where the cursor is, as a bar pass — the default target for the first tap. */
export function getSlotAtCursor(): BarSlot | null {
  const order = getPlaybackOrder();
  const tick = api?.tickPosition ?? 0;
  let current: BarSlot | null = order[0] ?? null;
  for (const slot of order) {
    if (slot.startTick <= tick) {
      current = slot;
    }
  }
  return current;
}

/** Re-sends the reference's position (after the view is shown again). */
export function refreshTabPosition(): void {
  pushPosition();
}
