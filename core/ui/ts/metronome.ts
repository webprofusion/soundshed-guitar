/**
 * The metronome panel and the footer's tempo controls.
 *
 * State lives in `uiState.metronome` and is owned by the engine: every edit
 * here goes out as a `setMetronome` message and comes back in the next state
 * broadcast, so the UI never has to guess what normalisation the engine
 * applied (a pattern is one character per beat, a grouping has to add up).
 * The meter helpers in metronomeMeters.ts mirror that normalisation so the
 * controls can be drawn before the round trip lands.
 */

import { uiState } from "./state.js";
import { setMetronome } from "./bridge.js";
import { GenericKnob, enhanceRangeInput } from "./controls.js";
import type { EnvironmentState, MetronomeState, MetronomeSubdivisionOption } from "./types.js";
import { getPlaySvg, getStopSvg } from "./iconAssets.js";
import { MetronomeDropdown, type DropdownOption } from "./metronomeDropdown.js";
import { MetronomeBeatGrid } from "./metronomeBeatGrid.js";
import {
  DEFAULT_SUBDIVISIONS,
  TIME_SIGNATURES,
  type BeatLevel,
  clampBeatsPerBar,
  defaultBeatPattern,
  describeTimeSignature,
  normaliseBeatPattern,
  patternFromLevels,
} from "./metronomeMeters.js";

const BPM_MIN = 30;
const BPM_MAX = 300;
const BPM_STEP = 1;
const BPM_FINE_STEP = 0.1;
const TAP_RESET_MS = 2500;
const TAP_HISTORY_MAX = 8;
const DEFAULT_SUBDIVISION_ID = "1/4";

const DEFAULT_METRONOME: MetronomeState = {
  bpm: 120,
  enabled: false,
  editable: true,
  source: "app",
  volumeDb: -12,
  pan: 0,
  clickType: "kit1",
  clickTypes: [],
  beatPattern: "HLLL",
  timeSigNum: 4,
  timeSigDen: 4,
  grouping: "",
  subdivision: DEFAULT_SUBDIVISION_ID,
  subdivisions: [],
};

const metronomeState = {
  isOpen: false,
};

let metronomeModal: HTMLElement | null = null;
let metronomeCloseBtn: HTMLElement | null = null;
let metronomeIconButton: HTMLButtonElement | null = null;
let metronomeVolumeKnob: GenericKnob | null = null;
let metronomePanKnob: GenericKnob | null = null;
let timeSignatureDropdown: MetronomeDropdown | null = null;
let soundDropdown: MetronomeDropdown | null = null;
let rhythmDropdown: MetronomeDropdown | null = null;
let beatGrid: MetronomeBeatGrid | null = null;

/// Rebuilding the beat strip throws away its pulse, so it is only redrawn when
/// something about the bar actually changed.
let lastBeatGridKey = "";

function clampBpm(value: number): number {
  if (!isFinite(value)) return uiState.metronome?.bpm ?? DEFAULT_METRONOME.bpm;
  return Math.min(BPM_MAX, Math.max(BPM_MIN, Math.round(value * 10) / 10));
}

function state(): MetronomeState {
  return uiState.metronome ?? DEFAULT_METRONOME;
}

function patchState(next: Partial<MetronomeState>): void {
  uiState.metronome = { ...state(), ...next };
}

function subdivisionOptions(): MetronomeSubdivisionOption[] {
  const fromEngine = state().subdivisions;
  return fromEngine && fromEngine.length ? fromEngine : DEFAULT_SUBDIVISIONS;
}

function ticksPerBeat(): number {
  const id = state().subdivision ?? DEFAULT_SUBDIVISION_ID;
  return subdivisionOptions().find((option) => option.id === id)?.ticksPerBeat ?? 1;
}

function getMetronomeElements(): {
  panel: HTMLElement | null;
  bpmInput: HTMLInputElement | null;
  toggleButton: HTMLButtonElement | null;
  tapButton: HTMLButtonElement | null;
  status: HTMLElement | null;
  source: HTMLElement | null;
  bpmUpButton: HTMLButtonElement | null;
  bpmDownButton: HTMLButtonElement | null;
  footerMetronomeButton: HTMLButtonElement | null;
  footerMetronomeToggleButton: HTMLButtonElement | null;
  footerTapButton: HTMLButtonElement | null;
  footerBpmButton: HTMLButtonElement | null;
  footerBpmPanel: HTMLElement | null;
  footerBpmInput: HTMLInputElement | null;
  footerBpmSlider: HTMLInputElement | null;
  footerBpmValue: HTMLElement | null;
  beats: HTMLElement | null;
  modal: HTMLElement | null;
  closeButton: HTMLElement | null;
  iconButton: HTMLButtonElement | null;
} {
  const panel = document.getElementById("metronome-modal");
  const footerPanel = document.getElementById("footer-bpm-panel");
  return {
    panel,
    bpmInput: panel?.querySelector<HTMLInputElement>("#metronome-bpm") ?? null,
    toggleButton: panel?.querySelector<HTMLButtonElement>("#metronome-toggle") ?? null,
    tapButton: panel?.querySelector<HTMLButtonElement>("#metronome-tap") ?? null,
    status: panel?.querySelector<HTMLElement>("#metronome-status") ?? null,
    source: panel?.querySelector<HTMLElement>("#metronome-source") ?? null,
    bpmUpButton: panel?.querySelector<HTMLButtonElement>("#metronome-bpm-up") ?? null,
    bpmDownButton: panel?.querySelector<HTMLButtonElement>("#metronome-bpm-down") ?? null,
    footerMetronomeButton: document.getElementById("footer-metronome-btn") as HTMLButtonElement | null,
    footerMetronomeToggleButton: document.getElementById("footer-metronome-toggle") as HTMLButtonElement | null,
    footerTapButton: document.getElementById("footer-tap-btn") as HTMLButtonElement | null,
    footerBpmButton: document.getElementById("footer-bpm-btn") as HTMLButtonElement | null,
    footerBpmPanel: footerPanel,
    footerBpmInput: footerPanel?.querySelector<HTMLInputElement>("#footer-bpm-input") ?? null,
    footerBpmSlider: footerPanel?.querySelector<HTMLInputElement>("#footer-bpm-slider") ?? null,
    footerBpmValue: document.getElementById("footer-bpm-value"),
    beats: panel?.querySelector<HTMLElement>("#metronome-beats") ?? null,
    modal: panel,
    closeButton: document.getElementById("metronome-close-btn"),
    iconButton: document.querySelector<HTMLButtonElement>('.icon-bar .icon-btn[data-panel="metronome"]'),
  };
}

function isStandalone(): boolean {
  return Boolean(uiState.environment?.standalone);
}

function isEditable(): boolean {
  return Boolean(uiState.metronome?.editable) && isStandalone();
}

// ── Control sync ──────────────────────────────────────────────────────

function syncBeatGrid(editable: boolean): void {
  if (!beatGrid) return;

  const current = state();
  const num = clampBeatsPerBar(current.timeSigNum ?? 4);
  const den = current.timeSigDen ?? 4;
  const grouping = current.grouping ?? "";
  const pattern = normaliseBeatPattern(current.beatPattern ?? "", num, den, grouping);
  const ticks = ticksPerBeat();
  const key = `${num}/${den}:${grouping}:${pattern}:${ticks}:${editable}`;

  if (key === lastBeatGridKey) return;
  lastBeatGridKey = key;

  beatGrid.render({ num, den, grouping, pattern, ticksPerBeat: ticks, editable });
}

function syncDropdowns(editable: boolean): void {
  const current = state();

  if (timeSignatureDropdown) {
    const num = current.timeSigNum ?? 4;
    const den = current.timeSigDen ?? 4;
    const grouping = current.grouping ?? "";
    const options: DropdownOption[] = TIME_SIGNATURES.map((option) => ({
      value: option.id,
      label: option.label,
      column: option.family,
    }));

    const activeId = grouping ? `${num}/${den}:${grouping}` : `${num}/${den}`;

    // A meter the catalogue does not list (an imported setting, say) still has
    // to be selectable, or the picker would silently show the wrong thing.
    if (!options.some((option) => option.value === activeId)) {
      options.push({ value: activeId, label: describeTimeSignature(num, den, grouping), column: "compound" });
    }

    timeSignatureDropdown.setOptions(options);
    timeSignatureDropdown.setValue(activeId);
    timeSignatureDropdown.setDisabled(!editable);
  }

  if (soundDropdown) {
    const clickTypes = current.clickTypes ?? [];
    soundDropdown.setOptions(clickTypes.map((type) => ({ value: type.id, label: type.label ?? type.id })));
    soundDropdown.setValue(current.clickType ?? DEFAULT_METRONOME.clickType);
    soundDropdown.setDisabled(!editable || !clickTypes.length);
  }

  if (rhythmDropdown) {
    rhythmDropdown.setOptions(subdivisionOptions().map((option) => ({ value: option.id, label: option.id })));
    rhythmDropdown.setValue(current.subdivision ?? DEFAULT_SUBDIVISION_ID);
    rhythmDropdown.setDisabled(!editable);
  }
}

function syncMetronomeControls(): void {
  const {
    bpmInput,
    toggleButton,
    tapButton,
    status,
    source,
    bpmUpButton,
    bpmDownButton,
    footerMetronomeToggleButton,
    footerBpmButton,
    footerBpmInput,
    footerBpmSlider,
    footerBpmValue,
    footerBpmPanel,
  } = getMetronomeElements();
  const current = state();
  const bpm = clampBpm(current.bpm);
  const editable = isEditable();

  for (const input of [bpmInput, footerBpmInput, footerBpmSlider]) {
    if (!input) continue;
    input.min = String(BPM_MIN);
    input.max = String(BPM_MAX);
    input.step = String(input === footerBpmSlider ? BPM_STEP : BPM_FINE_STEP);
    input.value = bpm.toFixed(1);
    input.disabled = !editable;
  }

  if (toggleButton) {
    const icon = toggleButton.querySelector<HTMLElement>(".metronome-play-icon");
    if (icon) {
      icon.innerHTML = current.enabled ? getStopSvg() : getPlaySvg();
    }
    toggleButton.disabled = !editable;
    toggleButton.classList.toggle("is-running", current.enabled);
    toggleButton.setAttribute("aria-label", current.enabled ? "Stop metronome" : "Start metronome");
  }

  if (footerMetronomeToggleButton) {
    const icon = footerMetronomeToggleButton.querySelector<HTMLElement>(".footer-metronome-play-icon");
    if (icon) {
      icon.innerHTML = current.enabled ? getStopSvg() : getPlaySvg();
    }
    footerMetronomeToggleButton.disabled = !editable;
    footerMetronomeToggleButton.classList.toggle("is-active", current.enabled);
    footerMetronomeToggleButton.setAttribute("aria-pressed", current.enabled ? "true" : "false");
    footerMetronomeToggleButton.setAttribute("aria-label", current.enabled ? "Stop metronome" : "Start metronome");
    footerMetronomeToggleButton.title = current.enabled ? "Stop metronome" : "Start metronome";
  }

  if (status) {
    status.textContent = current.enabled ? "Running" : "Stopped";
  }

  if (source) {
    source.textContent = current.source === "host" ? "Host tempo" : "Standalone";
  }

  if (footerBpmValue) {
    footerBpmValue.textContent = bpm.toFixed(1);
  }

  if (footerBpmButton) {
    footerBpmButton.disabled = !editable;
  }

  if (footerBpmPanel && !editable) {
    footerBpmPanel.classList.remove("open");
    footerBpmPanel.setAttribute("aria-hidden", "true");
  }

  metronomeVolumeKnob?.setValue(current.volumeDb ?? DEFAULT_METRONOME.volumeDb);
  metronomePanKnob?.setValue(current.pan ?? DEFAULT_METRONOME.pan);

  if (tapButton) tapButton.disabled = !editable;
  if (bpmUpButton) bpmUpButton.disabled = !editable;
  if (bpmDownButton) bpmDownButton.disabled = !editable;

  if (!current.enabled) beatGrid?.clearPulse();

  syncDropdowns(editable);
  syncBeatGrid(editable);
}

function initializeMetronomeKnobs(): void {
  const { panel } = getMetronomeElements();
  if (!panel) {
    return;
  }

  const volumeKnob = panel.querySelector<HTMLElement>('.knob[data-param="metronome_volume"]');
  if (volumeKnob) {
    metronomeVolumeKnob = new GenericKnob({
      knobElement: volumeKnob,
      paramId: "metronome_volume",
      minValue: -60,
      maxValue: 12,
      defaultValue: -12,
      displayFormat: (value) => `${value.toFixed(1)} dB`,
      valueDisplayId: "metronome-volume-value",
      sensitivity: 0.5,
      sendParameter: false,
      onValueChange: (value) => updateVolumeDb(value),
    });
  }

  const panKnob = panel.querySelector<HTMLElement>('.knob[data-param="metronome_pan"]');
  if (panKnob) {
    metronomePanKnob = new GenericKnob({
      knobElement: panKnob,
      paramId: "metronome_pan",
      minValue: -1,
      maxValue: 1,
      defaultValue: 0,
      displayFormat: (value) => {
        if (Math.abs(value) < 0.01) return "C";
        const direction = value < 0 ? "L" : "R";
        return `${Math.round(Math.abs(value) * 100)} ${direction}`;
      },
      valueDisplayId: "metronome-pan-value",
      sensitivity: 0.02,
      sendParameter: false,
      onValueChange: (value) => updatePan(value),
    });
  }
}

function applyBodyStandaloneClass(): void {
  document.body.classList.toggle("is-standalone", isStandalone());
}

// ── Edits ─────────────────────────────────────────────────────────────

function updateBpm(nextBpm: number): void {
  const bpm = clampBpm(nextBpm);
  patchState({ bpm });
  setMetronome({ bpm });
  syncMetronomeControls();
}

function updateEnabled(nextEnabled: boolean): void {
  patchState({ enabled: nextEnabled });
  setMetronome({ enabled: nextEnabled });
  syncMetronomeControls();
}

function updateVolumeDb(nextVolumeDb: number): void {
  if (!isEditable()) return;
  patchState({ volumeDb: nextVolumeDb });
  setMetronome({ volumeDb: nextVolumeDb });
}

function updatePan(nextPan: number): void {
  if (!isEditable()) return;
  patchState({ pan: nextPan });
  setMetronome({ pan: nextPan });
}

function updateClickType(nextType: string): void {
  if (!isEditable() || !nextType) return;
  patchState({ clickType: nextType });
  setMetronome({ clickType: nextType });
  syncMetronomeControls();
}

function updateBeatPattern(pattern: string): void {
  if (!isEditable()) return;
  patchState({ beatPattern: pattern });
  setMetronome({ beatPattern: pattern });
}

function updateSubdivision(subdivision: string): void {
  if (!isEditable()) return;
  patchState({ subdivision });
  setMetronome({ subdivision });
  syncMetronomeControls();
}

/**
 * A meter change re-seeds the accent pattern, because a pattern is one
 * character per beat. The engine does the same thing; sending both together
 * keeps the dots from flickering through a stale bar on the way back.
 */
function updateTimeSignature(optionId: string): void {
  if (!isEditable()) return;

  const option = TIME_SIGNATURES.find((entry) => entry.id === optionId);
  if (!option) return;

  const beatPattern = defaultBeatPattern(option.num, option.den, option.grouping);
  patchState({
    timeSigNum: option.num,
    timeSigDen: option.den,
    grouping: option.grouping,
    beatPattern,
  });
  setMetronome({
    timeSigNum: option.num,
    timeSigDen: option.den,
    grouping: option.grouping,
    beatPattern,
  });
  syncMetronomeControls();
}

function openMetronome(): void {
  if (!metronomeModal) return;
  metronomeModal.style.display = "flex";
  metronomeState.isOpen = true;
  metronomeIconButton?.classList.add("active");
  syncMetronomeControls();
}

function closeMetronome(): void {
  if (!metronomeModal) return;
  metronomeModal.style.display = "none";
  metronomeState.isOpen = false;
  metronomeIconButton?.classList.remove("active");
  beatGrid?.clearPulse();
}

const tapTimes: number[] = [];

function handleTapTempo(): void {
  if (!isEditable()) return;

  const now = performance.now();
  const lastTap = tapTimes[tapTimes.length - 1];
  if (lastTap && now - lastTap > TAP_RESET_MS) {
    tapTimes.length = 0;
  }
  tapTimes.push(now);
  if (tapTimes.length > TAP_HISTORY_MAX) {
    tapTimes.shift();
  }

  if (tapTimes.length < 3) return;

  const intervals: number[] = [];
  for (let i = 1; i < tapTimes.length; i += 1) {
    intervals.push(tapTimes[i] - tapTimes[i - 1]);
  }
  if (!intervals.length) return;

  const avgInterval = intervals.reduce((sum, value) => sum + value, 0) / intervals.length;
  updateBpm(60000 / avgInterval);
}

// ── Engine messages ───────────────────────────────────────────────────

export function applyEnvironmentState(environment: EnvironmentState): void {
  uiState.environment = environment;
  applyBodyStandaloneClass();
  syncMetronomeControls();
}

/** Accepts the raw `metronome` payload; every field is optional. */
export function applyMetronomeState(payload: Record<string, unknown> | Partial<MetronomeState>): void {
  const raw = payload as Record<string, unknown>;
  const prev = state();

  const readNumber = (key: string, fallback: number): number =>
    typeof raw[key] === "number" ? (raw[key] as number) : fallback;
  const readString = (key: string, fallback: string): string =>
    typeof raw[key] === "string" ? (raw[key] as string) : fallback;

  const clickTypes = Array.isArray(raw.clickTypes)
    ? (raw.clickTypes as Array<{ id?: unknown; label?: unknown }>)
        .filter((entry) => entry && typeof entry.id === "string")
        .map((entry) => ({
          id: entry.id as string,
          label: typeof entry.label === "string" ? entry.label : (entry.id as string),
        }))
    : prev.clickTypes;

  const subdivisions = Array.isArray(raw.subdivisions)
    ? (raw.subdivisions as Array<{ id?: unknown; ticksPerBeat?: unknown }>)
        .filter((entry) => entry && typeof entry.id === "string")
        .map((entry) => ({
          id: entry.id as string,
          ticksPerBeat: typeof entry.ticksPerBeat === "number" ? entry.ticksPerBeat : 1,
        }))
    : prev.subdivisions;

  uiState.metronome = {
    bpm: readNumber("bpm", prev.bpm),
    enabled: "enabled" in raw ? Boolean(raw.enabled) : prev.enabled,
    editable: "editable" in raw ? Boolean(raw.editable) : prev.editable,
    source: raw.source === "host" ? "host" : raw.source === "app" ? "app" : prev.source,
    volumeDb: readNumber("volumeDb", prev.volumeDb),
    pan: readNumber("pan", prev.pan),
    clickType: readString("clickType", prev.clickType),
    clickTypes,
    beatPattern: readString("beatPattern", prev.beatPattern ?? ""),
    timeSigNum: readNumber("timeSigNum", prev.timeSigNum ?? 4),
    timeSigDen: readNumber("timeSigDen", prev.timeSigDen ?? 4),
    grouping: readString("grouping", prev.grouping ?? ""),
    subdivision: readString("subdivision", prev.subdivision ?? DEFAULT_SUBDIVISION_ID),
    subdivisions,
  };

  syncMetronomeControls();
}

/** Lights the beat the engine just played. */
export function applyMetronomeBeat(beatIndex: number): void {
  if (!metronomeState.isOpen) return;
  beatGrid?.pulse(beatIndex);
}

// ── Wiring ────────────────────────────────────────────────────────────

function initializeDropdowns(panel: HTMLElement): void {
  const build = (name: string, onSelect: (value: string) => void): MetronomeDropdown | null => {
    const trigger = panel.querySelector<HTMLButtonElement>(`#metronome-${name}-trigger`);
    const dropdownPanel = panel.querySelector<HTMLElement>(`#metronome-${name}-panel`);
    if (!trigger || !dropdownPanel) return null;
    return new MetronomeDropdown({ trigger, panel: dropdownPanel, onSelect });
  };

  timeSignatureDropdown = build("timesig", (value) => updateTimeSignature(value));
  soundDropdown = build("sound", (value) => updateClickType(value));
  rhythmDropdown = build("rhythm", (value) => updateSubdivision(value));
}

function initializeBeatGrid(): void {
  const { beats } = getMetronomeElements();
  if (!beats) return;

  beatGrid = new MetronomeBeatGrid(beats, (levels: BeatLevel[]) => {
    const pattern = patternFromLevels(levels);
    patchState({ beatPattern: pattern });
    lastBeatGridKey = "";
    updateBeatPattern(pattern);
  });
}

function initializeBpmControls(): void {
  const { bpmInput, bpmUpButton, bpmDownButton, tapButton } = getMetronomeElements();

  const nudge = (direction: number, fine: boolean) => {
    updateBpm((uiState.metronome?.bpm ?? DEFAULT_METRONOME.bpm) + direction * (fine ? BPM_FINE_STEP : BPM_STEP));
  };

  if (bpmInput) {
    bpmInput.addEventListener("change", () => updateBpm(parseFloat(bpmInput.value)));

    // Wheel over the readout replaces the coarse slider the panel used to
    // carry; Shift drops it to tenths, which is where tapped tempos land.
    bpmInput.addEventListener("wheel", (event) => {
      if (!isEditable()) return;
      event.preventDefault();
      nudge(event.deltaY < 0 ? 1 : -1, event.shiftKey);
    });

    bpmInput.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowUp" && event.key !== "ArrowDown") return;
      if (!event.shiftKey) return;
      event.preventDefault();
      nudge(event.key === "ArrowUp" ? 1 : -1, true);
    });
  }

  bpmUpButton?.addEventListener("click", (event) => nudge(1, event.shiftKey));
  bpmDownButton?.addEventListener("click", (event) => nudge(-1, event.shiftKey));
  tapButton?.addEventListener("click", () => handleTapTempo());
}

function initializeFooterControls(): void {
  const {
    footerMetronomeButton,
    footerMetronomeToggleButton,
    footerTapButton,
    footerBpmButton,
    footerBpmPanel,
    footerBpmInput,
    footerBpmSlider,
  } = getMetronomeElements();

  footerMetronomeButton?.addEventListener("click", () => {
    if (metronomeState.isOpen) {
      closeMetronome();
    } else {
      openMetronome();
    }
  });

  footerMetronomeToggleButton?.addEventListener("click", () => {
    if (!isEditable()) return;
    updateEnabled(!uiState.metronome?.enabled);
  });

  footerTapButton?.addEventListener("click", () => handleTapTempo());

  if (footerBpmButton && footerBpmPanel) {
    footerBpmButton.addEventListener("click", (event) => {
      event.stopPropagation();
      if (!isEditable()) return;
      const isOpen = footerBpmPanel.classList.toggle("open");
      footerBpmPanel.setAttribute("aria-hidden", isOpen ? "false" : "true");
    });

    document.addEventListener("click", (event) => {
      if (!footerBpmPanel.classList.contains("open")) return;
      const target = event.target as HTMLElement | null;
      if (target && (footerBpmPanel.contains(target) || footerBpmButton.contains(target))) return;
      footerBpmPanel.classList.remove("open");
      footerBpmPanel.setAttribute("aria-hidden", "true");
    });
  }

  footerBpmInput?.addEventListener("change", () => updateBpm(parseFloat(footerBpmInput.value)));

  if (footerBpmSlider) {
    enhanceRangeInput(footerBpmSlider);
    footerBpmSlider.addEventListener("input", () => updateBpm(parseFloat(footerBpmSlider.value)));
  }
}

export function initializeMetronome(): void {
  applyBodyStandaloneClass();

  const { panel, toggleButton, modal, closeButton, iconButton } = getMetronomeElements();

  metronomeModal = modal;
  metronomeCloseBtn = closeButton;
  metronomeIconButton = iconButton;

  initializeMetronomeKnobs();
  initializeBeatGrid();
  initializeBpmControls();
  initializeFooterControls();

  if (panel) {
    initializeDropdowns(panel);
  }

  toggleButton?.addEventListener("click", () => updateEnabled(!uiState.metronome?.enabled));
  metronomeCloseBtn?.addEventListener("click", () => closeMetronome());

  metronomeIconButton?.addEventListener("click", () => {
    if (metronomeState.isOpen) {
      closeMetronome();
    } else {
      openMetronome();
    }
  });

  metronomeModal?.addEventListener("mousedown", (event) => {
    if (event.target === metronomeModal) {
      closeMetronome();
    }
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && metronomeState.isOpen) {
      closeMetronome();
    }
    if (event.code === "Space" && !event.repeat) {
      const target = event.target as HTMLElement | null;
      const tagName = target?.tagName?.toLowerCase();
      const isTextInput = tagName === "input" || tagName === "textarea" || target?.isContentEditable;
      if (!isTextInput) {
        event.preventDefault();
        handleTapTempo();
      }
    }
  });

  syncMetronomeControls();
}
