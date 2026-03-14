import { DEMO_AUDIO_SAMPLES, uiState } from "./state.js";
import { arrayBufferToBase64, parseWavMetadata, resolveDemoSamplePath } from "./utils.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { postMessage } from "./bridge.js";
import type { DemoSample } from "./types.js";

// Track whether demo audio is currently playing
let demoAudioPlaying = false;

type DemoAudioSource =
  | { id: string; title: string; kind: "builtin"; path: string }
  | { id: string; title: string; kind: "riff"; takeId: string };

function getDemoAudioSources(): DemoAudioSource[] {
  const builtins: DemoAudioSource[] = DEMO_AUDIO_SAMPLES.map((sample) => ({
    id: sample.id,
    title: sample.title,
    kind: "builtin",
    path: sample.path,
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

type DemoAudioBindConfig = {
  selectId: string;
  playId: string;
  repeatId: string;
  syncSelectId?: string;
  syncRepeatId?: string;
};

function bindDemoAudioControlsSet(config: DemoAudioBindConfig): void {
  const selectElement = document.getElementById(config.selectId) as HTMLSelectElement | null;
  if (selectElement) {
    selectElement.value = uiState.demoAudioSelectedId ?? selectElement.value;
    selectElement.addEventListener("change", (event) => {
      const value = (event.target as HTMLSelectElement).value;
      uiState.demoAudioSelectedId = value;
      if (config.syncSelectId) {
        const syncSelect = document.getElementById(config.syncSelectId) as HTMLSelectElement | null;
        if (syncSelect) {
          syncSelect.value = value;
        }
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
}

/**
 * Renders compact demo audio controls for the footer bar.
 * Returns an HTML string with select, play, and repeat controls.
 */
export function renderFooterDemoAudioControls(): string {
  if (!getDemoAudioSources().length) {
    return "";
  }
  const options = renderDemoAudioOptions();

  return `
    <div class="footer-demo-controls">
      <select id="footer-demo-audio-select" class="footer-demo-select themed-select" title="Select demo audio">
        ${options}
      </select>
      <button id="footer-play-demo-audio" class="footer-play-btn" title="Play demo audio">
        <span class="play-icon">▶</span>
      </button>
      <button id="footer-demo-audio-repeat" class="footer-repeat-btn" title="Repeat demo audio" aria-pressed="false">
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M7 6h9a4 4 0 0 1 0 8h-3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M7 6L4 9l3 3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M17 18H8a4 4 0 0 1 0-8h3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M17 18l3-3-3-3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
        </svg>
      </button>
    </div>
  `;
}

/**
 * Binds event listeners for the footer demo audio controls.
 * Should be called after the footer HTML is rendered.
 */
export function bindFooterDemoAudioControls(): void {
  bindDemoAudioControlsSet({
    selectId: "footer-demo-audio-select",
    playId: "footer-play-demo-audio",
    repeatId: "footer-demo-audio-repeat",
    syncSelectId: "demo-audio-select",
    syncRepeatId: "demo-audio-repeat-checkbox",
  });
}

export function renderDemoAudioControls(): string {
  if (!getDemoAudioSources().length) {
    return "";
  }
  const options = renderDemoAudioOptions();

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
        <button id="play-demo-audio" class="play-btn">
          <span class="play-icon">▶</span>
          Play
        </button>
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
  });
}

export async function previewSelectedDemoAudio(): Promise<void> {
  const sample = getSelectedDemoAudio();
  if (!sample) {
    showNotification("No demo audio available");
    return;
  }

  try {
    if (sample.kind === "riff") {
      postMessage({
        type: "previewRiffTake",
        takeId: sample.takeId,
        enableGuidance: false,
      });
      showNotification("Starting riff preview", sample.title);
      appendLog(`riff preview sent → ${sample.takeId}`);
      return;
    }

    const demoSample = sample as DemoSample;
    const resolvedPath = resolveDemoSamplePath(sample.path);
    if (!resolvedPath) {
      throw new Error("Demo audio path is not set");
    }

    appendLog(`preview start → ${resolvedPath}`);
    const response = await fetch(resolvedPath);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const buffer = await response.arrayBuffer();
    const base64 = arrayBufferToBase64(buffer);
    const metadata = parseWavMetadata(buffer);

    const audioPayload: Record<string, unknown> = {
      id: demoSample.id,
      title: demoSample.title,
      path: demoSample.path,
      size: buffer.byteLength,
      contentType: "audio/wav",
      data: base64,
    };

    if (metadata) {
      audioPayload.sampleRate = metadata.sampleRate;
      audioPayload.channels = metadata.channels;
      audioPayload.bitsPerSample = metadata.bitsPerSample;
    }

    postMessage({
      type: "previewDemoAudio",
      audio: audioPayload,
    });

    showNotification("Starting demo preview", sample.title);
    appendLog(`preview sent → ${sample.title}`);
  } catch (error) {
    console.error("Failed to preview demo audio", error);
    appendLog(`preview error ← ${sample.title}: ${error instanceof Error ? error.message : String(error)}`);
    showNotification("Failed to preview demo audio", error instanceof Error ? error.message : String(error));
  }
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
  uiState.demoAudioSelectedId = selectedId;

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

/**
 * Stop the currently playing demo audio.
 */
export function stopDemoAudio(): void {
  postMessage({ type: "stopDemoAudio" });
  appendLog("stop demo audio requested");
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
  const playIcon = playing ? "⏹" : "▶";
  const playTitle = playing ? "Stop demo audio" : "Play demo audio";
  
  // Update footer play button
  const footerBtn = document.getElementById("footer-play-demo-audio");
  if (footerBtn) {
    const iconSpan = footerBtn.querySelector(".play-icon");
    if (iconSpan) {
      iconSpan.textContent = playIcon;
    }
    footerBtn.title = playTitle;
    footerBtn.classList.toggle("is-playing", playing);
  }
  
  // Update main play button
  const mainBtn = document.getElementById("play-demo-audio");
  if (mainBtn) {
    const iconSpan = mainBtn.querySelector(".play-icon");
    if (iconSpan) {
      iconSpan.textContent = playIcon;
    }
    // Update the button text too
    mainBtn.innerHTML = `<span class="play-icon">${playIcon}</span> ${playing ? "Stop" : "Play"}`;
    mainBtn.title = playTitle;
    mainBtn.classList.toggle("is-playing", playing);
  }
}
