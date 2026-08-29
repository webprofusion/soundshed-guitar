/**
 * Getting an audio file into the Practice Tool: the waveform card's drop zone,
 * and the "this resets your settings" gate the Browse button shares with it.
 *
 * Split out of practiceTool.ts so the import paths stay one concern; the
 * facade still owns everything that happens *after* the engine reports the
 * file loaded.
 */

import { loadPracticeToolFile, loadPracticeToolFileData } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { uiState } from "../state.js";
import { arrayBufferToBase64 } from "../utils.js";

// WebView2 is standard Chromium — a dropped File's real filesystem path is
// never available to JS (that's an Electron-only extension), so this is
// only ever populated in environments where it happens to exist; the drop
// handler below falls back to reading bytes directly, which is what
// actually works here. See the "Dropped-file paths" note in
// .github/copilot-instructions.md.
function readDroppedFilePath(file: File): string | null {
  const withPath = file as File & { path?: string };
  return typeof withPath.path === "string" && withPath.path ? withPath.path : null;
}

const SUPPORTED_AUDIO_DROP_EXTENSIONS = [".wav", ".aiff", ".aif", ".mp3"];

function hasSupportedAudioExtension(fileName: string): boolean {
  const lower = fileName.trim().toLowerCase();
  return SUPPORTED_AUDIO_DROP_EXTENSIONS.some((ext) => lower.endsWith(ext));
}

/** Loading a new file resets Volume/Balance/Speed/Pitch back to their
 * defaults (see applyPracticeToolFileLoaded) — ask first, unless there's
 * nothing currently loaded to lose. Shared by both load entry points
 * (Browse File and drag-and-drop) so neither can bypass the other's gate.
 *
 * Recalling a saved project is deliberately *not* gated by this: it replaces
 * the settings with a named set the user explicitly picked, rather than
 * silently dropping them on the floor, so there is nothing to warn about. */
export async function confirmResetIfNeeded(): Promise<boolean> {
  const player = uiState.practiceTool;
  if (!player?.filePath) {
    return true;
  }
  return showConfirm(
    "Loading a new file will reset the current session — Volume, Balance, Speed, and Pitch will return to their defaults. Save it as a project first if you want it back. Continue?",
    "Load New File"
  );
}

export function bindPracticeToolDropZone(): void {
  const dropZone = document.getElementById("practice-tool-waveform-wrap");
  if (!dropZone || dropZone.dataset.bound === "true") {
    return;
  }
  dropZone.dataset.bound = "true";

  dropZone.addEventListener("dragover", (event) => {
    event.preventDefault();
    dropZone.classList.add("is-drag-over");
  });
  dropZone.addEventListener("dragleave", () => {
    dropZone.classList.remove("is-drag-over");
  });
  dropZone.addEventListener("drop", (event) => {
    event.preventDefault();
    dropZone.classList.remove("is-drag-over");
    const file = event.dataTransfer?.files?.[0];
    if (!file) {
      return;
    }
    if (!hasSupportedAudioExtension(file.name)) {
      showNotification("Unsupported file", "Drop a WAV, AIFF, or MP3 file");
      return;
    }

    void confirmResetIfNeeded().then((proceed) => {
      if (!proceed) {
        return;
      }

      const path = readDroppedFilePath(file);
      if (path) {
        loadPracticeToolFile(path);
        appendLog(`practice tool load requested (drop, path) → ${path}`);
        return;
      }

      void file
        .arrayBuffer()
        .then((buffer) => {
          loadPracticeToolFileData(file.name, arrayBufferToBase64(buffer));
          appendLog(`practice tool load requested (drop, data) → ${file.name}`);
        })
        .catch((error) => {
          showNotification("Unable to read dropped file", error instanceof Error ? error.message : String(error));
        });
    });
  });
}

