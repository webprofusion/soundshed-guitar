import { renderDemoAudioControls, bindDemoAudioControls } from "./demoAudio.js";
import { uiState } from "./state.js";
import { addActivePreset, setPresetMix, setPresetPan, setPresetMute, setPresetSolo, setMasterGain, setLimiterEnabled } from "./bridge.js";
import { escapeHtml } from "./utils.js";
import type { Preset } from "./types.js";

const presetListElement = document.getElementById("preset-list");
const presetDetailsElement = document.getElementById("preset-details");

interface RenderHooks {
  onPresetSelected: (presetId: string) => Promise<void> | void;
  onApplyPreset: (presetId: string) => Promise<void> | void;
  onRequestSignalTest: () => void;
  onBindLoadButtons: () => void;
}

function formatParameterValue(parameter: { value: unknown }): string {
  if (typeof parameter.value === "number") {
    return parameter.value.toFixed(2);
  }
  if (typeof parameter.value === "boolean") {
    return parameter.value ? "On" : "Off";
  }
  return `${parameter.value ?? ""}`;
}

function renderParameterSection(): string {
  const parameterItems = (uiState.parameters.values ?? [])
    .map((parameter) => {
      const label = (parameter as { label?: string; id?: string }).label ?? (parameter as { id?: string }).id ?? "";
      return `
        <div class="param-card">
          <span class="param-label">${label}</span>
          <span class="param-value">${formatParameterValue(parameter)}</span>
        </div>
      `;
    })
    .join("");

  const gateStatus = typeof uiState.parameters.gateEnabled === "boolean"
    ? `
        <div class="param-card ${uiState.parameters.gateEnabled ? "active" : ""}">
          <span class="param-label">Noise Gate</span>
          <span class="param-value">${uiState.parameters.gateEnabled ? "On" : "Off"}${
            typeof uiState.parameters.gateThreshold === "number"
              ? ` (${uiState.parameters.gateThreshold.toFixed(1)} dB)`
              : ""
          }</span>
        </div>
      `
    : "";

  const signalTestSection = uiState.signalTest
    ? `
        <div class="test-results ${uiState.signalTest.passed ? "passed" : "failed"}">
          <div class="test-header">
            <span class="test-icon">${uiState.signalTest.passed ? "✓" : "✗"}</span>
            <span class="test-title">Signal Path Test</span>
            <span class="test-status">${uiState.signalTest.passed ? "Passed" : "Failed"}</span>
          </div>
          <div class="test-details">
            <div class="test-stat">
              <span class="stat-label">Frequency</span>
              <span class="stat-value">${uiState.signalTest.frequency.toFixed(1)} Hz</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Audio Duration</span>
              <span class="stat-value">${uiState.signalTest.duration.toFixed(3)} s</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Processing Time</span>
              <span class="stat-value">${uiState.signalTest.elapsed.toFixed(3)} s</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Realtime Ratio</span>
              <span class="stat-value ${uiState.signalTest.elapsed > 0 && uiState.signalTest.duration / uiState.signalTest.elapsed < 1 ? "warning" : ""}">${uiState.signalTest.elapsed > 0 ? (uiState.signalTest.duration / uiState.signalTest.elapsed).toFixed(2) : "N/A"}x</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Input RMS</span>
              <span class="stat-value">${uiState.signalTest.inputRMS.toFixed(4)}</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Output L/R</span>
              <span class="stat-value">${uiState.signalTest.outputLeft.toFixed(4)} / ${uiState.signalTest.outputRight.toFixed(4)}</span>
            </div>
          </div>
          ${uiState.signalTest.message ? `<p class="test-message">${uiState.signalTest.message}</p>` : ""}
        </div>
      `
    : "";

  return `
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">⚙️</span>
        Current Parameters
      </h3>
      <div class="params-grid">
        ${parameterItems}
        ${gateStatus}
      </div>
    </div>
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">🔬</span>
        Diagnostics
      </h3>
      <button id="run-signal-test" class="test-btn">Run Signal Path Test</button>
      ${signalTestSection}
    </div>
  `;
}

function buildMixerMarkup(): string {
  const mixer = uiState.mixer;
  if (!mixer || !mixer.activePresetIds.length) {
    return `
      <div class="mixer-panel-empty">
        <p>No active presets in the mixer.</p>
        <p>Add a preset from the Preset details view.</p>
      </div>
    `;
  }

  const rows = mixer.activePresetIds.map((id) => {
    const presetName = uiState.presetCache.get(id)?.name ?? id;
    const ps = mixer.presets[id] ?? { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
    return `
      <div class="mixer-row" data-preset-id="${escapeHtml(id)}">
        <div class="mixer-row-header">
          <span class="mixer-row-name">${escapeHtml(presetName)}</span>
          <label class="toggle mini-toggle"><input type="checkbox" class="mixer-solo" ${ps.solo ? "checked" : ""}/> Solo</label>
          <label class="toggle mini-toggle"><input type="checkbox" class="mixer-mute" ${ps.mute ? "checked" : ""}/> Mute</label>
        </div>
        <div class="mixer-controls">
          <label class="mixer-control">
            <span>Mix</span>
            <input type="range" class="mixer-mix" min="0" max="1" step="0.01" value="${ps.mix}"/>
          </label>
          <label class="mixer-control">
            <span>Pan</span>
            <input type="range" class="mixer-pan" min="-1" max="1" step="0.01" value="${ps.pan}"/>
          </label>
        </div>
      </div>
    `;
  }).join("");

  return `
    <div class="mixer-panel">
      <div class="mixer-master">
        <label class="mixer-control">
          <span>Master Gain</span>
          <input type="range" id="mixer-master-gain" min="0" max="2" step="0.01" value="${mixer.masterGain}"/>
        </label>
        <label class="toggle mini-toggle">
          <input type="checkbox" id="mixer-limiter" ${mixer.limiterEnabled ? "checked" : ""}/> Limiter
        </label>
      </div>
      <div class="mixer-rows">
        ${rows}
      </div>
    </div>
  `;
}

function bindMixerControls(container: HTMLElement): void {
  const masterGainSlider = container.querySelector<HTMLInputElement>("#mixer-master-gain");
  if (masterGainSlider) {
    masterGainSlider.addEventListener("input", () => {
      const val = parseFloat(masterGainSlider.value);
      setMasterGain(isFinite(val) ? val : 1.0);
    });
  }

  const limiterCheckbox = container.querySelector<HTMLInputElement>("#mixer-limiter");
  if (limiterCheckbox) {
    limiterCheckbox.addEventListener("change", () => {
      setLimiterEnabled(Boolean(limiterCheckbox.checked));
    });
  }

  container.querySelectorAll<HTMLElement>(".mixer-row").forEach((row) => {
    const pid = row.dataset.presetId || "";
    const mixEl = row.querySelector<HTMLInputElement>(".mixer-mix");
    const panEl = row.querySelector<HTMLInputElement>(".mixer-pan");
    const muteEl = row.querySelector<HTMLInputElement>(".mixer-mute");
    const soloEl = row.querySelector<HTMLInputElement>(".mixer-solo");
    if (mixEl) {
      mixEl.addEventListener("input", () => {
        const v = parseFloat(mixEl.value);
        setPresetMix(pid, isFinite(v) ? v : 1.0);
        if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].mix = isFinite(v) ? v : 1.0;
      });
    }
    if (panEl) {
      panEl.addEventListener("input", () => {
        const v = parseFloat(panEl.value);
        setPresetPan(pid, isFinite(v) ? v : 0.0);
        if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].pan = isFinite(v) ? v : 0.0;
      });
    }
    if (muteEl) {
      muteEl.addEventListener("change", () => {
        const v = Boolean(muteEl.checked);
        setPresetMute(pid, v);
        if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].mute = v;
      });
    }
    if (soloEl) {
      soloEl.addEventListener("change", () => {
        const v = Boolean(soloEl.checked);
        setPresetSolo(pid, v);
        if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].solo = v;
      });
    }
  });
}

export function renderMixerPanel(): void {
  const panel = document.getElementById("mixer-panel");
  if (!panel) return;
  panel.innerHTML = buildMixerMarkup();
  bindMixerControls(panel);
}

export function renderPresetList(
  presets: Preset[],
  activePresetId: string | null,
  onSelect: (presetId: string) => void | Promise<void>,
): void {
  if (!presetListElement) {
    return;
  }

  if (!presets.length) {
    presetListElement.innerHTML = '<p class="empty">No presets available.</p>';
    return;
  }

  presetListElement.innerHTML = presets
    .map(
      (preset) => `
        <article class="preset ${preset.id === activePresetId ? "active" : ""}" data-id="${preset.id}">
          <header>
            <h3>${escapeHtml(preset.name)}</h3>
            <span>${escapeHtml(preset.category ?? "")}</span>
          </header>
          <p>${escapeHtml(preset.description ?? "")}</p>
        </article>
      `,
    )
    .join("");

  presetListElement.querySelectorAll("article.preset").forEach((element) => {
    element.addEventListener("click", async () => {
      const presetId = element.getAttribute("data-id");
      if (presetId) {
        await onSelect(presetId);
      }
    });
  });
}

export function renderPresetDetails(
  preset: Preset | null,
  hooks: RenderHooks,
): void {
  if (!presetDetailsElement) return;

  if (!preset) {
    const parameterSection = renderParameterSection();
    const demoSection = renderDemoAudioControls();
    presetDetailsElement.innerHTML = `
      <div class="signal-chain-container">
        <div class="signal-chain-header">
          <div class="preset-info">
            <h2 class="preset-title">No Preset Loaded</h2>
            <p class="preset-description">Select a preset to see details.</p>
          </div>
        </div>
        ${demoSection}
        ${parameterSection}
      </div>
    `;
    const signalTestButton = document.getElementById("run-signal-test");
    if (signalTestButton) {
      signalTestButton.addEventListener("click", hooks.onRequestSignalTest);
    }
    bindDemoAudioControls();
    hooks.onBindLoadButtons();
    return;
  }

  const attachmentCards = (preset.attachments ?? [])
    .map((attachment) => {
      const icon = attachment.type === "audiofx" ? "🎸" : attachment.type === "ir" ? "🔊" : "📦";
      const label = attachment.type === "audiofx" ? "Amp Model" : attachment.type === "ir" ? "Cabinet IR" : attachment.type;
      const hashShort = attachment.hash ? `${attachment.hash.substring(0, 12)}...` : "N/A";
      return `
        <div class="attachment-card">
          <div class="attachment-icon">${icon}</div>
          <div class="attachment-info">
            <span class="attachment-type">${label}</span>
            <span class="attachment-hash" title="${escapeHtml(attachment.hash ?? "")}">${hashShort}</span>
          </div>
          <div class="attachment-status active"></div>
        </div>
      `;
    })
    .join("");

  const fxChainNodes = (preset.fxChain ?? [])
    .map((stage) => {
      const icon = stage === "noise_gate" ? "🔇" : stage === "compressor" ? "📊" : stage === "eq" ? "🎚️" : "⚡";
      return `
        <div class="fx-node">
          <div class="fx-node-icon">${icon}</div>
          <span class="fx-node-label">${stage.replace(/_/g, " ")}</span>
        </div>
        <div class="fx-connector"></div>
      `;
    })
    .join("");

  const fxChainContent = fxChainNodes || '<span class="fx-empty">No effects in chain</span>';

  function renderMixerSection(): string {
    const mixer = uiState.mixer;
    if (!mixer || !mixer.activePresetIds.length) {
      return "";
    }

    const rows = mixer.activePresetIds.map((id) => {
      const presetName = uiState.presetCache.get(id)?.name ?? id;
      const ps = mixer.presets[id] ?? { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
      return `
        <div class="mixer-row" data-preset-id="${escapeHtml(id)}">
          <div class="mixer-row-header">
            <span class="mixer-row-name">${escapeHtml(presetName)}</span>
            <label class="toggle"><input type="checkbox" class="mixer-solo" ${ps.solo ? "checked" : ""}/> Solo</label>
            <label class="toggle"><input type="checkbox" class="mixer-mute" ${ps.mute ? "checked" : ""}/> Mute</label>
          </div>
          <div class="mixer-controls">
            <label class="mixer-control">
              <span>Mix</span>
              <input type="range" class="mixer-mix" min="0" max="1" step="0.01" value="${ps.mix}"/>
            </label>
            <label class="mixer-control">
              <span>Pan</span>
              <input type="range" class="mixer-pan" min="-1" max="1" step="0.01" value="${ps.pan}"/>
            </label>
          </div>
        </div>
      `;
    }).join("");

    return `
      <div class="signal-chain-section">
        <h3 class="section-title">
          <span class="section-icon">🎛️</span>
          Mixer
        </h3>
        <div class="mixer-master">
          <label class="mixer-control">
            <span>Master Gain</span>
            <input type="range" id="mixer-master-gain" min="0" max="2" step="0.01" value="${mixer.masterGain}"/>
          </label>
          <label class="toggle">
            <input type="checkbox" id="mixer-limiter" ${mixer.limiterEnabled ? "checked" : ""}/> Limiter
          </label>
        </div>
        <div class="mixer-rows">
          ${rows}
        </div>
      </div>
    `;
  }

  presetDetailsElement.innerHTML = `
    <div class="signal-chain-container">
      <div class="signal-chain-header">
        <div class="preset-info">
          <h2 class="preset-title">${escapeHtml(preset.name)}</h2>
          <p class="preset-description">${escapeHtml(preset.description ?? "")}</p>
        </div>
        <div class="preset-actions">
          <button id="apply-preset" class="apply-btn">Apply Preset</button>
          <button id="add-preset-to-mixer" class="apply-btn" title="Add this preset to the active mixer">Add to Mixer</button>
        </div>
      </div>

      <div class="signal-chain-section">
        <h3 class="section-title">
          <span class="section-icon">🔗</span>
          Signal Chain
        </h3>
        <div class="fx-chain-flow">
          <div class="fx-node input-node">
            <div class="fx-node-icon">🎸</div>
            <span class="fx-node-label">Input</span>
          </div>
          <div class="fx-connector"></div>
          ${fxChainContent}
          <div class="fx-node output-node">
            <div class="fx-node-icon">🔊</div>
            <span class="fx-node-label">Output</span>
          </div>
        </div>
      </div>

      <div class="signal-chain-section">
        <h3 class="section-title">
          <span class="section-icon">📦</span>
          Loaded Components
        </h3>
        <div class="attachments-grid">
          ${attachmentCards || '<span class="no-attachments">No components loaded</span>'}
        </div>
      </div>

      ${renderDemoAudioControls()}
      ${renderParameterSection()}
      <div class="mixer-panel-host">${buildMixerMarkup()}</div>
    </div>
  `;

  const applyButton = document.getElementById("apply-preset");
  if (applyButton) {
    applyButton.addEventListener("click", async () => {
      await hooks.onApplyPreset(preset.id);
    });
  }

  const addToMixerButton = document.getElementById("add-preset-to-mixer");
  if (addToMixerButton) {
    addToMixerButton.addEventListener("click", () => {
      addActivePreset(preset.id);
    });
  }

  const signalTestButton = document.getElementById("run-signal-test");
  if (signalTestButton) {
    signalTestButton.addEventListener("click", hooks.onRequestSignalTest);
  }
  bindDemoAudioControls();
  hooks.onBindLoadButtons();

  const mixerHost = presetDetailsElement.querySelector(".mixer-panel-host") as HTMLElement | null;
  if (mixerHost) {
    bindMixerControls(mixerHost);
  }
}

export function updateDSPPerformancePlot(): void {
  const canvas = document.getElementById("performance-plot") as HTMLCanvasElement;
  if (!canvas) return;

  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const history = uiState.dspPerformanceHistory;
  if (history.length === 0) return;

  // Clear canvas
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  // Set up drawing
  ctx.strokeStyle = "#00ff00";
  ctx.lineWidth = 2;
  ctx.beginPath();

  const width = canvas.width;
  const height = canvas.height;
  const maxLoad = Math.max(...history.map(h => h.dspLoadPercent), 100); // At least 100% for scale

  // Draw line
  history.forEach((stat, index) => {
    const x = (index / (history.length - 1)) * width;
    const y = height - (stat.dspLoadPercent / maxLoad) * height;
    if (index === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });

  ctx.stroke();

  // Draw grid lines
  ctx.strokeStyle = "#333";
  ctx.lineWidth = 1;
  ctx.setLineDash([5, 5]);

  // Horizontal grid (25%, 50%, 75%)
  for (let percent = 25; percent < 100; percent += 25) {
    const y = height - (percent / maxLoad) * height;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }

  ctx.setLineDash([]);

  // Update current values
  const currentStat = history[history.length - 1];
  const loadValue = document.getElementById("dsp-load-value");
  if (loadValue) {
    loadValue.textContent = `${currentStat.dspLoadPercent.toFixed(1)}%`;
  }

  const peakValue = document.getElementById("dsp-peak-value");
  if (peakValue) {
    const peakLoad = Math.max(...history.map(h => h.dspLoadPercent));
    peakValue.textContent = `${peakLoad.toFixed(1)}%`;
  }

  // Update per-effect details
  const detailsList = document.getElementById("performance-details-list");
  if (detailsList && currentStat.nodeProcessingTimesUs) {
    detailsList.innerHTML = Object.entries(currentStat.nodeProcessingTimesUs)
      .map(([nodeId, timeUs]) => `<div class="performance-detail-item">${nodeId}: ${(timeUs as number).toFixed(1)} μs</div>`)
      .join("");
  }
}
