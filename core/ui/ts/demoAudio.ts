import { getActivePresetForRender, uiState } from "./state.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { postMessage, renderDemoAudio, requestCaptureDebugSnapshot } from "./bridge.js";
import { updateAppSetting } from "./appSettingsStore.js";
import { sanitizeFilename } from "./archiveUtils.js";
import type { DemoClip } from "./types.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import { getPlaySvg, getStopSvg } from "./iconAssets.js";

// Track whether demo audio is currently playing
let demoAudioPlaying = false;
/** The engine's demo clips (state.demoClips). It reads, plays and renders them itself, by id. */
let demoClips: DemoClip[] = [];
let openDemoActionsButton: HTMLButtonElement | null = null;
let openDemoActionsMenu: HTMLElement | null = null;

const DEMO_AUDIO_SELECTED_ID_SETTING = "demoAudio.selectedId";
const DEMO_AUDIO_RENDER_SAMPLE_RATE_SETTING = "demoAudio.renderSampleRate";
const DEMO_RENDER_SAMPLE_RATE_OPTIONS = [
  { value: 0, label: "Device rate" },
  { value: 44100, label: "44.1 kHz" },
  { value: 48000, label: "48 kHz" },
  { value: 88200, label: "88.2 kHz" },
  { value: 96000, label: "96 kHz" },
  { value: 176400, label: "176.4 kHz" },
  { value: 192000, label: "192 kHz" },
] as const;

type DemoAudioSource =
  | { id: string; title: string; kind: "builtin" }
  | { id: string; title: string; kind: "riff"; takeId: string };

function getDemoAudioSources(): DemoAudioSource[] {
  const builtins: DemoAudioSource[] = demoClips.map((clip) => ({
    id: clip.id,
    title: clip.title,
    kind: "builtin",
  }));

  const riffs = uiState.riffLibrary?.riffs ?? [];
  const favorites: DemoAudioSource[] = riffs
    .filter((riff) => Boolean(riff.favorite) && Array.isArray(riff.takes) && riff.takes.length > 0)
    .map((riff) => {
      const preferredTakeId = riff.preferredTakeId && riff.takes.some((take) => take.id === riff.preferredTakeId)
        ? riff.preferredTakeId
        : riff.takes[0].id;
      return {
        id: `riff:${preferredTakeId}`,
        title: `★ ${riff.title}`,
        kind: "riff",
        takeId: preferredTakeId,
      };
    });

  return [...builtins, ...favorites];
}

function getSelectedDemoAudio(): DemoAudioSource | null {
  const sources = getDemoAudioSources();
  if (!sources.length) {
    return null;
  }
  const selectedId = uiState.demoAudioSelectedId ?? sources[0].id;
  return sources.find((sample) => sample.id === selectedId) ?? sources[0];
}

function normalizeDemoAudioSourceId(id: string | null | undefined): string | null {
  if (!id) {
    return null;
  }

  const sources = getDemoAudioSources();
  if (sources.some((sample) => sample.id === id)) {
    return id;
  }

  const riffId = `riff:${id}`;
  if (sources.some((sample) => sample.id === riffId)) {
    return riffId;
  }

  return null;
}

function renderDemoAudioOptions(): string {
  const sources = getDemoAudioSources();
  if (!sources.length) {
    return "";
  }
  const selectedId = uiState.demoAudioSelectedId ?? sources[0].id;
  return sources
    .map((sample) => {
      const selected = sample.id === selectedId;
      return `<option value="${sample.id}"${selected ? " selected" : ""}>${sample.title}</option>`;
    })
    .join("");
}

function normalizeDemoRenderSampleRate(value: unknown): number {
  const numericValue = typeof value === "number"
    ? value
    : typeof value === "string"
      ? Number(value)
      : 0;
  if (!Number.isFinite(numericValue)) {
    return 0;
  }

  const rounded = Math.round(numericValue);
  return DEMO_RENDER_SAMPLE_RATE_OPTIONS.some((option) => option.value === rounded) ? rounded : 0;
}

function getDemoRenderSampleRate(): number {
  return normalizeDemoRenderSampleRate(uiState.appSettings?.[DEMO_AUDIO_RENDER_SAMPLE_RATE_SETTING]);
}

function formatDemoRenderSampleRate(sampleRate: number): string {
  const normalized = normalizeDemoRenderSampleRate(sampleRate);
  return DEMO_RENDER_SAMPLE_RATE_OPTIONS.find((option) => option.value === normalized)?.label ?? "Device rate";
}

function renderDemoRenderSampleRateOptions(): string {
  const selectedRate = getDemoRenderSampleRate();
  return DEMO_RENDER_SAMPLE_RATE_OPTIONS
    .map((option) => `<option value="${option.value}"${option.value === selectedRate ? " selected" : ""}>${option.label}</option>`)
    .join("");
}

function persistDemoRenderSampleRate(sampleRate: number): void {
  const normalized = normalizeDemoRenderSampleRate(sampleRate);
  updateAppSetting(DEMO_AUDIO_RENDER_SAMPLE_RATE_SETTING, normalized);
}

function refreshDemoRenderSampleRateSelectors(): void {
  const selectedRate = String(getDemoRenderSampleRate());
  const mainSelect = document.getElementById("demo-render-sample-rate") as HTMLSelectElement | null;
  if (mainSelect) {
    mainSelect.value = selectedRate;
  }

  const footerSelect = document.getElementById("footer-demo-render-sample-rate") as HTMLSelectElement | null;
  if (footerSelect) {
    footerSelect.value = selectedRate;
  }
}

function persistDemoAudioSelection(selectedId: string | null): void {
  updateAppSetting(DEMO_AUDIO_SELECTED_ID_SETTING, selectedId);
}

function getStoredDemoAudioSelectionId(): string | null {
  const stored = uiState.appSettings?.[DEMO_AUDIO_SELECTED_ID_SETTING];
  return typeof stored === "string" && stored.trim().length > 0 ? stored : null;
}

type DemoAudioBindConfig = {
  selectId: string;
  playId: string;
  repeatId: string;
  syncSelectId?: string;
  syncRepeatId?: string;
  actionsButtonId?: string;
  actionsMenuId?: string;
  renderActionId?: string;
  renderSampleRateId?: string;
  syncRenderSampleRateId?: string;
};

function closeDemoActionsMenu(refocusButton = false): void {
  if (!openDemoActionsButton || !openDemoActionsMenu) {
    openDemoActionsButton = null;
    openDemoActionsMenu = null;
    return;
  }

  openDemoActionsButton.setAttribute("aria-expanded", "false");
  openDemoActionsMenu.classList.remove("open", "drop-up");
  openDemoActionsMenu.setAttribute("aria-hidden", "true");

  const buttonToFocus = refocusButton ? openDemoActionsButton : null;
  openDemoActionsButton = null;
  openDemoActionsMenu = null;

  document.removeEventListener("click", handleDemoActionsDocumentClick);
  document.removeEventListener("keydown", handleDemoActionsDocumentKeydown);

  if (buttonToFocus) {
    buttonToFocus.focus();
  }
}

function handleDemoActionsDocumentClick(event: MouseEvent): void {
  if (!openDemoActionsButton || !openDemoActionsMenu) {
    return;
  }

  const target = event.target as Node | null;
  if (!target) {
    closeDemoActionsMenu();
    return;
  }

  if (openDemoActionsButton.contains(target) || openDemoActionsMenu.contains(target)) {
    return;
  }

  closeDemoActionsMenu();
}

function handleDemoActionsDocumentKeydown(event: KeyboardEvent): void {
  if (event.key !== "Escape") {
    return;
  }

  if (!openDemoActionsMenu) {
    return;
  }

  event.preventDefault();
  closeDemoActionsMenu(true);
}

function openActionsMenu(button: HTMLButtonElement, menu: HTMLElement): void {
  if (openDemoActionsMenu && openDemoActionsMenu !== menu) {
    closeDemoActionsMenu();
  }

  const rect = button.getBoundingClientRect();
  const menuHeight = menu.offsetHeight || 44;
  const availableBelow = window.innerHeight - rect.bottom;
  menu.classList.toggle("drop-up", availableBelow < menuHeight + 12 && rect.top > availableBelow);
  menu.classList.add("open");
  menu.setAttribute("aria-hidden", "false");
  button.setAttribute("aria-expanded", "true");

  openDemoActionsButton = button;
  openDemoActionsMenu = menu;

  document.addEventListener("click", handleDemoActionsDocumentClick);
  document.addEventListener("keydown", handleDemoActionsDocumentKeydown);
}

function toggleActionsMenu(button: HTMLButtonElement, menu: HTMLElement): void {
  if (openDemoActionsMenu === menu) {
    closeDemoActionsMenu();
    return;
  }

  openActionsMenu(button, menu);
}

function buildDemoRenderSuggestedName(sample: DemoAudioSource): string {
  const sourceName = sample.title.replace(/^★\s*/, "").trim();
  const presetName = getActivePresetForRender()?.name?.trim() || "current-preset";
  return `${sanitizeFilename(presetName, "current-preset")}-${sanitizeFilename(sourceName || "demo-audio", "demo-audio")}.wav`;
}

function bindDemoAudioControlsSet(config: DemoAudioBindConfig): void {
  const selectElement = document.getElementById(config.selectId) as HTMLSelectElement | null;
  if (selectElement) {
    selectElement.value = uiState.demoAudioSelectedId ?? selectElement.value;
    selectElement.addEventListener("change", async (event) => {
      const value = (event.target as HTMLSelectElement).value;
      uiState.demoAudioSelectedId = value;
      persistDemoAudioSelection(value);
      if (config.syncSelectId) {
        const syncSelect = document.getElementById(config.syncSelectId) as HTMLSelectElement | null;
        if (syncSelect) {
          syncSelect.value = value;
        }
      }

      if (demoAudioPlaying) {
        stopDemoAudio();
        await previewSelectedDemoAudio();
      }
    });
  }

  const playButton = document.getElementById(config.playId);
  if (playButton) {
    playButton.addEventListener("click", async () => {
      if (demoAudioPlaying) {
        stopDemoAudio();
      } else {
        await previewSelectedDemoAudio();
      }
    });
  }

  const repeatElement = document.getElementById(config.repeatId);
  if (repeatElement) {
    const isCheckbox = repeatElement instanceof HTMLInputElement;
    const setRepeatState = (enabled: boolean) => {
      uiState.demoAudioRepeat = enabled;
      if (isCheckbox) {
        (repeatElement as HTMLInputElement).checked = enabled;
      } else {
        repeatElement.classList.toggle("is-active", enabled);
        repeatElement.setAttribute("aria-pressed", enabled ? "true" : "false");
      }
      if (config.syncRepeatId) {
        const syncRepeat = document.getElementById(config.syncRepeatId) as HTMLInputElement | null;
        if (syncRepeat) {
          syncRepeat.checked = enabled;
        }
      }
    };

    setRepeatState(uiState.demoAudioRepeat);

    if (isCheckbox) {
      repeatElement.addEventListener("change", (event) => {
        setRepeatState((event.target as HTMLInputElement).checked);
      });
    } else {
      repeatElement.addEventListener("click", () => {
        setRepeatState(!uiState.demoAudioRepeat);
      });
    }
  }

  if (config.actionsButtonId && config.actionsMenuId && config.renderActionId) {
    const actionsButton = document.getElementById(config.actionsButtonId) as HTMLButtonElement | null;
    const actionsMenu = document.getElementById(config.actionsMenuId) as HTMLElement | null;
    const renderAction = document.getElementById(config.renderActionId) as HTMLButtonElement | null;

    if (actionsButton && actionsMenu && renderAction) {
      actionsButton.addEventListener("click", (event) => {
        event.stopPropagation();
        toggleActionsMenu(actionsButton, actionsMenu);
      });

      actionsMenu.addEventListener("click", (event) => {
        event.stopPropagation();
      });

      renderAction.addEventListener("click", async (event) => {
        event.stopPropagation();
        closeDemoActionsMenu();
        await renderSelectedDemoAudio();
      });
    }
  }

  if (config.renderSampleRateId) {
    const renderSampleRateSelect = document.getElementById(config.renderSampleRateId) as HTMLSelectElement | null;
    if (renderSampleRateSelect) {
      renderSampleRateSelect.value = String(getDemoRenderSampleRate());
      renderSampleRateSelect.addEventListener("change", (event) => {
        const sampleRate = normalizeDemoRenderSampleRate((event.target as HTMLSelectElement).value);
        persistDemoRenderSampleRate(sampleRate);
        renderSampleRateSelect.value = String(sampleRate);
        if (config.syncRenderSampleRateId) {
          const syncSelect = document.getElementById(config.syncRenderSampleRateId) as HTMLSelectElement | null;
          if (syncSelect) {
            syncSelect.value = String(sampleRate);
          }
        }
      });
    }
  }
}

/**
 * Renders compact demo audio controls for the footer bar.
 * Returns an HTML string with select, play, and repeat controls.
 */
function renderFooterDemoAudioControls(): string {
  if (!getDemoAudioSources().length) {
    return "";
  }
  const options = renderDemoAudioOptions();
  const sampleRateOptions = renderDemoRenderSampleRateOptions();
  const debugCaptureHidden = isFeatureEnabled(Features.DebugStateCapture) ? "" : " hidden";

  return `
    <div class="footer-demo-controls">
      <select id="footer-demo-audio-select" class="footer-demo-select themed-select" title="Select demo audio">
        ${options}
      </select>
      <button id="footer-play-demo-audio" class="footer-play-btn" title="Play demo audio">
        ${getPlaySvg()}
      </button>
      <button id="footer-demo-audio-repeat" class="footer-repeat-btn" title="Repeat demo audio" aria-pressed="false">
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M7 6h9a4 4 0 0 1 0 8h-3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M7 6L4 9l3 3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M17 18H8a4 4 0 0 1 0-8h3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M17 18l3-3-3-3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
        </svg>
      </button>
      <div class="footer-demo-actions-wrap demo-actions-wrap">
        <button
          id="footer-demo-audio-actions-button"
          class="footer-demo-actions-button"
          type="button"
          title="Demo audio actions"
          aria-label="Demo audio actions"
          aria-haspopup="menu"
          aria-expanded="false"
          aria-controls="footer-demo-audio-actions-menu"
        >
          <span aria-hidden="true">...</span>
        </button>
        <div class="demo-audio-actions-menu footer-demo-audio-actions-menu" id="footer-demo-audio-actions-menu" role="menu" aria-hidden="true">
          <label class="demo-render-rate-control" title="Render sample rate">
            <span>Rate</span>
            <select id="footer-demo-render-sample-rate" class="demo-render-rate-select themed-select" aria-label="Render sample rate">
              ${sampleRateOptions}
            </select>
          </label>
          <button
            id="footer-render-demo-audio"
            class="demo-audio-action-item"
            type="button"
            role="menuitem"
            title="Render the selected demo audio with the current preset"
          >
            Render WAV
          </button>
        </div>
      </div>
      <button
        id="footer-capture-debug-state-btn"
        class="footer-debug-capture-btn"
        type="button"
        title="Capture the current UI and backend debug state"
        aria-label="Capture debug state"
        ${debugCaptureHidden}
      >
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M8 7.5h2l1.2-1.8a1 1 0 0 1 .83-.45h3.94a1 1 0 0 1 .83.45L18 7.5h1a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-7a2 2 0 0 1 2-2h1.2" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <circle cx="12" cy="13" r="3.25" fill="none" stroke="currentColor" stroke-width="1.6"/>
          <path d="M12 3.75v2.5M12 19.75v.5" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>
        </svg>
      </button>
    </div>
  `;
}

/**
 * Draws the footer's demo audio controls into their container and binds them. Called at
 * startup, and again when the engine's clip list arrives if the footer was drawn without one.
 */
export function renderFooterDemoAudio(): void {
  const container = document.getElementById("footer-demo-audio-container");
  if (!container) {
    return;
  }
  container.innerHTML = renderFooterDemoAudioControls();
  bindFooterDemoAudioControls();
}

/**
 * Takes the engine's demo clip list (state.demoClips), then redraws what shows it. The
 * footer is drawn at startup, before the first state arrives, so it may still be empty.
 */
export function applyDemoClips(clips: unknown): void {
  if (!Array.isArray(clips)) {
    return;
  }
  demoClips = clips.flatMap((clip): DemoClip[] => {
    const { id, title } = (clip ?? {}) as { id?: unknown; title?: unknown };
    return typeof id === "string" && id ? [{ id, title: typeof title === "string" && title ? title : id }] : [];
  });

  if (!document.getElementById("footer-demo-audio-select")) {
    renderFooterDemoAudio();
  }
  refreshDemoAudioSelectors();
}

/**
 * Binds event listeners for the footer demo audio controls.
 * Should be called after the footer HTML is rendered.
 */
function bindFooterDemoAudioControls(): void {
  bindDemoAudioControlsSet({
    selectId: "footer-demo-audio-select",
    playId: "footer-play-demo-audio",
    repeatId: "footer-demo-audio-repeat",
    syncSelectId: "demo-audio-select",
    syncRepeatId: "demo-audio-repeat-checkbox",
    actionsButtonId: "footer-demo-audio-actions-button",
    actionsMenuId: "footer-demo-audio-actions-menu",
    renderActionId: "footer-render-demo-audio",
    renderSampleRateId: "footer-demo-render-sample-rate",
    syncRenderSampleRateId: "demo-render-sample-rate",
  });

  const captureButton = document.getElementById("footer-capture-debug-state-btn") as HTMLButtonElement | null;
  if (captureButton && captureButton.dataset.bound !== "true") {
    captureButton.dataset.bound = "true";
    captureButton.addEventListener("click", () => {
      requestCaptureDebugSnapshot("footer-button");
      showNotification("Capturing debug state", "Snapshot will be written to the app logs folder.");
    });
  }
}

export function renderDemoAudioControls(): string {
  if (!getDemoAudioSources().length) {
    return "";
  }
  const options = renderDemoAudioOptions();
  const sampleRateOptions = renderDemoRenderSampleRateOptions();

  return `
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">🎵</span>
        Demo Audio
      </h3>
      <div class="demo-controls">
        <select id="demo-audio-select" class="themed-select">
          ${options}
        </select>
        <button id="play-demo-audio" class="btn btn-primary">
          ${getPlaySvg()}
          Play
        </button>
        <div class="demo-actions-wrap">
          <button
            id="demo-audio-actions-button"
            class="demo-actions-button"
            type="button"
            title="Demo audio actions"
            aria-label="Demo audio actions"
            aria-haspopup="menu"
            aria-expanded="false"
            aria-controls="demo-audio-actions-menu"
          >
            <span aria-hidden="true">...</span>
          </button>
          <div class="demo-audio-actions-menu" id="demo-audio-actions-menu" role="menu" aria-hidden="true">
            <label class="demo-render-rate-control" title="Render sample rate">
              <span>Rate</span>
              <select id="demo-render-sample-rate" class="demo-render-rate-select themed-select" aria-label="Render sample rate">
                ${sampleRateOptions}
              </select>
            </label>
            <button
              id="render-demo-audio"
              class="demo-audio-action-item"
              type="button"
              role="menuitem"
              title="Render the selected demo audio with the current preset"
            >
              Render WAV
            </button>
          </div>
        </div>
        <div class="toggle-control demo-repeat-control">
          <span class="toggle-label">REPEAT</span>
          <label class="toggle-switch">
            <input type="checkbox" id="demo-audio-repeat-checkbox" />
            <span class="toggle-slider"></span>
          </label>
        </div>
      </div>
    </div>
  `;
}

export function bindDemoAudioControls(): void {
  bindDemoAudioControlsSet({
    selectId: "demo-audio-select",
    playId: "play-demo-audio",
    repeatId: "demo-audio-repeat-checkbox",
    syncSelectId: "footer-demo-audio-select",
    syncRepeatId: "footer-demo-audio-repeat",
    actionsButtonId: "demo-audio-actions-button",
    actionsMenuId: "demo-audio-actions-menu",
    renderActionId: "render-demo-audio",
    renderSampleRateId: "demo-render-sample-rate",
    syncRenderSampleRateId: "footer-demo-render-sample-rate",
  });
}

export async function renderSelectedDemoAudio(): Promise<void> {
  const sample = getSelectedDemoAudio();
  if (!sample) {
    showNotification("No demo audio available");
    return;
  }

  const suggestedName = buildDemoRenderSuggestedName(sample);
  const renderSampleRate = getDemoRenderSampleRate();
  const renderRatePayload = renderSampleRate > 0 ? { renderSampleRate } : {};
  const renderRateLabel = formatDemoRenderSampleRate(renderSampleRate);

  if (sample.kind === "riff") {
    renderDemoAudio({
      takeId: sample.takeId,
      title: sample.title.replace(/^★\s*/, ""),
      suggestedName,
      ...renderRatePayload,
    });
    showNotification("Choose export location", sample.title.replace(/^★\s*/, ""));
    appendLog(`render demo audio requested → ${sample.takeId} @ ${renderRateLabel}`);
    return;
  }

  // The engine reads the clip itself; a failure comes back as "demoAudioRenderFailed".
  renderDemoAudio({
    clipId: sample.id,
    title: sample.title,
    suggestedName,
    ...renderRatePayload,
  });
  showNotification("Choose export location", sample.title);
  appendLog(`render demo audio requested → ${sample.title} @ ${renderRateLabel}`);
}

export async function previewSelectedDemoAudio(): Promise<void> {
  const sample = getSelectedDemoAudio();
  if (!sample) {
    showNotification("No demo audio available");
    return;
  }

  if (sample.kind === "riff") {
    // A library take always loops in the engine until it is stopped.
    postMessage({
      type: "previewRiffTake",
      takeId: sample.takeId,
      enableGuidance: false,
    });
    showNotification("Starting riff preview", sample.title);
    appendLog(`riff preview sent → ${sample.takeId}`);
    return;
  }

  // The engine reads the clip itself and, with repeat, loops it in place until it is
  // stopped; Repeat is taken when playback starts. A failure comes back as an "error".
  postMessage({
    type: "previewDemoAudio",
    clipId: sample.id,
    repeat: uiState.demoAudioRepeat,
  });

  showNotification("Starting demo preview", sample.title);
  appendLog(`preview sent → ${sample.title}`);
}

export function refreshDemoAudioSelectors(): void {
  const options = renderDemoAudioOptions();
  const sources = getDemoAudioSources();
  if (!sources.length) {
    return;
  }

  const selectedId = uiState.demoAudioSelectedId && sources.some((entry) => entry.id === uiState.demoAudioSelectedId)
    ? uiState.demoAudioSelectedId
    : sources[0].id;

  const mainSelect = document.getElementById("demo-audio-select") as HTMLSelectElement | null;
  if (mainSelect) {
    mainSelect.innerHTML = options;
    mainSelect.value = selectedId;
  }

  const footerSelect = document.getElementById("footer-demo-audio-select") as HTMLSelectElement | null;
  if (footerSelect) {
    footerSelect.innerHTML = options;
    footerSelect.value = selectedId;
  }
}

export function syncDemoAudioSelectionFromPreview(previewId: string | null | undefined): void {
  const normalizedId = normalizeDemoAudioSourceId(previewId);
  if (!normalizedId) {
    return;
  }

  uiState.demoAudioSelectedId = normalizedId;
  persistDemoAudioSelection(normalizedId);

  const mainSelect = document.getElementById("demo-audio-select") as HTMLSelectElement | null;
  if (mainSelect) {
    mainSelect.value = normalizedId;
  }

  const footerSelect = document.getElementById("footer-demo-audio-select") as HTMLSelectElement | null;
  if (footerSelect) {
    footerSelect.value = normalizedId;
  }
}

/**
 * Stop the currently playing demo audio.
 */
export function stopDemoAudio(): void {
  closeDemoActionsMenu();
  postMessage({ type: "stopDemoAudio" });
  appendLog("stop demo audio requested");
}

export function applyStoredDemoAudioSelection(): void {
  const storedSelection = getStoredDemoAudioSelectionId();
  if (storedSelection) {
    uiState.demoAudioSelectedId = storedSelection;
    refreshDemoAudioSelectors();
  }

  refreshDemoRenderSampleRateSelectors();
}

/**
 * Called when demo audio playback starts.
 */
export function onDemoAudioStarted(): void {
  demoAudioPlaying = true;
  updatePlayButtonIcons(true);
}

/**
 * Called when demo audio playback stops or completes.
 */
export function onDemoAudioStopped(): void {
  demoAudioPlaying = false;
  updatePlayButtonIcons(false);
}

/**
 * Check if demo audio is currently playing.
 */
export function isDemoAudioPlaying(): boolean {
  return demoAudioPlaying;
}

/**
 * Update all play button icons to show play or stop state.
 */
function updatePlayButtonIcons(playing: boolean): void {
  const playTitle = playing ? "Stop demo audio" : "Play demo audio";
  
  // Update footer play button
  const footerBtn = document.getElementById("footer-play-demo-audio");
  if (footerBtn) {
    const iconSpan = footerBtn.querySelector("svg");
    if (iconSpan) {
      iconSpan.remove();
    }
    footerBtn.insertAdjacentHTML('afterbegin', playing ? getStopSvg() : getPlaySvg());
    footerBtn.title = playTitle;
    footerBtn.classList.toggle("is-playing", playing);
  }
  
  // Update main play button
  const mainBtn = document.getElementById("play-demo-audio");
  if (mainBtn) {
    mainBtn.innerHTML = `${playing ? getStopSvg() : getPlaySvg()} ${playing ? "Stop" : "Play"}`;
    mainBtn.title = playTitle;
    mainBtn.classList.toggle("is-playing", playing);
  }
}
