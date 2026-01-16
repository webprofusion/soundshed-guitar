import { uiState } from "./state.js";
import { setMetronome } from "./bridge.js";
import { GenericKnob } from "./controls.js";
import type { EnvironmentState, MetronomeState } from "./types.js";

const BPM_MIN = 30;
const BPM_MAX = 300;
const BPM_STEP = 1;

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
  footerBpmButton: HTMLButtonElement | null;
  footerBpmPanel: HTMLElement | null;
  footerBpmInput: HTMLInputElement | null;
  footerBpmSlider: HTMLInputElement | null;
  footerBpmValue: HTMLElement | null;
} {
  const panel = document.getElementById("panel-metronome");
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
    footerBpmButton: document.getElementById("footer-bpm-btn") as HTMLButtonElement | null,
    footerBpmPanel: footerPanel,
    footerBpmInput: footerPanel?.querySelector<HTMLInputElement>("#footer-bpm-input") ?? null,
    footerBpmSlider: footerPanel?.querySelector<HTMLInputElement>("#footer-bpm-slider") ?? null,
    footerBpmValue: document.getElementById("footer-bpm-value"),
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
  } = getMetronomeElements();
  const state = uiState.metronome ?? { bpm: 120, enabled: false, editable: false, source: "app" };
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
    new GenericKnob({
      knobElement: volumeKnob,
      paramId: "metronome_volume",
      minValue: -60,
      maxValue: 6,
      defaultValue: 0,
      displayFormat: (value) => `${value.toFixed(1)} dB`,
      valueDisplayId: "metronome-volume-value",
      sensitivity: 0.5,
      sendParameter: false,
    });
  }

  const panKnob = panel.querySelector<HTMLElement>(
    '.metronome-knob[data-param="metronome_pan"]',
  );
  if (panKnob) {
    new GenericKnob({
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
    });
  }
}

function applyBodyStandaloneClass(): void {
  document.body.classList.toggle("is-standalone", isStandalone());
}

function updateBpm(nextBpm: number): void {
  const bpm = clampBpm(nextBpm);
  uiState.metronome = {
    ...(uiState.metronome ?? { bpm, enabled: false, editable: true, source: "app" }),
    bpm,
  };
  setMetronome({ bpm });
  syncMetronomeControls();
}

function updateEnabled(nextEnabled: boolean): void {
  uiState.metronome = {
    ...(uiState.metronome ?? { bpm: 120, enabled: nextEnabled, editable: true, source: "app" }),
    enabled: nextEnabled,
  };
  setMetronome({ enabled: nextEnabled });
  syncMetronomeControls();
}

export function applyEnvironmentState(environment: EnvironmentState): void {
  uiState.environment = environment;
  applyBodyStandaloneClass();
  syncMetronomeControls();
}

export function applyMetronomeState(nextState: Partial<MetronomeState>): void {
  uiState.metronome = {
    ...(uiState.metronome ?? { bpm: 120, enabled: false, editable: true, source: "app" }),
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
    footerBpmButton,
    footerBpmPanel,
    footerBpmInput,
    footerBpmSlider,
  } = getMetronomeElements();
  if (!panel) {
    // Still wire footer BPM if panel is not mounted yet
  }

  initializeMetronomeKnobs();

  if (bpmInput) {
    bpmInput.addEventListener("change", () => {
      const value = parseFloat(bpmInput.value);
      updateBpm(value);
    });
  }

  if (bpmSlider) {
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

  if (toggleButton) {
    toggleButton.addEventListener("click", () => {
      const enabled = !Boolean(uiState.metronome?.enabled);
      updateEnabled(enabled);
    });
  }

  if (footerMetronomeButton) {
    footerMetronomeButton.addEventListener("click", () => {
      const metronomeTab = document.querySelector<HTMLElement>(
        '.icon-bar .icon-btn[data-panel="metronome"]',
      );
      metronomeTab?.click();
    });
  }

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
    footerBpmSlider.addEventListener("input", () => {
      const value = parseFloat(footerBpmSlider.value);
      updateBpm(value);
    });
  }

  syncMetronomeControls();
}
