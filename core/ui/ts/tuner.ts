import { postMessage } from "./bridge.js";
import { appendLog } from "./logging.js";

// Note names for display
const NOTE_NAMES = ["C", "C#/Db", "D", "D#/Eb", "E", "F", "F#/Gb", "G", "G#/Ab", "A", "A#/Bb", "B"];

// Get adjacent note names
function getAdjacentNotes(noteName: string): { left: string; right: string } {
  const trimmed = noteName.trim();
  if (!trimmed) {
    return { left: "-", right: "-" };
  }

  // Handle combined notation like "D#/Eb"
  const baseNote = trimmed.includes("/") ? trimmed.split("/")[0] : trimmed;
  const noteIndex = NOTE_NAMES.findIndex(n => n.startsWith(baseNote));
  
  if (noteIndex < 0) {
    return { left: "-", right: "-" };
  }
  
  const leftIndex = (noteIndex - 1 + 12) % 12;
  const rightIndex = (noteIndex + 1) % 12;
  
  return {
    left: NOTE_NAMES[leftIndex],
    right: NOTE_NAMES[rightIndex],
  };
}

// Tuner state
interface TunerState {
  isOpen: boolean;
  isLive: boolean;
  isMuted: boolean;
  displayMode: "cents" | "hz";
  referenceFrequency: number;
  lastDetection: TunerData | null;
}

interface TunerData {
  detected: boolean;
  noteName?: string;
  octave?: number;
  frequency?: number;
  centOffset?: number;
  confidence?: number;
}

interface TunerSample {
  centOffset: number;
  frequency: number;
  confidence: number;
}

const tunerState: TunerState = {
  isOpen: false,
  isLive: true,
  isMuted: false,
  displayMode: "cents",
  referenceFrequency: 440.0,
  lastDetection: null,
};

const tunerSampleBuffer: TunerSample[] = [];
const kTunerSampleWindow = 6;

function resetTunerSamples(): void {
  tunerSampleBuffer.length = 0;
}

function addTunerSample(sample: TunerSample): void {
  tunerSampleBuffer.push(sample);
  if (tunerSampleBuffer.length > kTunerSampleWindow) {
    tunerSampleBuffer.shift();
  }
}

function getAveragedSample(): TunerSample | null {
  if (!tunerSampleBuffer.length) return null;
  let centSum = 0;
  let freqSum = 0;
  let confSum = 0;
  for (const sample of tunerSampleBuffer) {
    centSum += sample.centOffset;
    freqSum += sample.frequency;
    confSum += sample.confidence;
  }
  const count = tunerSampleBuffer.length;
  return {
    centOffset: centSum / count,
    frequency: freqSum / count,
    confidence: confSum / count,
  };
}

// DOM elements
let tunerModal: HTMLElement | null = null;
let tunerCloseBtn: HTMLElement | null = null;
let tunerMuteBtn: HTMLElement | null = null;
let tunerLiveToggle: HTMLInputElement | null = null;
let tunerCentsBtn: HTMLElement | null = null;
let tunerHzBtn: HTMLElement | null = null;
let tunerRefInput: HTMLInputElement | null = null;
let tunerRefUpBtn: HTMLElement | null = null;
let tunerRefDownBtn: HTMLElement | null = null;
let tunerPitchIndicator: HTMLElement | null = null;
let tunerDisplay: HTMLElement | null = null;
let tunerCurrentNote: HTMLElement | null = null;
let tunerNoteLeft: HTMLElement | null = null;
let tunerNoteRight: HTMLElement | null = null;
let tunerCentsValue: HTMLElement | null = null;
let tunerCentsIndicator: HTMLElement | null = null;

export function initializeTuner(): void {
  // Cache DOM elements
  tunerModal = document.getElementById("tuner-modal");
  tunerCloseBtn = document.getElementById("tuner-close-btn");
  tunerMuteBtn = document.getElementById("tuner-mute-btn");
  tunerLiveToggle = document.getElementById("tuner-live-toggle") as HTMLInputElement;
  tunerCentsBtn = document.getElementById("tuner-cents-btn");
  tunerHzBtn = document.getElementById("tuner-hz-btn");
  tunerRefInput = document.getElementById("tuner-ref-freq") as HTMLInputElement;
  tunerRefUpBtn = document.getElementById("tuner-ref-up");
  tunerRefDownBtn = document.getElementById("tuner-ref-down");
  tunerPitchIndicator = document.getElementById("tuner-pitch-indicator");
  tunerDisplay = document.querySelector(".tuner-display") as HTMLElement;
  tunerCurrentNote = document.getElementById("tuner-current-note");
  tunerNoteLeft = document.getElementById("tuner-note-left");
  tunerNoteRight = document.getElementById("tuner-note-right");
  tunerCentsValue = document.getElementById("tuner-cents-value");
  tunerCentsIndicator = document.getElementById("tuner-cents-indicator");

  // Find and wire up the TUNER button in the footer
  const footerButtons = document.querySelectorAll(".footer-btn");
  console.log("[Tuner] Found", footerButtons.length, "footer buttons");
  let tunerButtonFound = false;
  footerButtons.forEach((btn) => {
    console.log("[Tuner] Button text:", JSON.stringify(btn.textContent));
    if (btn.textContent?.trim() === "TUNER") {
      console.log("[Tuner] Found TUNER button, adding click listener");
      btn.addEventListener("click", openTuner);
      tunerButtonFound = true;
    }
  });
  if (!tunerButtonFound) {
    console.error("[Tuner] TUNER button not found in footer!");
  }

  // Wire up tuner controls
  if (tunerCloseBtn) {
    tunerCloseBtn.addEventListener("click", closeTuner);
  }

  if (tunerMuteBtn) {
    tunerMuteBtn.addEventListener("click", toggleMute);
  }

  if (tunerLiveToggle) {
    tunerLiveToggle.addEventListener("change", toggleLive);
  }

  if (tunerCentsBtn) {
    tunerCentsBtn.addEventListener("click", () => setDisplayMode("cents"));
  }

  if (tunerHzBtn) {
    tunerHzBtn.addEventListener("click", () => setDisplayMode("hz"));
  }

  if (tunerRefInput) {
    tunerRefInput.addEventListener("change", onReferenceChange);
  }

  if (tunerRefUpBtn) {
    tunerRefUpBtn.addEventListener("click", () => adjustReference(0.1));
  }

  if (tunerRefDownBtn) {
    tunerRefDownBtn.addEventListener("click", () => adjustReference(-0.1));
  }

  // Close modal when clicking outside
  if (tunerModal) {
    tunerModal.addEventListener("mousedown", (e) => {
      if (e.target === tunerModal) {
        closeTuner();
      }
    });
  }

  // Keyboard shortcut to close
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && tunerState.isOpen) {
      closeTuner();
    }
  });

  appendLog("Tuner initialized");
}

export function openTuner(): void {
  if (!tunerModal) return;

  tunerModal.style.display = "flex";
  tunerState.isOpen = true;

  resetTunerSamples();

  // Always start the tuner when modal opens
  startTuner();
  
  // Also send the current live mode state
  postMessage({
    type: "tuner",
    action: "setLiveMode",
    liveMode: tunerState.isLive,
  });

  appendLog("Tuner opened");
}

export function closeTuner(): void {
  if (!tunerModal) return;

  tunerModal.style.display = "none";
  tunerState.isOpen = false;

  resetTunerSamples();

  // Stop the tuner
  stopTuner();

  appendLog("Tuner closed");
}

function startTuner(): void {
  const message = {
    type: "tuner",
    action: "start",
    referenceFrequency: tunerState.referenceFrequency,
  };
  console.log("[Tuner] Sending start message:", JSON.stringify(message));
  appendLog(`Tuner sending: ${JSON.stringify(message)}`);
  postMessage(message);
}

function stopTuner(): void {
  const message = {
    type: "tuner",
    action: "stop",
  };
  console.log("[Tuner] Sending stop message:", JSON.stringify(message));
  appendLog(`Tuner sending: ${JSON.stringify(message)}`);
  postMessage(message);
}

function toggleMute(): void {
  tunerState.isMuted = !tunerState.isMuted;
  
  if (tunerMuteBtn) {
    tunerMuteBtn.classList.toggle("muted", tunerState.isMuted);
  }
  
  // Note: The actual mute functionality would need to be implemented
  // in the DSP/plugin layer if we want to mute audio while tuning
  appendLog(`Tuner mute: ${tunerState.isMuted}`);
}

function toggleLive(): void {
  if (!tunerLiveToggle) return;

  tunerState.isLive = tunerLiveToggle.checked;

  // Send live mode change to plugin
  // When live mode is ON: audio passes through DSP while tuning
  // When live mode is OFF: output is silent while tuning (just tuner display)
  postMessage({
    type: "tuner",
    action: "setLiveMode",
    liveMode: tunerState.isLive,
  });

  appendLog(`Tuner live mode: ${tunerState.isLive ? "ON (audio through)" : "OFF (silent)"}`);
}

function setDisplayMode(mode: "cents" | "hz"): void {
  tunerState.displayMode = mode;

  if (tunerCentsBtn) {
    tunerCentsBtn.classList.toggle("active", mode === "cents");
  }

  if (tunerHzBtn) {
    tunerHzBtn.classList.toggle("active", mode === "hz");
  }

  // Update the display
  if (tunerState.lastDetection) {
    updateTunerDisplay(tunerState.lastDetection);
  }
}

function onReferenceChange(): void {
  if (!tunerRefInput) return;

  const value = parseFloat(tunerRefInput.value);
  if (isNaN(value) || value < 400 || value > 480) {
    tunerRefInput.value = tunerState.referenceFrequency.toFixed(1);
    return;
  }

  tunerState.referenceFrequency = value;

  postMessage({
    type: "tuner",
    action: "setReference",
    referenceFrequency: value,
  });

  appendLog(`Tuner reference: ${value} Hz`);
}

function adjustReference(delta: number): void {
  if (!tunerRefInput) return;

  const newValue = Math.max(400, Math.min(480, tunerState.referenceFrequency + delta));
  tunerState.referenceFrequency = newValue;
  tunerRefInput.value = newValue.toFixed(1);

  postMessage({
    type: "tuner",
    action: "setReference",
    referenceFrequency: newValue,
  });
}

export function handleTunerUpdate(data: TunerData): void {
  appendLog(`Tuner update: detected=${data.detected}, note=${data.noteName}, freq=${data.frequency?.toFixed(1)}, cents=${data.centOffset?.toFixed(1)}`);
  
  if (!tunerState.isOpen) {
    appendLog("Tuner update ignored - modal not open");
    return;
  }

  if (!data.detected) {
    resetTunerSamples();
    tunerState.lastDetection = data;
    updateTunerDisplay(data);
    return;
  }

  const centOffset = data.centOffset ?? 0;
  const frequency = data.frequency ?? 0;
  const confidence = data.confidence ?? 0;
  addTunerSample({ centOffset, frequency, confidence });
  const averaged = getAveragedSample();
  if (averaged) {
    data = {
      ...data,
      centOffset: averaged.centOffset,
      frequency: averaged.frequency,
      confidence: averaged.confidence,
    };
  }

  tunerState.lastDetection = data;
  updateTunerDisplay(data);
}

function updateTunerDisplay(data: TunerData): void {
  if (!tunerDisplay || !tunerPitchIndicator || !tunerCurrentNote || 
      !tunerNoteLeft || !tunerNoteRight || !tunerCentsValue || !tunerCentsIndicator) {
    appendLog(`Tuner display missing elements: display=${!!tunerDisplay}, indicator=${!!tunerPitchIndicator}, note=${!!tunerCurrentNote}, left=${!!tunerNoteLeft}, right=${!!tunerNoteRight}, cents=${!!tunerCentsValue}, centsInd=${!!tunerCentsIndicator}`);
    return;
  }

  appendLog(`Updating tuner display: detected=${data.detected}, note=${data.noteName}`);

  if (!data.detected) {
    // No pitch detected
    tunerDisplay.classList.add("no-signal");
    tunerCurrentNote.textContent = "-";
    tunerNoteLeft.textContent = "-";
    tunerNoteRight.textContent = "-";
    tunerCentsValue.textContent = "-";
    tunerCentsIndicator.textContent = "";
    tunerPitchIndicator.style.left = "50%";
    tunerPitchIndicator.classList.remove("in-tune");
    return;
  }

  tunerDisplay.classList.remove("no-signal");

  // Update note display
  const rawNoteName = (data.noteName ?? "").trim();
  const noteName = rawNoteName.length > 0 ? rawNoteName : "?";
  const baseNoteName = noteName.includes("/") ? noteName.split("/")[0] : noteName;
  tunerCurrentNote.textContent = baseNoteName || "?";

  // Update adjacent notes
  const adjacentNotes = rawNoteName.length > 0 ? getAdjacentNotes(rawNoteName) : { left: "-", right: "-" };
  tunerNoteLeft.textContent = adjacentNotes.left;
  tunerNoteRight.textContent = adjacentNotes.right;

  // Update cents/Hz display
  const centOffset = data.centOffset ?? 0;
  if (tunerState.displayMode === "cents") {
    const sign = centOffset >= 0 ? "+" : "";
    tunerCentsValue.textContent = `${sign}${centOffset.toFixed(1)}`;
  } else {
    // Show frequency
    const freq = data.frequency ?? 0;
    tunerCentsValue.textContent = `${freq.toFixed(1)} Hz`;
  }

  // Update cents indicator (flat/sharp arrow)
  if (Math.abs(centOffset) < 3) {
    tunerCentsIndicator.textContent = "✓";
    tunerCentsIndicator.style.color = "#40c080";
  } else if (centOffset < 0) {
    tunerCentsIndicator.textContent = "<";
    tunerCentsIndicator.style.color = "#e07848";
  } else {
    tunerCentsIndicator.textContent = ">";
    tunerCentsIndicator.style.color = "#e07848";
  }

  // Update pitch indicator position (map -50 to +50 cents to 0% to 100%)
  const position = 50 + (centOffset / 50) * 45; // Clamp movement to 45% each direction
  tunerPitchIndicator.style.left = `${Math.max(5, Math.min(95, position))}%`;

  // Show green when in tune (within ±3 cents)
  const isInTune = Math.abs(centOffset) < 3;
  tunerPitchIndicator.classList.toggle("in-tune", isInTune);
}

export function handleTunerStarted(referenceFrequency: number): void {
  tunerState.referenceFrequency = referenceFrequency;
  if (tunerRefInput) {
    tunerRefInput.value = referenceFrequency.toFixed(1);
  }
  appendLog(`Tuner started (ref: ${referenceFrequency} Hz)`);
}

export function handleTunerStopped(): void {
  appendLog("Tuner stopped");
}

export function handleTunerReferenceChanged(referenceFrequency: number): void {
  tunerState.referenceFrequency = referenceFrequency;
  if (tunerRefInput) {
    tunerRefInput.value = referenceFrequency.toFixed(1);
  }
  appendLog(`Tuner reference changed: ${referenceFrequency} Hz`);
}

export function handleTunerLiveModeChanged(liveMode: boolean): void {
  tunerState.isLive = liveMode;
  if (tunerLiveToggle) {
    tunerLiveToggle.checked = liveMode;
  }
  appendLog(`Tuner live mode: ${liveMode ? "ON (audio through)" : "OFF (silent)"}`);
}
