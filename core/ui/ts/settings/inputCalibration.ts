/**
 * Input calibration, shown in the app as Input Levelling: measuring the peak an
 * instrument actually produces, and storing the trim that brings it to the level
 * the amp models expect. The code, settings keys and messages keep the older
 * "calibration" name, so saved profiles carry over.
 *
 * The engine owns all of it (PluginControllerInputLevelling.cpp): the stored profiles, the
 * training that follows the raw input's peak, and the trim maths. This module draws what the
 * engine reports ("inputLevelling", "inputLevellingTraining", "inputLevellingSaved") and sends
 * the user's choices back as commands. The toolbar menu and the settings select render from
 * the same profile list.
 */

import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import type { IncomingPayload } from "../messages/types.js";
import { showNotification } from "../notifications.js";
import { uiState } from "../state.js";
import type { UserInputCalibrationProfile } from "../types.js";
import { renderPickupMatchTab } from "../pickupMatchGlobal.js";
import { userInputCalibrationCancelButton, userInputCalibrationCapturedPeak, userInputCalibrationCloseButton, userInputCalibrationDeleteButton, userInputCalibrationDescriptionInput, userInputCalibrationLivePeak, userInputCalibrationModal, userInputCalibrationNameInput, userInputCalibrationProfileSelect, userInputCalibrationRecommendedTrim, userInputCalibrationResetButton, userInputCalibrationSaveButton, userInputCalibrationStatus, userInputCalibrationSummary, userInputCalibrationToolbarMenu, userInputCalibrationToolbarTrigger, userInputCalibrationTrainButton } from "./dom.js";
import { USER_INPUT_CALIBRATION_NONE_VALUE, USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS } from "./keys.js";

let userInputCalibrationControlsInitialized = false;

/** The engine's "inputLevelling": the profiles, the active one and whether training runs. */
interface LevellingState {
  profiles: UserInputCalibrationProfile[];
  activeId: string | null;
  trainingActive: boolean;
  targetPeakDbfs: number;
}

/** The engine's "inputLevellingTraining", in dBFS (NaN for no level yet). */
interface TrainingStatus {
  active: boolean;
  livePeakDbfs: number;
  capturedPeakDbfs: number;
  recommendedTrimDb: number;
  clipped: boolean;
}

let levelling: LevellingState = {
  profiles: [],
  activeId: null,
  trainingActive: false,
  targetPeakDbfs: USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS,
};

const IDLE_TRAINING: TrainingStatus = {
  active: false,
  livePeakDbfs: Number.NaN,
  capturedPeakDbfs: Number.NaN,
  recommendedTrimDb: Number.NaN,
  clipped: false,
};

let training: TrainingStatus = { ...IDLE_TRAINING };

function numberOrNaN(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) ? value : Number.NaN;
}

function stringOrEmpty(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function readProfile(entry: unknown): UserInputCalibrationProfile | null {
  if (!entry || typeof entry !== "object" || Array.isArray(entry)) {
    return null;
  }
  const candidate = entry as Record<string, unknown>;
  const id = stringOrEmpty(candidate.id);
  if (!id) {
    return null;
  }
  return {
    id,
    name: stringOrEmpty(candidate.name),
    description: stringOrEmpty(candidate.description),
    capturedPeakDbfs: numberOrNaN(candidate.capturedPeakDbfs),
    targetPeakDbfs: numberOrNaN(candidate.targetPeakDbfs),
    gainDb: numberOrNaN(candidate.gainDb),
    createdAt: typeof candidate.createdAt === "string" ? candidate.createdAt : undefined,
    updatedAt: typeof candidate.updatedAt === "string" ? candidate.updatedAt : undefined,
  };
}

/** "inputLevelling": sent for getInputLevelling and after every change, whoever made it. */
export function onInputLevelling(payload: IncomingPayload): void {
  const message = payload as { profiles?: unknown; activeId?: unknown; trainingActive?: unknown; targetPeakDbfs?: unknown };
  const profiles = Array.isArray(message.profiles)
    ? message.profiles.map(readProfile).filter((profile): profile is UserInputCalibrationProfile => profile !== null)
    : [];
  const target = numberOrNaN(message.targetPeakDbfs);
  levelling = {
    profiles,
    activeId: typeof message.activeId === "string" && message.activeId ? message.activeId : null,
    trainingActive: message.trainingActive === true,
    targetPeakDbfs: Number.isFinite(target) ? target : USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS,
  };
  refreshUserInputCalibrationView();
}

/** "inputLevellingTraining": ~15 Hz while a profile is trained, and once when it stops. */
export function onInputLevellingTraining(payload: IncomingPayload): void {
  const message = payload as Record<string, unknown>;
  training = {
    active: message.active === true,
    livePeakDbfs: numberOrNaN(message.livePeakDbfs),
    capturedPeakDbfs: numberOrNaN(message.capturedPeakDbfs),
    recommendedTrimDb: numberOrNaN(message.recommendedTrimDb),
    clipped: message.clipped === true,
  };
  if (levelling.trainingActive !== training.active) {
    levelling = { ...levelling, trainingActive: training.active };
    refreshUserInputCalibrationView();
    return;
  }
  if (isUserInputCalibrationModalOpen()) {
    refreshUserInputCalibrationTrainingView();
  }
}

/** "inputLevellingSaved": the save's outcome. A saved profile ends the training and the dialog. */
export function onInputLevellingSaved(payload: IncomingPayload): void {
  const message = payload as { ok?: unknown; message?: unknown; profile?: unknown };
  if (message.ok !== true) {
    showNotification("Input levelling profile not saved", stringOrEmpty(message.message));
    return;
  }
  const profile = readProfile(message.profile);
  closeUserInputCalibrationTrainingModal();
  if (profile) {
    showNotification("Input levelling profile saved", `${profile.name} • ${formatSignedDbValue(profile.gainDb)}`);
  }
}

export function readUserInputCalibrationProfiles(): UserInputCalibrationProfile[] {
  return levelling.profiles;
}

export function readActiveUserInputCalibrationProfileId(): string | null {
  return levelling.activeId;
}

export function getActiveUserInputCalibrationProfile(): UserInputCalibrationProfile | null {
  const activeProfileId = readActiveUserInputCalibrationProfileId();
  return activeProfileId ? levelling.profiles.find((profile) => profile.id === activeProfileId) ?? null : null;
}

export function readDisplayedUserInputCalibrationProfileId(): string | null {
  return levelling.trainingActive ? null : readActiveUserInputCalibrationProfileId();
}

export function formatDbfsValue(value: number): string {
  return Number.isFinite(value) ? `${value.toFixed(1)} dBFS` : "-";
}

export function formatSignedDbValue(value: number): string {
  if (!Number.isFinite(value)) {
    return "-";
  }
  const sign = value >= 0 ? "+" : "";
  return `${sign}${value.toFixed(1)} dB`;
}

export function getCurrentRawInputPeakDbfs(): number {
  return uiState.signalDiagnostics?.rawInput?.peakDbfs
    ?? uiState.signalDiagnostics?.input?.peakDbfs
    ?? Number.NEGATIVE_INFINITY;
}

export function isUserInputCalibrationModalOpen(): boolean {
  return Boolean(userInputCalibrationModal && userInputCalibrationModal.style.display !== "none");
}

function pickupMatchIndicator(): HTMLElement | null {
  return document.getElementById("pickup-match-indicator");
}

export function isUserInputCalibrationToolbarMenuOpen(): boolean {
  return Boolean(userInputCalibrationToolbarMenu && !userInputCalibrationToolbarMenu.hidden);
}

export function positionUserInputCalibrationToolbarMenu(): void {
  if (!userInputCalibrationToolbarMenu || !userInputCalibrationToolbarTrigger) {
    return;
  }

  const offsetParent = userInputCalibrationToolbarMenu.offsetParent;
  if (!(offsetParent instanceof HTMLElement)) {
    return;
  }

  const viewportMargin = 8;
  const parentRect = offsetParent.getBoundingClientRect();
  const triggerRect = userInputCalibrationToolbarTrigger.getBoundingClientRect();
  const menuWidth = userInputCalibrationToolbarMenu.offsetWidth;

  const centeredLeft = (triggerRect.left + (triggerRect.width / 2)) - parentRect.left - (menuWidth / 2);
  const minLeft = viewportMargin - parentRect.left;
  const maxLeft = window.innerWidth - viewportMargin - parentRect.left - menuWidth;
  const clampedLeft = Math.min(Math.max(centeredLeft, minLeft), Math.max(minLeft, maxLeft));

  userInputCalibrationToolbarMenu.style.left = `${Math.round(clampedLeft)}px`;
}

export function closeUserInputCalibrationToolbarMenu(): void {
  if (userInputCalibrationToolbarMenu) {
    userInputCalibrationToolbarMenu.hidden = true;
  }
  userInputCalibrationToolbarTrigger?.setAttribute("aria-expanded", "false");
  userInputCalibrationToolbarTrigger
    ?.closest(".control-bar")
    ?.classList.remove("has-open-calibration");
}

export function openUserInputCalibrationToolbarMenu(tab?: ToolbarTab): void {
  if (!userInputCalibrationToolbarMenu) {
    return;
  }

  userInputCalibrationToolbarMenu.hidden = false;
  // Pickup Match is drawn fresh each time it is shown, so it opens on the node as it is now.
  ensureToolbarMenuTabs(userInputCalibrationToolbarMenu);
  showToolbarTab(userInputCalibrationToolbarMenu, tab ?? activeToolbarTab);
  positionUserInputCalibrationToolbarMenu();
  userInputCalibrationToolbarTrigger?.setAttribute("aria-expanded", "true");
  userInputCalibrationToolbarTrigger
    ?.closest(".control-bar")
    ?.classList.add("has-open-calibration");
}

export function toggleUserInputCalibrationToolbarMenu(): void {
  if (isUserInputCalibrationToolbarMenuOpen()) {
    closeUserInputCalibrationToolbarMenu();
    return;
  }

  openUserInputCalibrationToolbarMenu();
}

export function refreshUserInputCalibrationTrainingView(): void {
  if (userInputCalibrationLivePeak) {
    userInputCalibrationLivePeak.textContent = formatDbfsValue(training.livePeakDbfs);
  }
  if (userInputCalibrationCapturedPeak) {
    userInputCalibrationCapturedPeak.textContent = formatDbfsValue(training.capturedPeakDbfs);
  }
  if (userInputCalibrationRecommendedTrim) {
    userInputCalibrationRecommendedTrim.textContent = formatSignedDbValue(training.recommendedTrimDb);
  }

  const trimmedName = userInputCalibrationNameInput?.value.trim() ?? "";
  const canSave = trimmedName.length > 0 && training.active && Number.isFinite(training.capturedPeakDbfs) && !training.clipped;

  if (userInputCalibrationSaveButton) {
    userInputCalibrationSaveButton.disabled = !canSave;
  }

  if (!userInputCalibrationStatus) {
    return;
  }

  if (training.clipped) {
    userInputCalibrationStatus.textContent = "Input clipped during training. Back the interface gain down and capture again.";
    return;
  }
  if (!Number.isFinite(training.capturedPeakDbfs)) {
    userInputCalibrationStatus.textContent = "Play your loudest picking or strumming to capture a training peak.";
    return;
  }
  userInputCalibrationStatus.textContent = `Captured ${formatDbfsValue(training.capturedPeakDbfs)}. Saving this profile will apply ${formatSignedDbValue(training.recommendedTrimDb)} toward a ${formatDbfsValue(levelling.targetPeakDbfs)} target.`;
}

/** Forgets the captured peak and any clip, to capture again. */
export function resetUserInputCalibrationTrainingPeak(): void {
  postMessage({ type: "resetInputLevellingTraining" });
}

export function openUserInputCalibrationTrainingModal(): void {
  if (!userInputCalibrationModal || isUserInputCalibrationModalOpen()) {
    return;
  }

  closeUserInputCalibrationToolbarMenu();
  // The engine bypasses the active trim and listens to the raw input until it is stopped or saved.
  postMessage({ type: "startInputLevellingTraining" });
  if (userInputCalibrationNameInput) userInputCalibrationNameInput.value = "";
  if (userInputCalibrationDescriptionInput) userInputCalibrationDescriptionInput.value = "";
  training = { ...IDLE_TRAINING, active: true };
  userInputCalibrationModal.style.display = "flex";
  refreshUserInputCalibrationTrainingView();
  userInputCalibrationNameInput?.focus();
}

export function closeUserInputCalibrationTrainingModal(): void {
  if (!userInputCalibrationModal || !isUserInputCalibrationModalOpen()) {
    return;
  }
  userInputCalibrationModal.style.display = "none";
  training = { ...IDLE_TRAINING };
  // After a save the engine has already stopped; stopping again changes nothing.
  postMessage({ type: "stopInputLevellingTraining" });
}

export async function deleteActiveUserInputCalibrationProfile(): Promise<void> {
  const activeProfile = getActiveUserInputCalibrationProfile();
  if (!activeProfile) {
    return;
  }

  await deleteUserInputCalibrationProfile(activeProfile.id);
}

export async function deleteUserInputCalibrationProfile(profileId: string): Promise<void> {
  const profile = levelling.profiles.find((entry) => entry.id === profileId) ?? null;
  if (!profile) {
    return;
  }

  const confirmed = await showConfirm(
    `Delete input levelling profile "${profile.name}"?`,
    "Delete Input Levelling Profile",
  );
  if (!confirmed) {
    return;
  }

  postMessage({ type: "deleteInputLevellingProfile", id: profile.id });
  showNotification("Input levelling profile deleted", profile.name);
}

/** Saves the peak the engine captured as a new, active profile; "inputLevellingSaved" answers. */
export function saveUserInputCalibrationProfile(): void {
  const name = userInputCalibrationNameInput?.value.trim() ?? "";
  if (!name) {
    showNotification("Input levelling profile name required");
    return;
  }

  postMessage({
    type: "saveInputLevellingProfile",
    name,
    description: userInputCalibrationDescriptionInput?.value.trim() ?? "",
  });
}

export function applyUserInputCalibrationProfileSelection(activeProfileId: string | null): void {
  // Drawn at once; the engine's "inputLevelling" stands.
  levelling = { ...levelling, activeId: activeProfileId };
  postMessage({ type: "selectInputLevellingProfile", id: activeProfileId });
  refreshUserInputCalibrationView();
}

export function handleUserInputCalibrationProfileSelection(): void {
  const selectedValue = userInputCalibrationProfileSelect?.value ?? USER_INPUT_CALIBRATION_NONE_VALUE;
  const activeProfileId = selectedValue === USER_INPUT_CALIBRATION_NONE_VALUE ? null : selectedValue;
  applyUserInputCalibrationProfileSelection(activeProfileId);
}

/** The flyout's tabs: the calibration profiles, and the global chain's Pickup Match. */
type ToolbarTab = "calibration" | "pickup";
let activeToolbarTab: ToolbarTab = "calibration";

/**
 * The tab strip and the two panels, made once. The calibration list is redrawn into its panel
 * whenever the profiles change; Pickup Match draws its own each time its tab is shown. The
 * strip reuses the params panel's tab styling.
 */
function ensureToolbarMenuTabs(menu: HTMLElement): HTMLElement {
  const existing = menu.querySelector<HTMLElement>('.input-calibration-tab-panel[data-tab="calibration"]');
  if (existing) {
    return existing;
  }
  menu.innerHTML = `
    <div class="node-param-tabs input-calibration-tabs" role="tablist" aria-label="Input settings">
      <button class="node-param-tab" data-tab="calibration" type="button" role="tab">Levelling</button>
      <button class="node-param-tab" data-tab="pickup" type="button" role="tab">Pickup Match</button>
    </div>
    <div class="input-calibration-tab-panel" data-tab="calibration" role="menu" aria-label="Input levelling"></div>
    <div class="input-calibration-tab-panel" data-tab="pickup" role="tabpanel" aria-label="Pickup Match"></div>`;
  showToolbarTab(menu, activeToolbarTab);
  return menu.querySelector<HTMLElement>('.input-calibration-tab-panel[data-tab="calibration"]') as HTMLElement;
}

function showToolbarTab(menu: HTMLElement, requested: ToolbarTab): void {
  // Pickup Match is experimental: with its feature off the strip goes and the profiles show alone.
  const pickupEnabled = isFeatureEnabled(Features.PickupMatch);
  const tab = pickupEnabled ? requested : "calibration";
  activeToolbarTab = tab;
  const strip = menu.querySelector<HTMLElement>(".input-calibration-tabs");
  if (strip) {
    strip.hidden = !pickupEnabled;
  }
  menu.querySelectorAll<HTMLElement>(".input-calibration-tabs .node-param-tab").forEach((button) => {
    const active = button.dataset.tab === tab;
    button.classList.toggle("is-active", active);
    button.setAttribute("aria-selected", active ? "true" : "false");
  });
  menu.querySelectorAll<HTMLElement>(".input-calibration-tab-panel").forEach((panel) => {
    panel.hidden = panel.dataset.tab !== tab;
  });
  menu.classList.toggle("is-pickup-tab", tab === "pickup");
  const pickupPanel = menu.querySelector<HTMLElement>('.input-calibration-tab-panel[data-tab="pickup"]');
  if (tab === "pickup" && pickupPanel) {
    renderPickupMatchTab(pickupPanel);
  }
  if (isUserInputCalibrationToolbarMenuOpen()) {
    positionUserInputCalibrationToolbarMenu();
  }
}

/**
 * The Pickup Match feature was switched on or off: the flyout's tab strip comes or goes. Left
 * alone otherwise, since showing the Pickup Match tab again would redraw it under a knob drag.
 */
export function syncUserInputCalibrationToolbarTabs(): void {
  const menu = userInputCalibrationToolbarMenu;
  const strip = menu?.querySelector<HTMLElement>(".input-calibration-tabs");
  if (!menu || !strip) {
    return;
  }
  const stripShown = !strip.hidden;
  if (stripShown !== isFeatureEnabled(Features.PickupMatch)) {
    showToolbarTab(menu, activeToolbarTab);
  }
}

export function renderUserInputCalibrationToolbarMenu(profiles: UserInputCalibrationProfile[], activeProfile: UserInputCalibrationProfile | null): void {
  // Bound to a local so the null check narrows inside the closures below; a
  // narrowing on an imported binding does not survive into a closure.
  const toolbarMenu = userInputCalibrationToolbarMenu;
  if (!toolbarMenu) {
    return;
  }

  const panel = ensureToolbarMenuTabs(toolbarMenu);
  panel.innerHTML = "";

  const header = document.createElement("div");
  header.className = "input-calibration-toolbar-menu-header";

  const title = document.createElement("span");
  title.className = "input-calibration-toolbar-menu-title";
  title.textContent = "Input Levelling";
  header.appendChild(title);

  const subtitle = document.createElement("span");
  subtitle.className = "input-calibration-toolbar-menu-subtitle";
  subtitle.textContent = levelling.trainingActive
    ? "Training bypass active"
    : (activeProfile ? `${activeProfile.name} active` : "No active levelling");
  header.appendChild(subtitle);

  panel.appendChild(header);

  const appendProfileItem = (profile: UserInputCalibrationProfile | null): void => {
    const itemRow = document.createElement("div");
    itemRow.className = "input-calibration-toolbar-row";

    const button = document.createElement("button");
    button.type = "button";
    button.className = "input-calibration-toolbar-item";
    button.dataset.profileId = profile?.id ?? USER_INPUT_CALIBRATION_NONE_VALUE;
    button.setAttribute("role", "menuitemradio");

    const isActive = profile ? activeProfile?.id === profile.id : !activeProfile;
    button.classList.toggle("active", isActive);
    button.setAttribute("aria-checked", isActive ? "true" : "false");

    const copy = document.createElement("span");
    copy.className = "input-calibration-toolbar-item-copy";

    const name = document.createElement("span");
    name.className = "input-calibration-toolbar-item-name";
    name.textContent = profile?.name ?? "No levelling";
    copy.appendChild(name);

    const note = document.createElement("span");
    note.className = "input-calibration-toolbar-item-note";
    note.textContent = profile
      ? (isActive
        ? `Applies ${formatSignedDbValue(profile.gainDb)} toward ${formatDbfsValue(profile.targetPeakDbfs)}`
        : (profile.description || `Captured ${formatDbfsValue(profile.capturedPeakDbfs)}`))
      : "Use raw input without a global trim";
    copy.appendChild(note);

    button.appendChild(copy);

    const meta = document.createElement("span");
    meta.className = "input-calibration-toolbar-item-meta";
    meta.textContent = isActive ? "ACTIVE" : (profile ? formatSignedDbValue(profile.gainDb) : "OFF");
    button.appendChild(meta);

    itemRow.appendChild(button);

    if (profile) {
      const deleteButton = document.createElement("button");
      deleteButton.type = "button";
      deleteButton.className = "input-calibration-toolbar-item-delete";
      deleteButton.dataset.action = "delete";
      deleteButton.dataset.profileId = profile.id;
      deleteButton.setAttribute("role", "menuitem");
      deleteButton.setAttribute("aria-label", `Delete ${profile.name}`);
      deleteButton.title = `Delete ${profile.name}`;
      deleteButton.innerHTML = '<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg>';
      itemRow.appendChild(deleteButton);
    }

    panel.appendChild(itemRow);
  };

  appendProfileItem(null);
  profiles.forEach((profile) => appendProfileItem(profile));

  const divider = document.createElement("div");
  divider.className = "input-calibration-toolbar-menu-divider";
  panel.appendChild(divider);

  const trainButton = document.createElement("button");
  trainButton.type = "button";
  trainButton.className = "input-calibration-toolbar-item action";
  trainButton.dataset.action = "train";
  trainButton.setAttribute("role", "menuitem");

  const trainCopy = document.createElement("span");
  trainCopy.className = "input-calibration-toolbar-item-copy";

  const trainName = document.createElement("span");
  trainName.className = "input-calibration-toolbar-item-name";
  trainName.textContent = "Train Input Level";
  trainCopy.appendChild(trainName);

  const trainNote = document.createElement("span");
  trainNote.className = "input-calibration-toolbar-item-note";
  trainNote.textContent = "Open the training dialog and save a new profile";
  trainCopy.appendChild(trainNote);

  trainButton.appendChild(trainCopy);

  const trainMeta = document.createElement("span");
  trainMeta.className = "input-calibration-toolbar-item-meta";
  trainMeta.textContent = "TRAIN";
  trainButton.appendChild(trainMeta);

  panel.appendChild(trainButton);

  if (userInputCalibrationToolbarTrigger) {
    userInputCalibrationToolbarTrigger.classList.toggle("has-active-calibration", Boolean(activeProfile));
    userInputCalibrationToolbarTrigger.title = activeProfile
      ? `Input levelling options (${activeProfile.name})`
      : "Input levelling options";
    userInputCalibrationToolbarTrigger.setAttribute(
      "aria-label",
      activeProfile
        ? `Input levelling options. Active profile ${activeProfile.name}`
        : "Input levelling options",
    );
  }
}

export async function handleUserInputCalibrationToolbarMenuClick(event: MouseEvent): Promise<void> {
  const target = event.target;
  if (!(target instanceof HTMLElement)) {
    return;
  }

  const tabButton = target.closest<HTMLElement>(".input-calibration-tabs .node-param-tab");
  if (tabButton && userInputCalibrationToolbarMenu) {
    showToolbarTab(userInputCalibrationToolbarMenu, tabButton.dataset.tab === "pickup" ? "pickup" : "calibration");
    return;
  }

  const deleteButton = target.closest(".input-calibration-toolbar-item-delete") as HTMLButtonElement | null;
  if (deleteButton) {
    const profileId = deleteButton.dataset.profileId?.trim();
    if (!profileId) {
      return;
    }

    await deleteUserInputCalibrationProfile(profileId);
    return;
  }

  const item = target.closest(".input-calibration-toolbar-item") as HTMLButtonElement | null;
  if (!item) {
    return;
  }

  if (item.dataset.action === "train") {
    openUserInputCalibrationTrainingModal();
    return;
  }

  const selectedValue = item.dataset.profileId ?? USER_INPUT_CALIBRATION_NONE_VALUE;
  const activeProfileId = selectedValue === USER_INPUT_CALIBRATION_NONE_VALUE ? null : selectedValue;
  applyUserInputCalibrationProfileSelection(activeProfileId);
  closeUserInputCalibrationToolbarMenu();
}

export function refreshUserInputCalibrationView(): void {
  // Bound to a local so the null check narrows inside the closures below; a
  // narrowing on an imported binding does not survive into a closure.
  const profileSelect = userInputCalibrationProfileSelect;
  const profiles = readUserInputCalibrationProfiles();
  const persistedActiveProfileId = readActiveUserInputCalibrationProfileId();
  const activeProfileId = readDisplayedUserInputCalibrationProfileId();
  const activeProfile = profiles.find((profile) => profile.id === activeProfileId) ?? null;
  const persistedActiveProfile = profiles.find((profile) => profile.id === persistedActiveProfileId) ?? null;

  if (profileSelect) {
    profileSelect.innerHTML = "";

    const noneOption = document.createElement("option");
    noneOption.value = USER_INPUT_CALIBRATION_NONE_VALUE;
    noneOption.textContent = "No levelling";
    profileSelect.appendChild(noneOption);

    profiles.forEach((profile) => {
      const option = document.createElement("option");
      option.value = profile.id;
      option.textContent = profile.name;
      profileSelect.appendChild(option);
    });

    profileSelect.value = activeProfile ? activeProfile.id : USER_INPUT_CALIBRATION_NONE_VALUE;
  }

  if (userInputCalibrationDeleteButton) {
    userInputCalibrationDeleteButton.disabled = !activeProfile;
  }

  renderUserInputCalibrationToolbarMenu(profiles, activeProfile);

  if (userInputCalibrationSummary) {
    const currentPeakDbfs = getCurrentRawInputPeakDbfs();
    if (levelling.trainingActive) {
      userInputCalibrationSummary.textContent = persistedActiveProfile
        ? `${persistedActiveProfile.name} temporarily bypassed while training a new input level profile.`
        : "Input levelling temporarily bypassed while training a new profile.";
    } else if (activeProfile) {
      const description = activeProfile.description ? `${activeProfile.description}. ` : "";
      const currentPeak = Number.isFinite(currentPeakDbfs) ? ` Current raw peak: ${formatDbfsValue(currentPeakDbfs)}.` : "";
      userInputCalibrationSummary.textContent = `${activeProfile.name}. ${description}Applies ${formatSignedDbValue(activeProfile.gainDb)} toward ${formatDbfsValue(activeProfile.targetPeakDbfs)} from a captured ${formatDbfsValue(activeProfile.capturedPeakDbfs)}.${currentPeak}`;
    } else {
      userInputCalibrationSummary.textContent = "No global input levelling active. Train one profile for each guitar or interface gain position you want Sepia to normalize.";
    }
  }

  if (isUserInputCalibrationToolbarMenuOpen()) {
    positionUserInputCalibrationToolbarMenu();
  }

  if (isUserInputCalibrationModalOpen()) {
    refreshUserInputCalibrationTrainingView();
  }
}

export function initUserInputCalibrationControls(): void {
  if (userInputCalibrationControlsInitialized) {
    refreshUserInputCalibrationView();
    return;
  }

  userInputCalibrationControlsInitialized = true;
  // The profiles are the engine's; it answers with "inputLevelling" and sends it again on every change.
  postMessage({ type: "getInputLevelling" });
  userInputCalibrationProfileSelect?.addEventListener("change", handleUserInputCalibrationProfileSelection);
  userInputCalibrationTrainButton?.addEventListener("click", openUserInputCalibrationTrainingModal);
  userInputCalibrationDeleteButton?.addEventListener("click", () => void deleteActiveUserInputCalibrationProfile());
  userInputCalibrationToolbarTrigger?.addEventListener("click", () => toggleUserInputCalibrationToolbarMenu());
  // The control bar's Pickup Match indicator opens the flyout on its tab, or closes it there.
  pickupMatchIndicator()?.addEventListener("click", () => {
    if (isUserInputCalibrationToolbarMenuOpen() && activeToolbarTab === "pickup") {
      closeUserInputCalibrationToolbarMenu();
      return;
    }
    openUserInputCalibrationToolbarMenu("pickup");
  });
  userInputCalibrationToolbarMenu?.addEventListener("click", (event) => void handleUserInputCalibrationToolbarMenuClick(event));
  userInputCalibrationCloseButton?.addEventListener("click", closeUserInputCalibrationTrainingModal);
  userInputCalibrationCancelButton?.addEventListener("click", closeUserInputCalibrationTrainingModal);
  userInputCalibrationResetButton?.addEventListener("click", resetUserInputCalibrationTrainingPeak);
  userInputCalibrationSaveButton?.addEventListener("click", saveUserInputCalibrationProfile);
  userInputCalibrationNameInput?.addEventListener("input", refreshUserInputCalibrationTrainingView);
  userInputCalibrationDescriptionInput?.addEventListener("input", refreshUserInputCalibrationTrainingView);
  userInputCalibrationModal?.addEventListener("mousedown", (event) => {
    if (event.target === userInputCalibrationModal) {
      closeUserInputCalibrationTrainingModal();
    }
  });
  document.addEventListener("mousedown", (event) => {
    const target = event.target;
    if (!(target instanceof Node)) {
      return;
    }

    if (userInputCalibrationToolbarMenu?.contains(target) || userInputCalibrationToolbarTrigger?.contains(target)
        || pickupMatchIndicator()?.contains(target)) {
      return;
    }

    // A popover the flyout opened (a Pickup Match picker) hangs from the body, outside it, and
    // so does a confirmation it asked for (deleting a profile or a guitar).
    const element = target instanceof Element ? target : target.parentElement;
    if (element?.closest("[data-control-bar-popover], #dialog-modal")) {
      return;
    }

    closeUserInputCalibrationToolbarMenu();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") {
      return;
    }

    if (isUserInputCalibrationToolbarMenuOpen()) {
      closeUserInputCalibrationToolbarMenu();
    }

    if (isUserInputCalibrationModalOpen()) {
      closeUserInputCalibrationTrainingModal();
    }
  });
  window.addEventListener("resize", () => {
    if (isUserInputCalibrationToolbarMenuOpen()) {
      positionUserInputCalibrationToolbarMenu();
    }
  });
  refreshUserInputCalibrationView();
}
