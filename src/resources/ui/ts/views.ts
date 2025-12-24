import { renderDemoAudioControls, bindDemoAudioControls } from "./demoAudio.js";
import { uiState } from "./state.js";
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
              <span class="stat-label">Duration</span>
              <span class="stat-value">${uiState.signalTest.duration.toFixed(2)} s</span>
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

  if (!parameterItems && !gateStatus && !uiState.parameters.modelPath && !uiState.parameters.irPath) {
    return `
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

  const signalPathCards = `
    <div class="signal-path-cards">
      <div class="path-card">
        <div class="path-icon">🎸</div>
        <div class="path-info">
          <span class="path-label">Amp Model</span>
          <span class="path-value">${uiState.parameters.modelPath || "None"}</span>
        </div>
        <div class="path-actions">
          <button class="load-btn" id="load-model-btn">Load Model</button>
        </div>
      </div>
      <div class="path-card">
        <div class="path-icon">🔊</div>
        <div class="path-info">
          <span class="path-label">Cabinet IR</span>
          <span class="path-value">${uiState.parameters.irPath || "None"}</span>
        </div>
        <div class="path-actions">
          <button class="load-btn" id="load-ir-btn">Load IR</button>
        </div>
      </div>
    </div>
  `;

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
        <span class="section-icon">🛤️</span>
        Active Signal Path
      </h3>
      ${signalPathCards}
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

  presetDetailsElement.innerHTML = `
    <div class="signal-chain-container">
      <div class="signal-chain-header">
        <div class="preset-info">
          <h2 class="preset-title">${escapeHtml(preset.name)}</h2>
          <p class="preset-description">${escapeHtml(preset.description ?? "")}</p>
        </div>
        <button id="apply-preset" class="apply-btn">Apply Preset</button>
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
    </div>
  `;

  const applyButton = document.getElementById("apply-preset");
  if (applyButton) {
    applyButton.addEventListener("click", async () => {
      await hooks.onApplyPreset(preset.id);
    });
  }

  const signalTestButton = document.getElementById("run-signal-test");
  if (signalTestButton) {
    signalTestButton.addEventListener("click", hooks.onRequestSignalTest);
  }
  bindDemoAudioControls();
  hooks.onBindLoadButtons();
}
