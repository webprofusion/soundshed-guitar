/**
 * The tab section of the Practice Tool: its toolbar, the sync bar, the drop
 * zone and the T shortcut. The session rules are session.ts's; this is only
 * the DOM.
 */

import { updateAppSetting } from "../../appSettingsStore.js";
import { browsePracticeToolTab, importPracticeToolTabData } from "../../bridge.js";
import { showNotification } from "../../notifications.js";
import { uiState } from "../../state.js";
import { arrayBufferToBase64, escapeHtml } from "../../utils.js";
import {
  chooseReferenceKind,
  closeTab,
  getActiveReference,
  getScoreTitle,
  getSyncTarget,
  getTabError,
  getTrackNames,
  isTabLoading,
  onScoreParsed,
  onScoreTimingReady,
  onTabError,
  requestTabRender,
  selectTrack,
  setSyncTarget,
  setTabRenderer,
  setVideoHost,
  updateSyncPoints,
  useYouTubeVideo,
} from "./session.js";
import { formatSyncTime, nextSlot, removeBarStart, setBarStart, shiftSyncPoints } from "./syncPoints.js";
import { getReferenceKind, getTabBinding, getYouTubeReference, onTabStateChange } from "./tabState.js";
import {
  getPlaybackOrder,
  getSlotAtCursor,
  hasScore,
  mountTabView,
  refreshTabPosition,
  setStaveChoice,
  setSyncEditing,
  type StaveChoice,
} from "./tabView.js";
import { parseYouTubeStartSeconds, parseYouTubeVideoId } from "./youtubeUrl.js";

const STAVES_SETTING_KEY = "practiceTool.tabStaves";
const SHIFT_STEP_MS = 10;
const TAB_FILE_EXTENSIONS = [".gp", ".gpx", ".gp3", ".gp4", ".gp5", ".gp7", ".musicxml", ".mxl", ".xml", ".capx", ".atex", ".alphatex"];

let syncEditing = false;

function byId<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

/** The tab section is on screen: the Jam panel, on its Practice Tool section. */
export function isTabSectionVisible(): boolean {
  if (!document.getElementById("panel-jam")?.classList.contains("active")) {
    return false;
  }
  const section = document.getElementById("jam-section-panel-practice-tool");
  return Boolean(section) && !section?.hidden;
}

function storedStaveChoice(): StaveChoice {
  const stored = uiState.appSettings?.[STAVES_SETTING_KEY];
  return stored === "scoretab" || stored === "score" ? stored : "tab";
}

function ordinal(n: number): string {
  return n === 2 ? "2nd" : n === 3 ? "3rd" : `${n}th`;
}

function describeSlot(slot: { barIndex: number; occurrence: number }): string {
  return `bar ${slot.barIndex + 1}${slot.occurrence > 0 ? ` (${ordinal(slot.occurrence + 1)} time)` : ""}`;
}

function hintText(): string {
  const error = getTabError();
  if (error) {
    return error;
  }
  const binding = getTabBinding();
  const reference = getActiveReference();
  const referenceName = getReferenceKind() === "youtube" ? "the video" : "the backing track";
  if (!binding) {
    return "Open or drop a Guitar Pro, MusicXML or alphaTex file to show its tab and follow it with the reference.";
  }
  if (isTabLoading() || !hasScore()) {
    return `Opening ${binding.fileName}…`;
  }
  if (!reference?.isAvailable()) {
    return getReferenceKind() === "youtube"
      ? "Paste a YouTube link and load it to play the tab along with the video."
      : "Load a backing track to play the tab along with it.";
  }
  if (syncEditing) {
    const target = getSyncTarget();
    return target
      ? `Play ${referenceName} and press T (or the button) on the first beat of ${describeSlot(target)}; each press moves on a bar. Paused, it sets the bar to the exact spot. Click a bar to choose another.`
      : "Click the bar to sync.";
  }
  return binding.syncPoints.length
    ? `The cursor follows ${referenceName}. Click a beat to jump there.`
    : `Not synced yet: the cursor follows ${referenceName} at the score's own tempo from 0:00. Use Edit Sync to line the bars up.`;
}

function renderToolbar(): void {
  const binding = getTabBinding();
  const kind = getReferenceKind();
  const trackSelect = byId<HTMLSelectElement>("practice-tool-tab-track");
  const staves = byId<HTMLSelectElement>("practice-tool-tab-staves");
  const referenceSelect = byId<HTMLSelectElement>("practice-tool-tab-reference");
  const urlInput = byId<HTMLInputElement>("practice-tool-tab-youtube-url");
  const loadVideo = byId<HTMLButtonElement>("practice-tool-tab-youtube-load");
  const syncToggle = byId<HTMLButtonElement>("practice-tool-tab-sync-toggle");
  const closeButton = byId<HTMLButtonElement>("practice-tool-tab-close");
  const names = getTrackNames();

  if (trackSelect) {
    trackSelect.hidden = !binding || names.length < 2;
    const signature = names.join("\u0000");
    if (trackSelect.dataset.signature !== signature) {
      trackSelect.dataset.signature = signature;
      trackSelect.innerHTML = names.map((name, index) => `<option value="${index}">${escapeHtml(name)}</option>`).join("");
    }
    trackSelect.value = String(binding?.trackIndex ?? 0);
  }
  if (staves) {
    staves.hidden = !binding;
    staves.value = storedStaveChoice();
  }
  if (referenceSelect) {
    referenceSelect.value = kind;
  }
  if (urlInput && loadVideo) {
    urlInput.hidden = kind !== "youtube";
    loadVideo.hidden = kind !== "youtube";
    const video = getYouTubeReference();
    if (video && document.activeElement !== urlInput && !urlInput.value) {
      urlInput.value = `https://youtu.be/${video.videoId}`;
    }
  }
  if (syncToggle) {
    syncToggle.disabled = !binding || !hasScore();
    syncToggle.setAttribute("aria-pressed", String(syncEditing));
    syncToggle.classList.toggle("is-active", syncEditing);
  }
  if (closeButton) {
    closeButton.hidden = !binding;
  }
}

function renderSyncBar(): void {
  const bar = byId<HTMLElement>("practice-tool-tab-sync-bar");
  if (!bar) {
    return;
  }
  bar.hidden = !syncEditing;
  if (!syncEditing) {
    return;
  }
  const binding = getTabBinding();
  const target = getSyncTarget();
  const points = binding?.syncPoints ?? [];
  const available = Boolean(getActiveReference()?.isAvailable());
  const setButton = byId<HTMLButtonElement>("practice-tool-tab-sync-set");
  if (setButton) {
    setButton.disabled = !target || !available;
    setButton.textContent = target ? `Bar ${target.barIndex + 1} starts here` : "Bar starts here";
  }
  const onTarget = target
    ? points.some((point) => point.barIndex === target.barIndex && point.barOccurence === target.occurrence && point.barPosition === 0)
    : false;
  const removeButton = byId<HTMLButtonElement>("practice-tool-tab-sync-remove");
  if (removeButton) {
    removeButton.disabled = !onTarget;
  }
  for (const id of ["practice-tool-tab-sync-earlier", "practice-tool-tab-sync-later", "practice-tool-tab-sync-clear"]) {
    const button = byId<HTMLButtonElement>(id);
    if (button) {
      button.disabled = points.length === 0;
    }
  }
  const status = byId<HTMLElement>("practice-tool-tab-sync-status");
  if (status) {
    const onTargetPoint = target
      ? points.find((point) => point.barIndex === target.barIndex && point.barOccurence === target.occurrence && point.barPosition === 0)
      : undefined;
    status.textContent = `${points.length} sync point${points.length === 1 ? "" : "s"}`
      + (onTargetPoint ? ` · ${describeSlot(target!)} at ${formatSyncTime(onTargetPoint.millisecondOffset)}` : "");
  }
}

function renderStage(): void {
  const binding = getTabBinding();
  const viewport = byId<HTMLElement>("practice-tool-tab-viewport");
  const video = byId<HTMLElement>("practice-tool-tab-video");
  if (viewport) {
    viewport.hidden = !binding;
    viewport.setAttribute("aria-label", getScoreTitle() || binding?.fileName || "Tablature");
  }
  if (video) {
    video.hidden = getReferenceKind() !== "youtube" || !getYouTubeReference();
  }
  const hint = byId<HTMLElement>("practice-tool-tab-hint");
  if (hint) {
    hint.textContent = hintText();
    hint.classList.toggle("is-error", Boolean(getTabError()));
  }
}

export function renderPracticeToolTab(): void {
  renderToolbar();
  renderSyncBar();
  renderStage();
  if (isTabSectionVisible()) {
    refreshTabPosition();
  }
}

function setSyncEditingOn(on: boolean): void {
  syncEditing = on && Boolean(getTabBinding()) && hasScore();
  setSyncEditing(syncEditing);
  // The first target is the first bar while nothing is synced (a first tap is
  // almost always the song's first downbeat, wherever the track was left), and
  // the bar under the cursor once something is.
  const first = getTabBinding()?.syncPoints.length ? getSlotAtCursor() : (getPlaybackOrder()[0] ?? null);
  setSyncTarget(syncEditing ? getSyncTarget() ?? first : null);
}

/** Sets the target bar's start to the reference's time now, then moves on a bar. */
function markBarStart(eventTimeStamp?: number): void {
  const binding = getTabBinding();
  const target = getSyncTarget();
  const reference = getActiveReference();
  if (!binding || !target || !reference?.isAvailable()) {
    return;
  }
  let ms = reference.positionMs();
  // A key press is handled a moment after it happened; take the time it happened.
  if (eventTimeStamp !== undefined && reference.isPlaying()) {
    const lagMs = Math.max(0, Math.min(250, performance.now() - eventTimeStamp));
    ms -= lagMs * (uiState.practiceTool?.speed && getReferenceKind() === "track" ? uiState.practiceTool.speed : 1);
  }
  const order = getPlaybackOrder();
  updateSyncPoints(setBarStart(order, binding.syncPoints, target, ms));
  setSyncTarget(nextSlot(order, target.barIndex, target.occurrence) ?? target);
}

function loadVideoFromInput(): void {
  const input = byId<HTMLInputElement>("practice-tool-tab-youtube-url");
  const text = input?.value ?? "";
  const videoId = parseYouTubeVideoId(text);
  if (!videoId) {
    showNotification("Not a YouTube link", "Paste a link to a YouTube video, like https://youtu.be/…");
    input?.focus();
    return;
  }
  useYouTubeVideo(videoId, parseYouTubeStartSeconds(text));
}

function isTabFile(name: string): boolean {
  const lower = name.trim().toLowerCase();
  return TAB_FILE_EXTENSIONS.some((ext) => lower.endsWith(ext));
}

function bindDropZone(section: HTMLElement): void {
  section.addEventListener("dragover", (event) => {
    const types = event.dataTransfer?.types;
    if (types && Array.from(types).includes("Files")) {
      event.preventDefault();
      section.classList.add("is-drag-over");
    }
  });
  section.addEventListener("dragleave", () => section.classList.remove("is-drag-over"));
  section.addEventListener("drop", (event) => {
    section.classList.remove("is-drag-over");
    const file = event.dataTransfer?.files?.[0];
    if (!file) {
      return;
    }
    event.preventDefault();
    if (!isTabFile(file.name)) {
      showNotification("Not a tab file", "Drop a Guitar Pro (.gp, .gp5, .gpx…), MusicXML, Capella or alphaTex file");
      return;
    }
    void file.arrayBuffer()
      .then((buffer) => importPracticeToolTabData(file.name, arrayBufferToBase64(buffer)))
      .catch((error: unknown) => showNotification("Unable to read dropped file", error instanceof Error ? error.message : String(error)));
  });
}

function isTypingTarget(target: EventTarget | null): boolean {
  const element = target as HTMLElement | null;
  return Boolean(element && (element.isContentEditable || ["INPUT", "TEXTAREA", "SELECT"].includes(element.tagName)));
}

function bindActions(): void {
  byId("practice-tool-tab-open")?.addEventListener("click", () => browsePracticeToolTab());
  byId("practice-tool-tab-close")?.addEventListener("click", () => {
    setSyncEditingOn(false);
    closeTab();
  });
  byId<HTMLSelectElement>("practice-tool-tab-track")?.addEventListener("change", (event) => {
    selectTrack(Number((event.target as HTMLSelectElement).value) || 0);
  });
  byId<HTMLSelectElement>("practice-tool-tab-staves")?.addEventListener("change", (event) => {
    const value = (event.target as HTMLSelectElement).value as StaveChoice;
    updateAppSetting(STAVES_SETTING_KEY, value);
    setStaveChoice(value);
  });
  byId<HTMLSelectElement>("practice-tool-tab-reference")?.addEventListener("change", (event) => {
    chooseReferenceKind((event.target as HTMLSelectElement).value === "youtube" ? "youtube" : "track");
    if (getReferenceKind() === "youtube" && !getYouTubeReference()) {
      byId<HTMLInputElement>("practice-tool-tab-youtube-url")?.focus();
    }
  });
  byId("practice-tool-tab-youtube-load")?.addEventListener("click", loadVideoFromInput);
  byId<HTMLInputElement>("practice-tool-tab-youtube-url")?.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      loadVideoFromInput();
    }
  });
  byId("practice-tool-tab-sync-toggle")?.addEventListener("click", () => setSyncEditingOn(!syncEditing));
  byId("practice-tool-tab-sync-set")?.addEventListener("click", () => markBarStart());
  byId("practice-tool-tab-sync-remove")?.addEventListener("click", () => {
    const binding = getTabBinding();
    const target = getSyncTarget();
    if (binding && target) {
      updateSyncPoints(removeBarStart(binding.syncPoints, target));
    }
  });
  const shift = (delta: number) => {
    const binding = getTabBinding();
    if (binding?.syncPoints.length) {
      updateSyncPoints(shiftSyncPoints(binding.syncPoints, delta));
    }
  };
  byId("practice-tool-tab-sync-earlier")?.addEventListener("click", () => shift(-SHIFT_STEP_MS));
  byId("practice-tool-tab-sync-later")?.addEventListener("click", () => shift(SHIFT_STEP_MS));
  byId("practice-tool-tab-sync-clear")?.addEventListener("click", () => updateSyncPoints([]));

  document.addEventListener("keydown", (event) => {
    if (!syncEditing || event.repeat || event.ctrlKey || event.metaKey || event.altKey) {
      return;
    }
    if ((event.key === "t" || event.key === "T") && !isTypingTarget(event.target) && isTabSectionVisible()) {
      event.preventDefault();
      markBarStart(event.timeStamp);
    }
  });
}

export function initializePracticeToolTab(): void {
  const section = byId<HTMLElement>("practice-tool-tab-section");
  const surface = byId<HTMLElement>("practice-tool-tab-surface");
  const video = byId<HTMLElement>("practice-tool-tab-video");
  if (!section || !surface || !video || section.dataset.bound === "true") {
    return;
  }
  section.dataset.bound = "true";
  setVideoHost(video);
  setStaveChoice(storedStaveChoice());
  setTabRenderer(renderPracticeToolTab);
  mountTabView(surface, {
    onScoreLoaded: onScoreParsed,
    onTimingReady: onScoreTimingReady,
    onRendered: () => undefined,
    onError: (message) => onTabError(message),
    onBarPicked: (slot) => setSyncTarget(slot),
  }, isTabSectionVisible);
  onTabStateChange(() => requestTabRender());
  bindDropZone(section);
  bindActions();
  renderPracticeToolTab();
}
