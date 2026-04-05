import { uiState } from "./state.js";
import { setMetronome } from "./bridge.js";
import { GenericKnob, enhanceRangeInput } from "./controls.js";
import type { EnvironmentState, MetronomeState } from "./types.js";

const BPM_MIN = 30;
const BPM_MAX = 300;
const BPM_STEP = 1;
const TAP_RESET_MS = 2500;
const TAP_HISTORY_MAX = 8;

const metronomeState = {
  isOpen: false,
};

let metronomeModal: HTMLElement | null = null;
let metronomeCloseBtn: HTMLElement | null = null;
let metronomeIconButton: HTMLButtonElement | null = null;
let metronomeVolumeKnob: GenericKnob | null = null;
let metronomePanKnob: GenericKnob | null = null;

function clampBpm(value: number): number {
  if (!isFinite(value)) return uiState.metronome?.bpm ?? 120;
  return Math.min(BPM_MAX, Math.max(BPM_MIN, value));
}

function getMetronomeElements(): {
  panel: HTMLElement | null;
  bpmInput: HTMLInputElement | null;
  bpmSlider: HTMLInputElement | null;
  toggleButton: HTMLButtonElement | null;
  status: HTMLElement | null;
  source: HTMLElement | null;
  bpmUpButton: HTMLButtonElement | null;
  bpmDownButton: HTMLButtonElement | null;
  footerMetronomeButton: HTMLButtonElement | null;
  footerTapButton: HTMLButtonElement | null;
  footerBpmButton: HTMLButtonElement | null;
  footerBpmPanel: HTMLElement | null;
  footerBpmInput: HTMLInputElement | null;
  footerBpmSlider: HTMLInputElement | null;
  footerBpmValue: HTMLElement | null;
  soundSelect: HTMLSelectElement | null;
  patternInput: HTMLInputElement | null;
  modal: HTMLElement | null;
  closeButton: HTMLElement | null;
  iconButton: HTMLButtonElement | null;
} {
  const panel = document.getElementById("metronome-modal");
  const footerPanel = document.getElementById("footer-bpm-panel");
  return {
    panel,
    bpmInput: panel?.querySelector<HTMLInputElement>("#metronome-bpm") ?? null,
    bpmSlider: panel?.querySelector<HTMLInputElement>("#metronome-bpm-slider") ?? null,
    toggleButton: panel?.querySelector<HTMLButtonElement>("#metronome-toggle") ?? null,
    status: panel?.querySelector<HTMLElement>("#metronome-status") ?? null,
    source: panel?.querySelector<HTMLElement>("#metronome-source") ?? null,
    bpmUpButton: panel?.querySelector<HTMLButtonElement>("#metronome-bpm-up") ?? null,
    bpmDownButton: panel?.querySelector<HTMLButtonElement>("#metronome-bpm-down") ?? null,
    footerMetronomeButton: document.getElementById("footer-metronome-btn") as HTMLButtonElement | null,
    footerTapButton: document.getElementById("footer-tap-btn") as HTMLButtonElement | null,
    footerBpmButton: document.getElementById("footer-bpm-btn") as HTMLButtonElement | null,
    footerBpmPanel: footerPanel,
    footerBpmInput: footerPanel?.querySelector<HTMLInputElement>("#footer-bpm-input") ?? null,
    footerBpmSlider: footerPanel?.querySelector<HTMLInputElement>("#footer-bpm-slider") ?? null,
    footerBpmValue: document.getElementById("footer-bpm-value"),
    soundSelect: panel?.querySelector<HTMLSelectElement>("#metronome-sound-select") ?? null,
    patternInput: panel?.querySelector<HTMLInputElement>("#metronome-accent-pattern") ?? null,
    modal: panel,
    closeButton: document.getElementById("metronome-close-btn"),
    iconButton: document.querySelector<HTMLButtonElement>(
      '.icon-bar .icon-btn[data-panel="metronome"]',
    ),
  };
}

function isStandalone(): boolean {
  return Boolean(uiState.environment?.standalone);
}

function isEditable(): boolean {
  return Boolean(uiState.metronome?.editable) && isStandalone();
}

function syncMetronomeControls(): void {
  const {
    bpmInput,
    bpmSlider,
    toggleButton,
    status,
    source,
    bpmUpButton,
    bpmDownButton,
    footerBpmButton,
    footerBpmInput,
    footerBpmSlider,
    footerBpmValue,
    footerBpmPanel,
    soundSelect,
    patternInput,
  } = getMetronomeElements();
  const state = uiState.metronome ?? {
    bpm: 120,
    enabled: false,
    editable: false,
    source: "app",
    volumeDb: -12,
    pan: 0,
    clickType: "click",
    clickTypes: [],
  };
  const bpm = clampBpm(state.bpm);
  const editable = isEditable();

  if (bpmInput) {
    bpmInput.min = String(BPM_MIN);
    bpmInput.max = String(BPM_MAX);
    bpmInput.step = String(BPM_STEP);
    bpmInput.value = bpm.toFixed(1);
    bpmInput.disabled = !editable;
  }

  if (bpmSlider) {
    bpmSlider.min = String(BPM_MIN);
    bpmSlider.max = String(BPM_MAX);
    bpmSlider.step = String(BPM_STEP);
    bpmSlider.value = bpm.toFixed(1);
    bpmSlider.disabled = !editable;
  }

  if (toggleButton) {
    const icon = toggleButton.querySelector<HTMLElement>(".metronome-play-icon");
    if (icon) {
      icon.textContent = state.enabled ? "■" : "▶";
    }
    toggleButton.disabled = !editable;
  }

  if (status) {
    status.textContent = state.enabled ? "Running" : "Stopped";
  }

  if (source) {
    source.textContent = state.source === "host" ? "Host tempo" : "Standalone";
  }

  if (footerBpmValue) {
    footerBpmValue.textContent = bpm.toFixed(1);
  }

  if (footerBpmButton) {
    footerBpmButton.disabled = !editable;
  }

  if (footerBpmInput) {
    footerBpmInput.min = String(BPM_MIN);
    footerBpmInput.max = String(BPM_MAX);
    footerBpmInput.step = String(BPM_STEP);
    footerBpmInput.value = bpm.toFixed(1);
    footerBpmInput.disabled = !editable;
  }

  if (footerBpmSlider) {
    footerBpmSlider.min = String(BPM_MIN);
    footerBpmSlider.max = String(BPM_MAX);
    footerBpmSlider.step = String(BPM_STEP);
    footerBpmSlider.value = bpm.toFixed(1);
    footerBpmSlider.disabled = !editable;
  }

  if (footerBpmPanel && !editable) {
    footerBpmPanel.classList.remove("open");
    footerBpmPanel.setAttribute("aria-hidden", "true");
  }

  if (metronomeVolumeKnob) {
    metronomeVolumeKnob.setValue(state.volumeDb ?? -12);
  }

  if (metronomePanKnob) {
    metronomePanKnob.setValue(state.pan ?? 0);
  }

  if (soundSelect) {
    const clickTypes = state.clickTypes ?? [];
    if (clickTypes.length) {
      soundSelect.innerHTML = "";
      clickTypes.forEach((type: { id: string; label?: string }) => {
        const option = document.createElement("option");
        option.value = type.id;
        option.textContent = type.label ?? type.id;
        soundSelect.appendChild(option);
      });
    }
    soundSelect.value = state.clickType ?? "click";
    soundSelect.disabled = !editable;
  }

  if (patternInput) {
    patternInput.value = state.beatPattern ?? "";
    patternInput.disabled = !editable;
  }

  if (bpmUpButton) bpmUpButton.disabled = !editable;
  if (bpmDownButton) bpmDownButton.disabled = !editable;
}

function initializeMetronomeKnobs(): void {
  const { panel } = getMetronomeElements();
  if (!panel) {
    return;
  }

  const volumeKnob = panel.querySelector<HTMLElement>(
    '.metronome-knob[data-param="metronome_volume"]',
  );
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

  const panKnob = panel.querySelector<HTMLElement>(
    '.metronome-knob[data-param="metronome_pan"]',
  );
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
        return `${direction}${Math.round(Math.abs(value) * 100)}`;
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

function updateBpm(nextBpm: number): void {
  const bpm = clampBpm(nextBpm);
  uiState.metronome = {
    ...(uiState.metronome ?? {
      bpm,
      enabled: false,
      editable: true,
      source: "app",
      volumeDb: -12,
      pan: 0,
      clickType: "click",
      clickTypes: [],
    }),
    bpm,
  };
  setMetronome({ bpm });
  syncMetronomeControls();
}

function updateEnabled(nextEnabled: boolean): void {
  uiState.metronome = {
    ...(uiState.metronome ?? {
      bpm: 120,
      enabled: nextEnabled,
      editable: true,
      source: "app",
      volumeDb: -12,
      pan: 0,
      clickType: "click",
      clickTypes: [],
    }),
    enabled: nextEnabled,
  };
  setMetronome({ enabled: nextEnabled });
  syncMetronomeControls();
}

function updateVolumeDb(nextVolumeDb: number): void {
  if (!isEditable()) return;
  uiState.metronome = {
    ...(uiState.metronome ?? {
      bpm: 120,
      enabled: false,
      editable: true,
      source: "app",
      volumeDb: nextVolumeDb,
      pan: 0,
      clickType: "click",
      clickTypes: [],
    }),
    volumeDb: nextVolumeDb,
  };
  setMetronome({ volumeDb: nextVolumeDb });
}

function updatePan(nextPan: number): void {
  if (!isEditable()) return;
  uiState.metronome = {
    ...(uiState.metronome ?? {
      bpm: 120,
      enabled: false,
      editable: true,
      source: "app",
      volumeDb: -12,
      pan: nextPan,
      clickType: "click",
      clickTypes: [],
    }),
    pan: nextPan,
  };
  setMetronome({ pan: nextPan });
}

function updateClickType(nextType: string): void {
  if (!isEditable()) return;
  if (!nextType) return;
  uiState.metronome = {
    ...(uiState.metronome ?? {
      bpm: 120,
      enabled: false,
      editable: true,
      source: "app",
      volumeDb: -12,
      pan: 0,
      clickType: nextType,
      clickTypes: [],
    }),
    clickType: nextType,
  };
  setMetronome({ clickType: nextType });
}

function updateBeatPattern(pattern: string): void {
  if (!isEditable()) return;
  uiState.metronome = {
    ...(uiState.metronome ?? {
      bpm: 120,
      enabled: false,
      editable: true,
      source: "app",
      volumeDb: -12,
      pan: 0,
      clickType: "click",
      clickTypes: [],
    }),
    beatPattern: pattern,
  };
  setMetronome({ beatPattern: pattern });
}

function openMetronome(): void {
  if (!metronomeModal) return;
  metronomeModal.style.display = "flex";
  metronomeState.isOpen = true;
  metronomeIconButton?.classList.add("active");
  syncMetronomeControls();
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
  const bpm = Math.round(60000 / avgInterval);
  updateBpm(bpm);
}

function closeMetronome(): void {
  if (!metronomeModal) return;
  metronomeModal.style.display = "none";
  metronomeState.isOpen = false;
  metronomeIconButton?.classList.remove("active");
}

export function applyEnvironmentState(environment: EnvironmentState): void {
  uiState.environment = environment;
  applyBodyStandaloneClass();
  syncMetronomeControls();
}

export function applyMetronomeState(nextState: Partial<MetronomeState>): void {
  uiState.metronome = {
    ...(uiState.metronome ?? {
      bpm: 120,
      enabled: false,
      editable: true,
      source: "app",
      volumeDb: -12,
      pan: 0,
      clickType: "click",
      clickTypes: [],
    }),
    ...nextState,
  };
  syncMetronomeControls();
}

export function initializeMetronome(): void {
  applyBodyStandaloneClass();

  const {
    panel,
    bpmInput,
    bpmSlider,
    toggleButton,
    bpmUpButton,
    bpmDownButton,
    footerMetronomeButton,
    footerTapButton,
    footerBpmButton,
    footerBpmPanel,
    footerBpmInput,
    footerBpmSlider,
    soundSelect,
    modal,
    closeButton,
    iconButton,
  } = getMetronomeElements();
  if (!panel) {
    // Still wire footer BPM if panel is not mounted yet
  }

  metronomeModal = modal;
  metronomeCloseBtn = closeButton;
  metronomeIconButton = iconButton;

  initializeMetronomeKnobs();

  const clickConfigSetting = uiState.appSettings?.["metronome.clickConfig"];
  if (Array.isArray(clickConfigSetting)) {
    const clickConfig = clickConfigSetting.flatMap((entry) => {
      if (!entry || typeof entry !== "object") return [];
      const value = entry as { id?: unknown; label?: unknown; lowPath?: unknown; highPath?: unknown };
      const id = typeof value.id === "string" ? value.id : "";
      const label = typeof value.label === "string" ? value.label : undefined;
      const lowPath = typeof value.lowPath === "string" ? value.lowPath : undefined;
      const highPath = typeof value.highPath === "string" ? value.highPath : undefined;
      if (!id || (!lowPath && !highPath)) return [];
      return [{ id, label, lowPath, highPath }];
    });

    if (clickConfig.length) {
      setMetronome({ clickConfig });
    }
  }

  if (bpmInput) {
    bpmInput.addEventListener("change", () => {
      const value = parseFloat(bpmInput.value);
      updateBpm(value);
    });
  }

  if (bpmSlider) {
    enhanceRangeInput(bpmSlider);
    bpmSlider.addEventListener("input", () => {
      const value = parseFloat(bpmSlider.value);
      updateBpm(value);
    });
  }

  if (bpmUpButton) {
    bpmUpButton.addEventListener("click", () => {
      updateBpm((uiState.metronome?.bpm ?? 120) + BPM_STEP);
    });
  }

  if (bpmDownButton) {
    bpmDownButton.addEventListener("click", () => {
      updateBpm((uiState.metronome?.bpm ?? 120) - BPM_STEP);
    });
  }

  if (soundSelect) {
    soundSelect.addEventListener("change", () => {
      updateClickType(soundSelect.value);
    });
  }

  const { patternInput } = getMetronomeElements();
  if (patternInput) {
    patternInput.addEventListener("change", () => {
      const normalized = patternInput.value.toUpperCase().replace(/[^HLS\-.]/g, "");
      patternInput.value = normalized;
      updateBeatPattern(normalized);
    });
  }

  if (toggleButton) {
    toggleButton.addEventListener("click", () => {
      const enabled = !Boolean(uiState.metronome?.enabled);
      updateEnabled(enabled);
    });
  }

  if (metronomeIconButton) {
    metronomeIconButton.addEventListener("click", () => {
      if (metronomeState.isOpen) {
        closeMetronome();
      } else {
        openMetronome();
      }
    });
  }

  if (footerMetronomeButton) {
    footerMetronomeButton.addEventListener("click", () => {
      if (metronomeState.isOpen) {
        closeMetronome();
      } else {
        openMetronome();
      }
    });
  }

  if (footerTapButton) {
    footerTapButton.addEventListener("click", () => {
      handleTapTempo();
    });
  }

  if (metronomeCloseBtn) {
    metronomeCloseBtn.addEventListener("click", () => closeMetronome());
  }

  if (metronomeModal) {
    metronomeModal.addEventListener("mousedown", (event) => {
      if (event.target === metronomeModal) {
        closeMetronome();
      }
    });
  }

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

  if (footerBpmButton && footerBpmPanel) {
    const togglePanel = () => {
      if (!isEditable()) return;
      const isOpen = footerBpmPanel.classList.toggle("open");
      footerBpmPanel.setAttribute("aria-hidden", isOpen ? "false" : "true");
    };

    footerBpmButton.addEventListener("click", (event) => {
      event.stopPropagation();
      togglePanel();
    });

    document.addEventListener("click", (event) => {
      if (!footerBpmPanel.classList.contains("open")) return;
      const target = event.target as HTMLElement | null;
      if (target && footerBpmPanel.contains(target)) return;
      if (target && footerBpmButton.contains(target)) return;
      footerBpmPanel.classList.remove("open");
      footerBpmPanel.setAttribute("aria-hidden", "true");
    });
  }

  if (footerBpmInput) {
    footerBpmInput.addEventListener("change", () => {
      const value = parseFloat(footerBpmInput.value);
      updateBpm(value);
    });
  }

  if (footerBpmSlider) {
    enhanceRangeInput(footerBpmSlider);
    footerBpmSlider.addEventListener("input", () => {
      const value = parseFloat(footerBpmSlider.value);
      updateBpm(value);
    });
  }

  syncMetronomeControls();
}
