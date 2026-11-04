/**
 * The Practice Tool's tab view: a score rendered by alphaTab, its cursor
 * following the reference — the backing track, or a YouTube video — through
 * sync points the user marks bar by bar. The facade over practiceTool/tab/:
 *
 *   tab/panel.ts             the section's DOM: toolbar, sync bar, drop zone, T key
 *   tab/session.ts           which score and reference are open, and what changing either does
 *   tab/tabView.ts           the alphaTab instance and its external-media player
 *   tab/tabState.ts          the open binding and its per-reference autosave
 *   tab/syncPoints.ts        sync point rules (pure)
 *   tab/reference.ts         the backing track as a reference
 *   tab/youtubeReference.ts  a YouTube video as a reference, through the API's relay page
 *   tab/youtubeUrl.ts        YouTube links to video ids (pure)
 *   tab/alphaTabLoader.ts    loads the vendored alphaTab script on first use
 *
 * Score files are kept by the engine (PracticeTabMessages.h); docs/practice-tool-tablature.md
 * describes the whole feature.
 */

import type { IncomingPayload } from "../messages/types.js";
import type { PracticeToolProject } from "../types.js";
import { applyProjectTab, captureProjectTab, onTabFileFailed, onTabFileLoaded, onTrackLoaded } from "./tab/session.js";

export { initializePracticeToolTab, renderPracticeToolTab } from "./tab/panel.js";

/** `practiceToolTabLoaded`: a score's bytes, just opened or asked for by id. */
export function onPracticeToolTabLoaded(payload: IncomingPayload): void {
  onTabFileLoaded(payload as { id?: string; fileName?: string; data?: string });
}

/** `practiceToolTabFailed`: a recalled score is no longer stored. */
export function onPracticeToolTabFailed(payload: IncomingPayload): void {
  onTabFileFailed(payload as { id?: string; error?: string });
}

/** A backing track loaded; `recalled` is the project it was loaded for, if any. */
export function onPracticeToolTabTrackLoaded(recalled: PracticeToolProject | null): void {
  onTrackLoaded(recalled);
}

/** A project recalled without a track load (its track was already open, or it has only a video). */
export function applyPracticeToolProjectTab(project: PracticeToolProject): void {
  applyProjectTab(project);
}

/** The tab and reference a project saves. */
export function capturePracticeToolProjectTab(): Pick<PracticeToolProject, "tab" | "youtube"> {
  return captureProjectTab();
}
