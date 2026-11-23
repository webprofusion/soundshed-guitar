import { renderDemoAudioControls, bindDemoAudioControls } from "./demoAudio.js";
import { uiState, setFocusedMixerPresetId } from "./state.js";
import { addMixerSlot, removeMixerSlot, setMixerMixGainDb, updateMixerSlot } from "./mixerStore.js";
import { cachePreset } from "./presetLibraryStore.js";
import { addActivePreset, removeActivePreset, setPresetMix, setPresetPan, setPresetMute, setPresetSolo, setMixGainDb, focusMixerPreset } from "./bridge.js";
import { escapeHtml, idAccentColor } from "./utils.js";
import { updateSignalPathClipIndicators, renderSignalPathBar } from "./signalPath.js";
import { renderIcon, getCheckmarkSvg, getXMarkSvg, getPlaySvg } from "./iconAssets.js";
import { enhanceRangeInput, formatGainDb } from "./controls.js";
import { MIX_GAIN_MAX_DB, MIX_GAIN_MIN_DB } from "./multiPresetMixerSupport.js";
import { getFriendlyGraphNodeName, getGraphTopologicalOrder, renderDspNodeTableHtml } from "./dspNodeTable.js";
import type { DSPPerformanceStats, Preset, PresetFolder, SignalLevelDiagnostics, SignalPeakHoldEntry } from "./types.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import { createVuMeter } from "./vuMeter.js";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } from "./presets/sorting.js";
import { beginPresetFolderDrag } from "./presets/folderDrag.js";

const presetListElement = document.getElementById("preset-list");
const presetDetailsElement = document.getElementById("preset-details");
const presetFolderTreeElement = document.getElementById("preset-folder-tree");


interface RenderHooks {
  onPresetSelected: (presetId: string) => Promise<void> | void;
  onApplyPreset: (presetId: string) => Promise<void> | void;
  onRequestSignalTest: () => void;
  onBindLoadButtons: () => void;
}

/** Renders a parameter value for display. The value is whatever the backend sent. */
function formatParameterValue(parameter: { value: unknown }): string {
  const { value } = parameter;
  if (typeof value === "number") return value.toFixed(2);
  if (typeof value === "boolean") return value ? "On" : "Off";
  if (typeof value === "string") return value;
  if (value === null || value === undefined) return "";
  // Values arrive as parsed JSON, so anything left is an object or an array.
  // Show its shape rather than the useless "[object Object]".
  return JSON.stringify(value) ?? "";
}

function renderParameterSection(): string {
  const parameterItems = (uiState.parameters.values ?? [])
    .map((parameter) => {
      const label = (parameter as { label?: string; id?: string }).label ?? (parameter as { id?: string }).id ?? "";
      return `
        <div class="param-card">
          <span class="param-label">${escapeHtml(label)}</span>
          <span class="param-value">${formatParameterValue(parameter)}</span>
        </div>
      `;
    })
    .join("");

  const signalTestSection = uiState.signalTest
    ? `
        <div class="test-results ${uiState.signalTest.passed ? "passed" : "failed"}">
          <div class="test-header">
            <span class="test-icon">${uiState.signalTest.passed ? getCheckmarkSvg() : getXMarkSvg()}</span>
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
        <p>Open the <strong>Preset Library</strong> and click <strong>+ Mixer</strong> on any preset to add it here.</p>
      </div>
    `;
  }

  const focusedId = uiState.focusedMixerPresetId ?? mixer.activePresetIds[0];

  const rows = mixer.activePresetIds.map((id) => {
    const presetName = uiState.presetCache.get(id)?.name ?? id;
    const ps = mixer.presets[id] ?? { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
    const isFocused = id === focusedId;
    return `
      <div class="mixer-row${isFocused ? " mixer-row-focused" : ""}" data-preset-id="${escapeHtml(id)}">
        <div class="mixer-row-header">
          <span class="mixer-row-accent" style="background:${idAccentColor(id)}"></span>
          <span class="mixer-row-name">${escapeHtml(presetName)}</span>
          <label class="toggle mini-toggle"><input type="checkbox" class="mixer-solo" ${ps.solo ? "checked" : ""}/> Solo</label>
          <label class="toggle mini-toggle"><input type="checkbox" class="mixer-mute" ${ps.mute ? "checked" : ""}/> Mute</label>
          <button class="mixer-view-chain-btn icon-btn" title="View signal chain" type="button">${getPlaySvg()}</button>
          <button class="mixer-remove-btn icon-btn" title="Remove from mixer" type="button">×</button>
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
          <span>Mix Out ${formatGainDb(mixer.mixGainDb)}</span>
          <input type="range" id="mixer-master-gain" min="${MIX_GAIN_MIN_DB}" max="${MIX_GAIN_MAX_DB}" step="0.1" value="${mixer.mixGainDb}"/>
        </label>
        <button class="mixer-save-multi-rig-btn secondary-btn" id="mixer-save-multi-rig" type="button" title="Save current mixer as a Multi-Rig preset">Save Multi-Rig…</button>
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
    enhanceRangeInput(masterGainSlider);
    masterGainSlider.addEventListener("input", () => {
      const raw = parseFloat(masterGainSlider.value);
      const val = isFinite(raw) ? raw : 0.0;
      setMixGainDb(val);
      setMixerMixGainDb(val);
      const label = masterGainSlider.parentElement?.querySelector("span");
      if (label) label.textContent = `Mix Out ${formatGainDb(val)}`;
    });
  }

  // Save Multi-Rig button — only shows when 2+ presets active; handled by multiPresetMixer.ts
  const saveMultiRigBtn = container.querySelector<HTMLButtonElement>("#mixer-save-multi-rig");
  if (saveMultiRigBtn) {
    saveMultiRigBtn.addEventListener("click", () => {
      const event = new CustomEvent("mixerSaveMultiRig", { bubbles: true });
      container.dispatchEvent(event);
    });
  }

  container.querySelectorAll<HTMLElement>(".mixer-row").forEach((row) => {
    const pid = row.dataset.presetId || "";
    const mixEl = row.querySelector<HTMLInputElement>(".mixer-mix");
    const panEl = row.querySelector<HTMLInputElement>(".mixer-pan");
    const muteEl = row.querySelector<HTMLInputElement>(".mixer-mute");
    const soloEl = row.querySelector<HTMLInputElement>(".mixer-solo");
    const viewBtn = row.querySelector<HTMLButtonElement>(".mixer-view-chain-btn");
    const removeBtn = row.querySelector<HTMLButtonElement>(".mixer-remove-btn");

    if (mixEl) {
      enhanceRangeInput(mixEl);
    }
    if (panEl) {
      enhanceRangeInput(panEl);
    }

    if (viewBtn) {
      viewBtn.addEventListener("click", () => {
        setFocusedMixerPresetId(pid);
        focusMixerPreset(pid);
        renderSignalPathBar();
        renderMixerPanel();
      });
    }

    if (removeBtn) {
      removeBtn.addEventListener("click", () => {
        removeActivePreset(pid);
        removeMixerSlot(pid);
        if (uiState.focusedMixerPresetId === pid) {
          uiState.focusedMixerPresetId = uiState.mixer?.activePresetIds[0] ?? null;
        }
        renderMixerPanel();
        renderSignalPathBar();
      });
    }

    if (mixEl) {
      mixEl.addEventListener("input", () => {
        const v = parseFloat(mixEl.value);
        setPresetMix(pid, isFinite(v) ? v : 1.0);
        updateMixerSlot(pid, { mix: isFinite(v) ? v : 1.0 });
      });
    }
    if (panEl) {
      panEl.addEventListener("input", () => {
        const v = parseFloat(panEl.value);
        setPresetPan(pid, isFinite(v) ? v : 0.0);
        updateMixerSlot(pid, { pan: isFinite(v) ? v : 0.0 });
      });
    }
    if (muteEl) {
      muteEl.addEventListener("change", () => {
        const v = Boolean(muteEl.checked);
        setPresetMute(pid, v);
        updateMixerSlot(pid, { mute: v });
        renderSignalPathBar(); // refresh tab indicators
      });
    }
    if (soloEl) {
      soloEl.addEventListener("change", () => {
        const v = Boolean(soloEl.checked);
        setPresetSolo(pid, v);
        updateMixerSlot(pid, { solo: v });
        renderSignalPathBar(); // refresh tab indicators
      });
    }
  });
}

export function renderMixerPanel(): void {
  // Note: buildMixerMarkup() renders into ".mixer-panel-host" (see renderPresetDetails);
  // there is no element with id="mixer-panel".
  const panel = document.querySelector<HTMLElement>(".mixer-panel-host");
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
    onMoveFolder: (folderId: string, targetParentId: string) => void;
    getRating: (presetId: string) => number | null;
    onRate: (presetId: string, rating: number | null) => void;
    getFolderPath?: (presetId: string) => string | null;
    recentsCount: number;
    recentsActive: boolean;
    onSelectRecents: () => void;
    favoritesCount: number;
    favoritesActive: boolean;
    onSelectFavorites: () => void;
    hasAnyPresets?: boolean;
    onOpenToneSharing?: () => void;
  },
): void {
  if (!presetListElement) {
    return;
  }

  if (presetFolderTreeElement && options) {
    const { folders, activeFolderId, onSelectFolder, onMoveFolder, recentsCount, recentsActive, onSelectRecents, favoritesCount, favoritesActive, onSelectFavorites } = options;
    const activeId = activeFolderId ?? PRESET_FOLDER_ALL_ID;
    const allPresetCount = uiState.presets.length;

    const renderFolderTree = (nodes: PresetFolder[], depth: number, parentId: string): string =>
      nodes
        .map((folder) => {
          const indent = `<span class="preset-folder-indent" style="margin-left: ${depth * 12}px"></span>`;
          const count = folder.presetIds.length;
          const childMarkup = folder.children?.length ? renderFolderTree(folder.children, depth + 1, folder.id) : "";
          return `
            <div class="preset-folder-item ${activeId === folder.id ? "active" : ""}" data-folder-id="${escapeHtml(folder.id)}" data-parent-id="${escapeHtml(parentId)}" data-depth="${depth}">
              ${indent}
              <span class="folder-name">${escapeHtml(folder.name)}</span>
              <span class="folder-count">${count}</span>
            </div>
            ${childMarkup}
          `;
        })
        .join("");

    presetFolderTreeElement.innerHTML = `
      <div class="preset-folder-item ${recentsActive ? "active" : ""}" data-folder-id="${PRESET_FOLDER_RECENTS_ID}">
        <span class="folder-name">Recents</span>
        <span class="folder-count">${recentsCount}</span>
      </div>
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
        if (folderId === PRESET_FOLDER_RECENTS_ID) {
          onSelectRecents();
        } else if (folderId === PRESET_FOLDER_FAVORITES_ID) {
          onSelectFavorites();
        } else {
          onSelectFolder(folderId);
        }
      });

      // Folders move by pointer (presets/folderDrag.ts), and presets land on them by pointer
      // too (presets/drag.ts). A file from the OS is left for the global importer.
      item.addEventListener("pointerdown", (event) => {
        beginPresetFolderDrag(event, item, presetFolderTreeElement, onMoveFolder);
      });
    });
  }

  if (!presets.length) {
    const hasAnyPresets = options?.hasAnyPresets ?? uiState.presets.length > 0;

    if (hasAnyPresets) {
      presetListElement.innerHTML = '<p class="preset-library-empty">No presets match your current search or folder.</p>';
      return;
    }

    presetListElement.innerHTML = `
      <div class="preset-library-empty">
        <p>No presets available yet.</p>
        <button type="button" class="btn btn-secondary" data-empty-preset-cta="tone-sharing">Browse Tone Sharing</button>
      </div>
    `;

    const ctaButton = presetListElement.querySelector<HTMLButtonElement>('[data-empty-preset-cta="tone-sharing"]');
    if (ctaButton && options?.onOpenToneSharing) {
      ctaButton.addEventListener("click", () => {
        options.onOpenToneSharing?.();
      });
    }
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
        const tagChips = (preset.tags ?? []).length > 0
          ? `<div class="preset-item-tags">${(preset.tags ?? []).map((t) => `<span class="preset-item-tag">${escapeHtml(t)}</span>`).join("")}</div>`
          : "";
        const inMixer = uiState.mixer?.activePresetIds.includes(preset.id) ?? false;
        const showMixerControls = isFeatureEnabled(Features.MultiRig);
        const addToMixerBtn = showMixerControls
          ? `<button class="preset-add-to-mixer-btn${inMixer ? " in-mixer" : ""}" data-preset-id="${escapeHtml(preset.id)}" title="${inMixer ? "Already in mixer" : "Add to mixer"}" type="button">${inMixer ? `${getCheckmarkSvg()} In Mixer` : "+ Mixer"}</button>`
          : "";

        const isLoadingPreset = preset.id === uiState.presetLoadingId;

        return `
        <article class="preset-item ${preset.id === activePresetId ? "active" : ""}${isLoadingPreset ? " loading" : ""}" data-id="${escapeHtml(preset.id)}">
          <header>
            <h3>${escapeHtml(preset.name)}</h3>
            ${addToMixerBtn}
          </header>
          ${metaParts ? `<div class="preset-item-meta">${metaParts}</div>` : ""}
          ${tagChips}
          <p>${escapeHtml(preset.description ?? "")}</p>
          <div class="preset-rating" data-preset-id="${escapeHtml(preset.id)}">
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

    // Add-to-mixer button — toggles inclusion in mixer
    const addBtn = element.querySelector<HTMLButtonElement>(".preset-add-to-mixer-btn");
    if (addBtn) {
      addBtn.addEventListener("click", (event) => {
        event.stopPropagation();
        const pid = addBtn.dataset.presetId ?? "";
        if (!pid) return;
        const alreadyIn = uiState.mixer?.activePresetIds.includes(pid) ?? false;
        // Changing mixer membership means it no longer matches whatever
        // Multi-Rig preset it was loaded from/saved as, if any.
        uiState.activeCompositePresetId = null;
        if (alreadyIn) {
          removeActivePreset(pid);
          removeMixerSlot(pid);
          addBtn.textContent = "+ Mixer";
          addBtn.classList.remove("in-mixer");
          addBtn.title = "Add to mixer";
          renderSignalPathBar();
          renderMixerPanel();
        } else {
          // Cache the preset's full data immediately so the signal chain tab is
          // populated before the C++ round-trip completes.
          const cachedPreset = uiState.presetCache.get(pid);
          if (!cachedPreset?.graph?.nodes?.length) {
            const fromList = uiState.presets.find((p) => p.id === pid);
            if (fromList?.graph?.nodes?.length) {
              cachePreset(fromList, pid);
            }
          }
          addActivePreset(pid);
          addMixerSlot(pid, uiState.presets.find((p) => p.id === pid)?.name ?? pid);
          addBtn.innerHTML = `${getCheckmarkSvg()} In Mixer`;
          addBtn.classList.add("in-mixer");
          addBtn.title = "Remove from mixer";
          renderSignalPathBar();
        }
      });
    }
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

function getPresetSignalChainStages(preset: Preset | null): string[] {
  if (!preset) {
    return [];
  }

  if (Array.isArray(preset.fxChain) && preset.fxChain.length > 0) {
    return preset.fxChain;
  }

  const graph = preset.graph?.nodes?.length
    ? preset.graph
    : preset.scenes?.find((scene) => scene.graph?.nodes?.length > 0)?.graph;

  if (!graph?.nodes?.length) {
    return [];
  }

  const order = getGraphTopologicalOrder(graph);
  return graph.nodes
    .slice()
    .sort((left, right) => (
      (order.get(left.id) ?? 9999) - (order.get(right.id) ?? 9999)
      || left.id.localeCompare(right.id)
    ))
    .filter((node) => node.id !== "__input__" && node.id !== "__output__" && node.type !== "input" && node.type !== "output")
    .map((node) => getFriendlyGraphNodeName(node, node.type, node.id));
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
            <span class="attachment-type">${escapeHtml(label)}</span>
            <span class="attachment-hash" title="${escapeHtml(attachment.hash ?? "")}">${escapeHtml(hashShort)}</span>
          </div>
          <div class="attachment-status active"></div>
        </div>
      `;
    })
    .join("");

  const fxChainNodes = getPresetSignalChainStages(preset)
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
          <span class="fx-node-label">${escapeHtml(stage.replace(/_/g, " "))}</span>
        </div>
        <div class="fx-connector"></div>
      `;
    })
    .join("");

  const fxChainContent = fxChainNodes || '<span class="fx-empty">No effects in chain</span>';

  const isPresetLoading = Boolean(uiState.presetLoadingId && preset.id === uiState.presetLoadingId);

  presetDetailsElement.innerHTML = `
    <div class="signal-chain-container">
      ${isPresetLoading ? `<div class="preset-loading-overlay"><span>Loading preset…</span></div>` : ""}
      <div class="signal-chain-header">
        <div class="preset-info">
          <h2 class="preset-title">${escapeHtml(preset.name)}</h2>
          <p class="preset-description">${escapeHtml(preset.description ?? "")}</p>
        </div>
        <div class="preset-actions">
          <button id="apply-preset" class="btn btn-primary">Apply Preset</button>
          <button id="add-preset-to-mixer" class="btn btn-primary" title="Add this preset to the active mixer">Add to Mixer</button>
          <button id="create-default-preset" class="btn btn-primary" type="button" title="Choose a new preset configuration" aria-haspopup="menu" aria-expanded="false" aria-controls="new-preset-menu">New Preset &#9662;</button>
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

let dspPerformancePlotRafId: number | null = null;

function getPerformancePlotThemeColors(canvas: HTMLCanvasElement): {
  line: string;
  fill: string;
  grid: string;
} {
  const styles = window.getComputedStyle(canvas);
  return {
    line: styles.getPropertyValue("--performance-plot-line").trim() || "rgba(104, 184, 104, 0.95)",
    fill: styles.getPropertyValue("--performance-plot-fill").trim() || "rgba(104, 184, 104, 0.18)",
    grid: styles.getPropertyValue("--performance-plot-grid").trim() || "rgba(255, 255, 255, 0.14)",
  };
}

/** The sub-tab is remembered while Settings is closed, so Settings itself must be open too. */
function isDSPPerformanceTabVisible(): boolean {
  const panel = document.getElementById("equipment-tab-performance");
  return Boolean(panel && panel.classList.contains("active") && !panel.hasAttribute("hidden")
    && document.getElementById("panel-settings")?.classList.contains("active"));
}

/** Its Diagnostics sub-tab: the plot and the tables. The load figures above the sub-tabs show on every one. */
function isDSPDiagnosticsVisible(): boolean {
  const panel = document.getElementById("performance-diagnostics-panel");
  return isDSPPerformanceTabVisible() && Boolean(panel && !panel.hasAttribute("hidden"));
}

export function scheduleDSPPerformancePlotUpdate(): void {
  if (dspPerformancePlotRafId !== null) {
    return;
  }

  dspPerformancePlotRafId = window.requestAnimationFrame(() => {
    dspPerformancePlotRafId = null;
    updateDSPPerformancePlot();
  });
}

export function updateDSPPerformancePlot(): void {
  if (!isDSPPerformanceTabVisible()) return;

  const history = uiState.dspLoadHistoryPercent;
  if (history.length === 0) return;
  updateDSPPerformanceFigures(history);

  const canvas = document.getElementById("performance-plot") as HTMLCanvasElement;
  if (!canvas || !isDSPDiagnosticsVisible()) return;

  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const themeColors = getPerformancePlotThemeColors(canvas);

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
  const width = displayWidth;
  const height = displayHeight;
  const maxLoad = Math.max(...history, 100); // At least 100% for scale

  const sampleCount = Math.max(1, history.length - 1);
  const points = history.map((loadPercent, index) => ({
    x: (index / sampleCount) * width,
    y: height - (loadPercent / maxLoad) * height,
  }));

  ctx.beginPath();
  points.forEach((point, index) => {
    if (index === 0) {
      ctx.moveTo(point.x, point.y);
    } else {
      ctx.lineTo(point.x, point.y);
    }
  });
  ctx.lineTo(width, height);
  ctx.lineTo(0, height);
  ctx.closePath();
  ctx.fillStyle = themeColors.fill;
  ctx.fill();

  ctx.beginPath();
  points.forEach((point, index) => {
    if (index === 0) {
      ctx.moveTo(point.x, point.y);
    } else {
      ctx.lineTo(point.x, point.y);
    }
  });
  ctx.strokeStyle = themeColors.line;
  ctx.lineWidth = 2;
  ctx.stroke();

  // Draw grid lines
  ctx.strokeStyle = themeColors.grid;
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
}

/** DSP Load, Peak Load and I/O Latency, over the section's sub-tabs. */
function updateDSPPerformanceFigures(history: number[]): void {
  const currentLoad = history[history.length - 1];
  const loadValue = document.getElementById("dsp-load-value");
  if (loadValue) {
    loadValue.textContent = `${currentLoad.toFixed(1)}%`;
  }

  const peakValue = document.getElementById("dsp-peak-value");
  if (peakValue) {
    const peakLoad = Math.max(...history);
    peakValue.textContent = `${peakLoad.toFixed(1)}%`;
  }

  const latencyValue = document.getElementById("dsp-latency-value");
  if (latencyValue) {
    latencyValue.textContent = formatLatencyValue(uiState.dspPerformance);
  }
}

function formatDb(value: number): string {
  if (!isFinite(value)) {
    return "—";
  }
  return value.toFixed(1);
}

const PEAK_HOLD_WINDOW_MS = 10_000;

function updateSignalPeakHold(diagnostics: SignalLevelDiagnostics): void {
  const now = Date.now();
  const currentPresetId = uiState.activePresetId ?? null;
  const hold = uiState.signalPeakHold;

  const makeEntry = (peakDbfs: number): SignalPeakHoldEntry => ({
    peakDbfs,
    windowStartedAt: now,
  });

  if (!hold || hold.presetId !== currentPresetId) {
    uiState.signalPeakHold = {
      presetId: currentPresetId,
      rawInput: makeEntry(diagnostics.rawInput?.peakDbfs ?? diagnostics.input.peakDbfs),
      input: makeEntry(diagnostics.input.peakDbfs),
      output: makeEntry(diagnostics.output.peakDbfs),
      nodes: Object.fromEntries(
        (diagnostics.nodes ?? []).map((n) => [n.nodeId, makeEntry(n.levels.peakDbfs)])
      ),
    };
    return;
  }

  // Raw input
  const rawPeak = diagnostics.rawInput?.peakDbfs ?? diagnostics.input.peakDbfs;
  if (now - hold.rawInput.windowStartedAt > PEAK_HOLD_WINDOW_MS) {
    hold.rawInput = makeEntry(rawPeak);
  } else if (rawPeak >= hold.rawInput.peakDbfs) {
    hold.rawInput.peakDbfs = rawPeak;
  }

  // Input
  if (now - hold.input.windowStartedAt > PEAK_HOLD_WINDOW_MS) {
    hold.input = makeEntry(diagnostics.input.peakDbfs);
  } else if (diagnostics.input.peakDbfs >= hold.input.peakDbfs) {
    hold.input.peakDbfs = diagnostics.input.peakDbfs;
  }

  // Output
  if (now - hold.output.windowStartedAt > PEAK_HOLD_WINDOW_MS) {
    hold.output = makeEntry(diagnostics.output.peakDbfs);
  } else if (diagnostics.output.peakDbfs >= hold.output.peakDbfs) {
    hold.output.peakDbfs = diagnostics.output.peakDbfs;
  }

  // Per-node
  for (const node of diagnostics.nodes ?? []) {
    const existing = hold.nodes[node.nodeId];
    if (!existing) {
      hold.nodes[node.nodeId] = makeEntry(node.levels.peakDbfs);
    } else if (now - existing.windowStartedAt > PEAK_HOLD_WINDOW_MS) {
      hold.nodes[node.nodeId] = makeEntry(node.levels.peakDbfs);
    } else if (node.levels.peakDbfs >= existing.peakDbfs) {
      existing.peakDbfs = node.levels.peakDbfs;
    }
  }
}

function setClipStatusText(el: HTMLElement, text: string): void {
  const nodes = el.childNodes;
  for (let i = nodes.length - 1; i >= 0; i--) {
    if (nodes[i].nodeType === Node.TEXT_NODE) {
      nodes[i].textContent = ` ${text}`;
      break;
    }
  }
}

function formatLatencyValue(stats: DSPPerformanceStats | null | undefined): string {
  if (!stats) {
    return "—";
  }

  const latencySamples = stats.totalLatencySamples;
  if (typeof latencySamples !== "number" || !isFinite(latencySamples) || latencySamples < 0) {
    return "—";
  }

  const sampleRate = stats.sampleRate ?? 0;
  if (sampleRate > 0) {
    const latencyMs = (latencySamples / sampleRate) * 1000.0;
    return `${latencySamples} samples (${latencyMs.toFixed(2)} ms)`;
  }

  return `${latencySamples} samples`;
}

// The control bar carries two of these. The input meter shows the raw signal ahead of
// the IN knob; the output meter shows the final mix after master gain and auto-level,
// so it tracks the OUT knob and reads silence while the output is muted.
const inputVuMeter = createVuMeter({ segmentsId: "vu-segments", peakHoldId: "vu-peak-hold", readoutId: "input-vu-readout", gaugeId: "input-vu-gauge" });
const outputVuMeter = createVuMeter({ segmentsId: "output-vu-segments", peakHoldId: "output-vu-peak-hold", readoutId: "output-vu-readout", gaugeId: "output-vu-gauge" });

// Cache for signal diagnostics view to prevent unnecessary DOM updates.
let signalDiagnosticsLastInputPeakText: string | null = null;
let signalDiagnosticsLastInputRmsText: string | null = null;
let signalDiagnosticsLastInputHeadroomText: string | null = null;
let signalDiagnosticsLastInputClipped: boolean | null = null;
let signalDiagnosticsLastOutputPeakText: string | null = null;
let signalDiagnosticsLastOutputRmsText: string | null = null;
let signalDiagnosticsLastOutputHeadroomText: string | null = null;
let signalDiagnosticsLastOutputClipped: boolean | null = null;
let signalDiagnosticsLastDesignedPeakText: string | null = null;
let signalDiagnosticsLastListHtml: string | null = null;

/**
 * Applies a diagnostics frame to whatever needs it, twenty times a second.
 *
 * Only some of that is the Signal Diagnostics panel, and the panel is usually not on
 * screen — it lives in Settings → DSP Performance. Rendering it regardless meant
 * rebuilding a per-node table and escaping a few hundred cells into a container with
 * `display: none`, on every frame, for nobody.
 *
 * The other two are not optional: the peak hold is state the save-preset modal reads
 * back, and the input VU meter and the signal path's clip indicators are on the main
 * screen. Those run whether the panel is up or not.
 */
export function updateSignalDiagnosticsView(): void {
  const diagnostics = uiState.signalDiagnostics;

  if (diagnostics) {
    updateSignalPeakHold(diagnostics);
  }

  if (isDSPDiagnosticsVisible()) {
    renderSignalDiagnosticsPanel(diagnostics);
  }

  if (!diagnostics) {
    return;
  }

  inputVuMeter.update(diagnostics.rawInput ?? diagnostics.input);
  outputVuMeter.update(diagnostics.output);
  updateSignalPathClipIndicators();
}

/**
 * Writes one frame into the Signal Diagnostics panel. Called only while that panel is
 * visible, so the `signalDiagnosticsLast*` memos stay in step with the DOM: skipping a
 * frame skips the memo update with it, and the first frame after the panel reopens sees
 * the same difference the DOM does.
 */
function renderSignalDiagnosticsPanel(diagnostics: SignalLevelDiagnostics | null | undefined): void {
  const statusEl = document.getElementById("signal-diagnostics-status");
  if (statusEl) {
    statusEl.textContent = "Enabled";
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

  if (!diagnostics) {
    // Only update if previously had diagnostics
    if (signalDiagnosticsLastInputPeakText !== "—") {
      if (inputPeak) inputPeak.textContent = "—";
      if (inputRms) inputRms.textContent = "—";
      if (inputHeadroom) inputHeadroom.textContent = "—";
      if (inputClip) {
        inputClip.classList.remove("clip-on");
        inputClip.classList.add("clip-off");
        setClipStatusText(inputClip, "—");
      }
      if (outputPeak) outputPeak.textContent = "—";
      if (outputRms) outputRms.textContent = "—";
      if (outputHeadroom) outputHeadroom.textContent = "—";
      if (outputClip) {
        outputClip.classList.remove("clip-on");
        outputClip.classList.add("clip-off");
        setClipStatusText(outputClip, "—");
      }
      if (listEl) listEl.innerHTML = "<div class=\"performance-detail-item\">Waiting for signal data.</div>";
      signalDiagnosticsLastInputPeakText = "—";
      signalDiagnosticsLastInputRmsText = "—";
      signalDiagnosticsLastInputHeadroomText = "—";
      signalDiagnosticsLastInputClipped = null;
      signalDiagnosticsLastOutputPeakText = "—";
      signalDiagnosticsLastOutputRmsText = "—";
      signalDiagnosticsLastOutputHeadroomText = "—";
      signalDiagnosticsLastOutputClipped = null;
      signalDiagnosticsLastDesignedPeakText = "—";
      signalDiagnosticsLastListHtml = null;
    }
    return;
  }

  const input = diagnostics.input ?? {
    peakDbfs: Number.NaN,
    rmsDbfs: Number.NaN,
    headroomDb: Number.NaN,
    clipped: false,
    clipCount: 0,
  };
  const output = diagnostics.output ?? {
    peakDbfs: Number.NaN,
    rmsDbfs: Number.NaN,
    headroomDb: Number.NaN,
    clipped: false,
    clipCount: 0,
  };

  const hold = uiState.signalPeakHold;

  // Update input metrics only if changed
  const inputPeakText = `${formatDb(hold?.input.peakDbfs ?? input.peakDbfs)} dBFS`;
  if (inputPeakText !== signalDiagnosticsLastInputPeakText) {
    if (inputPeak) inputPeak.textContent = inputPeakText;
    signalDiagnosticsLastInputPeakText = inputPeakText;
  }

  const inputRmsText = `${formatDb(input.rmsDbfs)} dBFS`;
  if (inputRmsText !== signalDiagnosticsLastInputRmsText) {
    if (inputRms) inputRms.textContent = inputRmsText;
    signalDiagnosticsLastInputRmsText = inputRmsText;
  }

  const inputHeadroomText = `${formatDb(input.headroomDb)} dB`;
  if (inputHeadroomText !== signalDiagnosticsLastInputHeadroomText) {
    if (inputHeadroom) inputHeadroom.textContent = inputHeadroomText;
    signalDiagnosticsLastInputHeadroomText = inputHeadroomText;
  }

  if (input.clipped !== signalDiagnosticsLastInputClipped) {
    if (inputClip) {
      inputClip.classList.toggle("clip-on", input.clipped);
      inputClip.classList.toggle("clip-off", !input.clipped);
      setClipStatusText(inputClip, input.clipped ? `Clipping (${input.clipCount})` : "OK");
    }
    signalDiagnosticsLastInputClipped = input.clipped;
  }

  // Update output metrics only if changed
  const outputPeakText = `${formatDb(hold?.output.peakDbfs ?? output.peakDbfs)} dBFS`;
  if (outputPeakText !== signalDiagnosticsLastOutputPeakText) {
    if (outputPeak) outputPeak.textContent = outputPeakText;
    signalDiagnosticsLastOutputPeakText = outputPeakText;
  }

  const outputRmsText = `${formatDb(output.rmsDbfs)} dBFS`;
  if (outputRmsText !== signalDiagnosticsLastOutputRmsText) {
    if (outputRms) outputRms.textContent = outputRmsText;
    signalDiagnosticsLastOutputRmsText = outputRmsText;
  }

  const outputHeadroomText = `${formatDb(output.headroomDb)} dB`;
  if (outputHeadroomText !== signalDiagnosticsLastOutputHeadroomText) {
    if (outputHeadroom) outputHeadroom.textContent = outputHeadroomText;
    signalDiagnosticsLastOutputHeadroomText = outputHeadroomText;
  }

  if (output.clipped !== signalDiagnosticsLastOutputClipped) {
    if (outputClip) {
      outputClip.classList.toggle("clip-on", output.clipped);
      outputClip.classList.toggle("clip-off", !output.clipped);
      setClipStatusText(outputClip, output.clipped ? `Clipping (${output.clipCount})` : "OK");
    }
    signalDiagnosticsLastOutputClipped = output.clipped;
  }

  // Update the designed peak input control
  const designedPeakCurrentEl = document.getElementById("designed-peak-current-value");
  const designedPeakStoredEl = document.getElementById("designed-peak-stored-value");
  
  const rawPeakDbfs = hold?.rawInput.peakDbfs ?? diagnostics.rawInput?.peakDbfs ?? input.peakDbfs;
  const designedPeakText = `${formatDb(rawPeakDbfs)} dBFS`;
  if (designedPeakText !== signalDiagnosticsLastDesignedPeakText) {
    if (designedPeakCurrentEl) designedPeakCurrentEl.textContent = designedPeakText;
    signalDiagnosticsLastDesignedPeakText = designedPeakText;
  }
  
  // Show stored value from active preset
  if (designedPeakStoredEl) {
    const activePreset = uiState.activePresetId
      ? (uiState.presetCache.get(uiState.activePresetId) ?? uiState.presets.find((p) => p.id === uiState.activePresetId))
      : null;
    const stored = activePreset?.designedPeakInputDbfs;
    designedPeakStoredEl.textContent = stored != null ? `${formatDb(stored)} dBFS` : "\u2014";
  }

  if (listEl) {
    const newListHtml = renderDspNodeTableHtml(diagnostics, input, hold);

    if (newListHtml !== signalDiagnosticsLastListHtml) {
      listEl.innerHTML = newListHtml;
      signalDiagnosticsLastListHtml = newListHtml;
    }
  }
}
