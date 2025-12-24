import { DEMO_AUDIO_SAMPLES, uiState } from "./state.js";
import { arrayBufferToBase64, parseWavMetadata, resolveDemoSamplePath } from "./utils.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { postMessage } from "./bridge.js";
import type { DemoSample } from "./types.js";

function getSelectedDemoAudio(): DemoSample | null {
  if (!DEMO_AUDIO_SAMPLES.length) {
    return null;
  }
  const selectedId = uiState.demoAudioSelectedId ?? DEMO_AUDIO_SAMPLES[0].id;
  return DEMO_AUDIO_SAMPLES.find((sample) => sample.id === selectedId) ?? DEMO_AUDIO_SAMPLES[0];
}

export function renderDemoAudioControls(): string {
  if (!DEMO_AUDIO_SAMPLES.length) {
    return "";
  }

  const options = DEMO_AUDIO_SAMPLES
    .map((sample) => {
      const selected = sample.id === (uiState.demoAudioSelectedId ?? DEMO_AUDIO_SAMPLES[0].id);
      return `<option value="${sample.id}"${selected ? " selected" : ""}>${sample.title}</option>`;
    })
    .join("");

  return `
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">🎵</span>
        Demo Audio
      </h3>
      <div class="demo-controls">
        <select id="demo-audio-select" class="demo-select">
          ${options}
        </select>
        <button id="play-demo-audio" class="play-btn">
          <span class="play-icon">▶</span>
          Play
        </button>
        <label class="repeat-toggle">
          <input type="checkbox" id="demo-audio-repeat-checkbox" />
          <span class="repeat-label">🔁 Repeat</span>
        </label>
      </div>
    </div>
  `;
}

export function bindDemoAudioControls(): void {
  const selectElement = document.getElementById("demo-audio-select") as HTMLSelectElement | null;
  if (selectElement) {
    selectElement.value = uiState.demoAudioSelectedId ?? selectElement.value;
    selectElement.addEventListener("change", (event) => {
      const value = (event.target as HTMLSelectElement).value;
      uiState.demoAudioSelectedId = value;
    });
  }

  const playButton = document.getElementById("play-demo-audio");
  if (playButton) {
    playButton.addEventListener("click", async () => {
      await previewSelectedDemoAudio();
    });
  }

  const repeatCheckbox = document.getElementById("demo-audio-repeat-checkbox") as HTMLInputElement | null;
  if (repeatCheckbox) {
    repeatCheckbox.checked = uiState.demoAudioRepeat;
    repeatCheckbox.addEventListener("change", (event) => {
      uiState.demoAudioRepeat = (event.target as HTMLInputElement).checked;
    });
  }
}

export async function previewSelectedDemoAudio(): Promise<void> {
  const sample = getSelectedDemoAudio();
  if (!sample) {
    showNotification("No demo audio available");
    return;
  }

  try {
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
      id: sample.id,
      title: sample.title,
      path: sample.path,
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
