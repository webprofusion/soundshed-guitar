import { renderDemoAudioControls, bindDemoAudioControls } from "./demoAudio.js";
import { uiState } from "./state.js";
import { addActivePreset, setPresetMix, setPresetPan, setPresetMute, setPresetSolo, setMasterGain, setLimiterEnabled } from "./bridge.js";
import { escapeHtml } from "./utils.js";
import { updateSignalPathClipIndicators } from "./signalPath.js";
import { renderIcon } from "./iconAssets.js";
import type { Preset, PresetFolder } from "./types.js";

const presetListElement = document.getElementById("preset-list");
const presetDetailsElement = document.getElementById("preset-details");
const presetFolderTreeElement = document.getElementById("preset-folder-tree");

const PRESET_FOLDER_ALL_ID = "__all__";
const PRESET_FOLDER_FAVORITES_ID = "__favorites__";

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
        <span class="section-icon">${renderIcon("gear", "section-icon-img")}</span>
        Current Parameters
      </h3>
      <div class="params-grid">
        ${parameterItems}
      </div>
    </div>
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">${renderIcon("microscope", "section-icon-img")}</span>
        Diagnostics
      </h3>
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
  options?: {
    folders: PresetFolder[];
    activeFolderId: string | null;
    onSelectFolder: (folderId: string) => void;
    onMovePresetToFolder: (presetId: string, folderId: string) => void;
    onMoveFolder: (folderId: string, targetParentId: string) => void;
    getRating: (presetId: string) => number | null;
    onRate: (presetId: string, rating: number | null) => void;
    getFolderPath?: (presetId: string) => string | null;
    favoritesCount: number;
    favoritesActive: boolean;
    onSelectFavorites: () => void;
  },
): void {
  if (!presetListElement) {
    return;
  }

  if (presetFolderTreeElement && options) {
    const { folders, activeFolderId, onSelectFolder, onMovePresetToFolder, onMoveFolder, favoritesCount, favoritesActive, onSelectFavorites } = options;
    const activeId = activeFolderId ?? PRESET_FOLDER_ALL_ID;
    const allPresetCount = uiState.presets.length;

    const renderFolderTree = (nodes: PresetFolder[], depth: number, parentId: string): string =>
      nodes
        .map((folder) => {
          const indent = `<span class="preset-folder-indent" style="margin-left: ${depth * 12}px"></span>`;
          const count = folder.presetIds.length;
          const childMarkup = folder.children?.length ? renderFolderTree(folder.children, depth + 1, folder.id) : "";
          return `
            <div class="preset-folder-item ${activeId === folder.id ? "active" : ""}" data-folder-id="${folder.id}" data-parent-id="${parentId}" data-depth="${depth}" draggable="true">
              ${indent}
              <span class="folder-name">${escapeHtml(folder.name)}</span>
              <span class="folder-count">${count}</span>
            </div>
            ${childMarkup}
          `;
        })
        .join("");

    presetFolderTreeElement.innerHTML = `
      <div class="preset-folder-item ${favoritesActive ? "active" : ""}" data-folder-id="${PRESET_FOLDER_FAVORITES_ID}">
        <span class="folder-name">Favourites</span>
        <span class="folder-count">${favoritesCount}</span>
      </div>
      <div class="preset-folder-item ${activeId === PRESET_FOLDER_ALL_ID ? "active" : ""}" data-folder-id="${PRESET_FOLDER_ALL_ID}">
        <span class="folder-name">All Presets</span>
        <span class="folder-count">${allPresetCount}</span>
      </div>
      ${renderFolderTree(folders, 0, PRESET_FOLDER_ALL_ID)}
    `;

    presetFolderTreeElement.querySelectorAll<HTMLElement>(".preset-folder-item").forEach((item) => {
      item.addEventListener("click", () => {
        const folderId = item.dataset.folderId ?? PRESET_FOLDER_ALL_ID;
        if (folderId === PRESET_FOLDER_FAVORITES_ID) {
          onSelectFavorites();
        } else {
          onSelectFolder(folderId);
        }
      });

      item.addEventListener("dragover", (event) => {
        event.preventDefault();
        item.classList.add("drag-over");
      });

      item.addEventListener("dragleave", () => {
        item.classList.remove("drag-over");
      });

      item.addEventListener("dragstart", (event) => {
        const folderId = item.dataset.folderId ?? "";
        if (!folderId || folderId === PRESET_FOLDER_FAVORITES_ID || folderId === PRESET_FOLDER_ALL_ID) {
          return;
        }
        event.dataTransfer?.setData("application/x-preset-folder", folderId);
        event.dataTransfer?.setDragImage(item, 20, 20);
      });

      item.addEventListener("drop", (event) => {
        event.preventDefault();
        item.classList.remove("drag-over");
        const folderDragId = event.dataTransfer?.getData("application/x-preset-folder") ?? "";
        const presetId = event.dataTransfer?.getData("text/plain") ?? "";
        const folderId = item.dataset.folderId ?? PRESET_FOLDER_ALL_ID;
        if (folderDragId) {
          if (folderId === PRESET_FOLDER_FAVORITES_ID) {
            return;
          }
          const offsetX = (event as DragEvent).offsetX ?? 0;
          const parentId = item.dataset.parentId ?? PRESET_FOLDER_ALL_ID;
          const depth = Number(item.dataset.depth ?? "0");
          const isIndentDrop = depth > 0 && offsetX < 16;
          const targetParentId = folderId === PRESET_FOLDER_ALL_ID
            ? PRESET_FOLDER_ALL_ID
            : (isIndentDrop ? parentId : folderId);
          onMoveFolder(folderDragId, targetParentId);
          return;
        }
        if (presetId) {
          onMovePresetToFolder(presetId, folderId);
        }
      });
    });
  }

  if (!presets.length) {
    presetListElement.innerHTML = '<p class="preset-library-empty">No presets available.</p>';
    return;
  }

  presetListElement.innerHTML = presets
    .map(
      (preset) => {
        const rating = options?.getRating(preset.id) ?? null;
        const stars = Array.from({ length: 5 }, (_, index) => {
          const value = index + 1;
          const active = rating !== null && value <= rating ? "active" : "";
          return `<button class="preset-rating-star ${active}" data-rating="${value}" type="button">★</button>`;
        }).join("");
        const label = rating === null ? "Not Rated" : `${rating}/5`;
        const folderPath = options?.getFolderPath?.(preset.id) ?? "";
        const category = preset.category ?? "";
        const metaParts = [
          category
            ? `<span class="preset-category-badge">${escapeHtml(category)}</span>`
            : "",
          folderPath
            ? `<span class="preset-folder-path">${escapeHtml(folderPath)}</span>`
            : "",
        ].filter(Boolean).join("");

        return `
        <article class="preset-item ${preset.id === activePresetId ? "active" : ""}" data-id="${preset.id}" draggable="true">
          <header>
            <h3>${escapeHtml(preset.name)}</h3>
          </header>
          ${metaParts ? `<div class="preset-item-meta">${metaParts}</div>` : ""}
          <p>${escapeHtml(preset.description ?? "")}</p>
          <div class="preset-rating" data-preset-id="${preset.id}">
            <span class="preset-rating-label">${label}</span>
            <div class="preset-rating-stars">
              ${stars}
            </div>
          </div>
        </article>
      `;
      },
    )
    .join("");

  presetListElement.querySelectorAll<HTMLElement>("article.preset-item").forEach((element) => {
    element.addEventListener("click", async () => {
      const presetId = element.getAttribute("data-id");
      if (presetId) {
        await onSelect(presetId);
      }
    });

    element.addEventListener("dragstart", (event) => {
      const presetId = element.getAttribute("data-id") ?? "";
      if (presetId) {
        event.dataTransfer?.setData("text/plain", presetId);
        event.dataTransfer?.setDragImage(element, 20, 20);
      }
    });
  });

  presetListElement.querySelectorAll<HTMLButtonElement>(".preset-rating-star").forEach((button) => {
    button.addEventListener("click", (event) => {
      event.stopPropagation();
      const wrapper = button.closest<HTMLElement>(".preset-rating");
      const presetId = wrapper?.dataset.presetId ?? "";
      const ratingValue = Number(button.dataset.rating ?? 0);
      if (!presetId || !options) {
        return;
      }
      const current = options.getRating(presetId);
      const next = current === ratingValue ? null : ratingValue;
      options.onRate(presetId, next);
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
      const icon = attachment.type === "audiofx"
        ? renderIcon("amp", "attachment-icon-img")
        : attachment.type === "ir"
          ? renderIcon("speaker", "attachment-icon-img")
          : renderIcon("package", "attachment-icon-img");
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
      const icon = stage === "dynamics_gate" || stage === "noise_gate"
        ? renderIcon("mute", "fx-node-icon-img")
        : stage === "compressor"
          ? renderIcon("meter", "fx-node-icon-img")
          : stage === "eq"
            ? renderIcon("sliders", "fx-node-icon-img")
            : renderIcon("bolt", "fx-node-icon-img");
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
          <span class="section-icon">${renderIcon("mixer", "section-icon-img")}</span>
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
          <button id="create-default-preset" class="apply-btn" title="Create a new preset from defaults">New Preset</button>
        </div>
      </div>

      <div class="signal-chain-section signal-chain-section-chain">
        <h3 class="section-title">
          <span class="section-icon">${renderIcon("link", "section-icon-img")}</span>
          Signal Chain
        </h3>
        <div class="fx-chain-flow">
          <div class="fx-node input-node">
            <div class="fx-node-icon">${renderIcon("amp", "fx-node-icon-img")}</div>
            <span class="fx-node-label">Input</span>
          </div>
          <div class="fx-connector"></div>
          ${fxChainContent}
          <div class="fx-node output-node">
            <div class="fx-node-icon">${renderIcon("speaker", "fx-node-icon-img")}</div>
            <span class="fx-node-label">Output</span>
          </div>
        </div>
        <button class="signal-chain-performance-link" type="button">Performance</button>
      </div>

      <div class="signal-chain-section">
        <h3 class="section-title">
          <span class="section-icon">${renderIcon("package", "section-icon-img")}</span>
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

  bindDemoAudioControls();
  hooks.onBindLoadButtons();

  const performanceLink = presetDetailsElement.querySelector(".signal-chain-performance-link") as HTMLButtonElement | null;
  if (performanceLink) {
    performanceLink.addEventListener("click", () => {
      const settingsButton = document.querySelector(".icon-bar .icon-btn[data-panel=\"settings\"]") as HTMLElement | null;
      settingsButton?.click();
      const performanceTabButton = document.querySelector(".equipment-tab-btn[data-equipment-tab=\"performance\"]") as HTMLElement | null;
      performanceTabButton?.click();
    });
  }

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

  const displayWidth = Math.max(1, Math.floor(canvas.clientWidth));
  const displayHeight = Math.max(1, Math.floor(canvas.clientHeight));
  if (displayWidth === 1 || displayHeight === 1) return;

  const dpr = window.devicePixelRatio || 1;
  const desiredWidth = Math.max(1, Math.floor(displayWidth * dpr));
  const desiredHeight = Math.max(1, Math.floor(displayHeight * dpr));
  if (canvas.width !== desiredWidth || canvas.height !== desiredHeight) {
    canvas.width = desiredWidth;
    canvas.height = desiredHeight;
  }

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  // Clear canvas
  ctx.clearRect(0, 0, displayWidth, displayHeight);

  // Set up drawing
  ctx.strokeStyle = "#00ff00";
  ctx.lineWidth = 2;
  ctx.beginPath();

  const width = displayWidth;
  const height = displayHeight;
  const maxLoad = Math.max(...history.map(h => h.dspLoadPercent), 100); // At least 100% for scale

  // Draw line
  const sampleCount = Math.max(1, history.length - 1);
  history.forEach((stat, index) => {
    const x = (index / sampleCount) * width;
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

function formatDb(value: number): string {
  if (!isFinite(value)) {
    return "—";
  }
  return value.toFixed(1);
}

export function updateSignalDiagnosticsView(): void {
  const diagnostics = uiState.signalDiagnostics;
  const enabled = Boolean(uiState.appSettings?.["diagnostics.signalLevelsEnabled"]);

  const statusEl = document.getElementById("signal-diagnostics-status");
  if (statusEl) {
    statusEl.textContent = enabled ? "Enabled" : "Disabled";
  }

  const inputPeak = document.getElementById("signal-input-peak");
  const inputRms = document.getElementById("signal-input-rms");
  const inputHeadroom = document.getElementById("signal-input-headroom");
  const inputClip = document.getElementById("signal-input-clipping");

  const outputPeak = document.getElementById("signal-output-peak");
  const outputRms = document.getElementById("signal-output-rms");
  const outputHeadroom = document.getElementById("signal-output-headroom");
  const outputClip = document.getElementById("signal-output-clipping");

  const listEl = document.getElementById("signal-diagnostics-list");

  if (!enabled || !diagnostics) {
    if (inputPeak) inputPeak.textContent = "—";
    if (inputRms) inputRms.textContent = "—";
    if (inputHeadroom) inputHeadroom.textContent = "—";
    if (inputClip) {
      inputClip.classList.remove("clip-on");
      inputClip.classList.add("clip-off");
      inputClip.childNodes.forEach((node) => {
        if (node.nodeType === Node.TEXT_NODE) {
          node.textContent = "—";
        }
      });
    }
    if (outputPeak) outputPeak.textContent = "—";
    if (outputRms) outputRms.textContent = "—";
    if (outputHeadroom) outputHeadroom.textContent = "—";
    if (outputClip) {
      outputClip.classList.remove("clip-on");
      outputClip.classList.add("clip-off");
      outputClip.childNodes.forEach((node) => {
        if (node.nodeType === Node.TEXT_NODE) {
          node.textContent = "—";
        }
      });
    }
    if (listEl) listEl.innerHTML = "<div class=\"performance-detail-item\">Diagnostics disabled.</div>";
    return;
  }

  if (inputPeak) inputPeak.textContent = `${formatDb(diagnostics.input.peakDbfs)} dBFS`;
  if (inputRms) inputRms.textContent = `${formatDb(diagnostics.input.rmsDbfs)} dBFS`;
  if (inputHeadroom) inputHeadroom.textContent = `${formatDb(diagnostics.input.headroomDb)} dB`;
  if (inputClip) {
    inputClip.classList.toggle("clip-on", diagnostics.input.clipped);
    inputClip.classList.toggle("clip-off", !diagnostics.input.clipped);
    inputClip.childNodes.forEach((node) => {
      if (node.nodeType === Node.TEXT_NODE) {
        node.textContent = diagnostics.input.clipped ? `Clipping (${diagnostics.input.clipCount})` : "OK";
      }
    });
  }

  if (outputPeak) outputPeak.textContent = `${formatDb(diagnostics.output.peakDbfs)} dBFS`;
  if (outputRms) outputRms.textContent = `${formatDb(diagnostics.output.rmsDbfs)} dBFS`;
  if (outputHeadroom) outputHeadroom.textContent = `${formatDb(diagnostics.output.headroomDb)} dB`;
  if (outputClip) {
    outputClip.classList.toggle("clip-on", diagnostics.output.clipped);
    outputClip.classList.toggle("clip-off", !diagnostics.output.clipped);
    outputClip.childNodes.forEach((node) => {
      if (node.nodeType === Node.TEXT_NODE) {
        node.textContent = diagnostics.output.clipped ? `Clipping (${diagnostics.output.clipCount})` : "OK";
      }
    });
  }

  if (listEl) {
    const rows = diagnostics.nodes
      .map((node) => {
        const scopeLabel = escapeHtml(node.scope);
        const presetLabel = node.presetId ? `${escapeHtml(node.presetId)} · ` : "";
        const nodeLabel = `${presetLabel}${escapeHtml(node.nodeId)} (${escapeHtml(node.nodeType)})`;
        const peak = formatDb(node.levels.peakDbfs);
        const headroom = formatDb(node.levels.headroomDb);
        const clipClass = node.levels.clipped ? "clip-on" : "clip-off";
        const clipText = node.levels.clipped ? `Clipping (${node.levels.clipCount})` : "OK";
        return `
          <div class=\"signal-diagnostics-item\">
            <span class=\"signal-diagnostics-cell scope\">${scopeLabel}</span>
            <span class=\"signal-diagnostics-cell node\">${nodeLabel}</span>
            <span class=\"signal-diagnostics-cell\">${peak} dBFS</span>
            <span class=\"signal-diagnostics-cell\">${headroom} dB</span>
            <span class=\"signal-diagnostics-cell clip ${clipClass}\">
              <span class=\"clip-indicator\"></span>
              ${clipText}
            </span>
          </div>
        `;
      })
      .join("");

    listEl.innerHTML = `
      <div class=\"signal-diagnostics-header\">
        <span class=\"signal-diagnostics-cell scope\">Scope</span>
        <span class=\"signal-diagnostics-cell node\">Node</span>
        <span class=\"signal-diagnostics-cell\">Peak</span>
        <span class=\"signal-diagnostics-cell\">Headroom</span>
        <span class=\"signal-diagnostics-cell\">Clip</span>
      </div>
      <div class=\"signal-diagnostics-columns\">
        ${rows}
      </div>
    `;
  }

  updateSignalPathClipIndicators();
}
