import { renderDemoAudioControls, bindDemoAudioControls } from "./demoAudio.js";
import { uiState, setFocusedMixerPresetId, isAdvancedOptionsEnabled } from "./state.js";
import { addActivePreset, removeActivePreset, setPresetMix, setPresetPan, setPresetMute, setPresetSolo, setMasterGain, setLimiterEnabled } from "./bridge.js";
import { escapeHtml, idAccentColor } from "./utils.js";
import { updateSignalPathClipIndicators, renderSignalPathBar } from "./signalPath.js";
import { renderIcon } from "./iconAssets.js";
import { EffectGuids } from "./effectGuids.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { enhanceRangeInput } from "./controls.js";
import type { GraphEdge, GraphNode, Preset, PresetFolder, SignalGraph, SignalLevelNodeMetrics } from "./types.js";

const presetListElement = document.getElementById("preset-list");
const presetDetailsElement = document.getElementById("preset-details");
const presetFolderTreeElement = document.getElementById("preset-folder-tree");

const PRESET_FOLDER_ALL_ID = "__all__";
const PRESET_FOLDER_FAVORITES_ID = "__favorites__";
const PRESET_FOLDER_RECENTS_ID = "__recents__";

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
          <button class="mixer-view-chain-btn icon-btn" title="View signal chain" type="button">▶</button>
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
          <span>Master Gain</span>
          <input type="range" id="mixer-master-gain" min="0" max="2" step="0.01" value="${mixer.masterGain}"/>
        </label>
        <label class="toggle mini-toggle">
          <input type="checkbox" id="mixer-limiter" ${mixer.limiterEnabled ? "checked" : ""}/> Limiter
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
        renderSignalPathBar();
        renderMixerPanel();
      });
    }

    if (removeBtn) {
      removeBtn.addEventListener("click", () => {
        removeActivePreset(pid);
        if (uiState.mixer) {
          uiState.mixer.activePresetIds = uiState.mixer.activePresetIds.filter((id) => id !== pid);
          delete uiState.mixer.presets[pid];
        }
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
        renderSignalPathBar(); // refresh tab indicators
      });
    }
    if (soloEl) {
      soloEl.addEventListener("change", () => {
        const v = Boolean(soloEl.checked);
        setPresetSolo(pid, v);
        if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].solo = v;
        renderSignalPathBar(); // refresh tab indicators
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
    const { folders, activeFolderId, onSelectFolder, onMovePresetToFolder, onMoveFolder, recentsCount, recentsActive, onSelectRecents, favoritesCount, favoritesActive, onSelectFavorites } = options;
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

      item.addEventListener("dragover", (event) => {
        event.preventDefault();
        item.classList.add("drag-over");
      });

      item.addEventListener("dragleave", () => {
        item.classList.remove("drag-over");
      });

      item.addEventListener("dragstart", (event) => {
        const folderId = item.dataset.folderId ?? "";
        if (!folderId || folderId === PRESET_FOLDER_RECENTS_ID || folderId === PRESET_FOLDER_FAVORITES_ID || folderId === PRESET_FOLDER_ALL_ID) {
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
          if (folderId === PRESET_FOLDER_RECENTS_ID || folderId === PRESET_FOLDER_FAVORITES_ID) {
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
        const showMixerControls = isAdvancedOptionsEnabled();
        const addToMixerBtn = showMixerControls
          ? `<button class="preset-add-to-mixer-btn${inMixer ? " in-mixer" : ""}" data-preset-id="${preset.id}" title="${inMixer ? "Already in mixer" : "Add to mixer"}" type="button">${inMixer ? "✓ In Mixer" : "+ Mixer"}</button>`
          : "";

        return `
        <article class="preset-item ${preset.id === activePresetId ? "active" : ""}" data-id="${preset.id}" draggable="true" style="border-left: 3px solid ${idAccentColor(preset.id)}">
          <header>
            <h3>${escapeHtml(preset.name)}</h3>
            ${addToMixerBtn}
          </header>
          ${metaParts ? `<div class="preset-item-meta">${metaParts}</div>` : ""}
          ${tagChips}
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

    // Add-to-mixer button — toggles inclusion in mixer
    const addBtn = element.querySelector<HTMLButtonElement>(".preset-add-to-mixer-btn");
    if (addBtn) {
      addBtn.addEventListener("click", (event) => {
        event.stopPropagation();
        const pid = addBtn.dataset.presetId ?? "";
        if (!pid) return;
        const alreadyIn = uiState.mixer?.activePresetIds.includes(pid) ?? false;
        if (alreadyIn) {
          removeActivePreset(pid);
          if (uiState.mixer) {
            uiState.mixer.activePresetIds = uiState.mixer.activePresetIds.filter((id) => id !== pid);
            delete uiState.mixer.presets[pid];
          }
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
              uiState.presetCache.set(pid, fromList);
            }
          }
          addActivePreset(pid);
          if (uiState.mixer) {
            uiState.mixer.activePresetIds.push(pid);
            if (!uiState.mixer.presets[pid]) {
              uiState.mixer.presets[pid] = { id: pid, mix: 1.0, pan: 0.0, mute: false, solo: false };
            }
          }
          addBtn.textContent = "✓ In Mixer";
          addBtn.classList.add("in-mixer");
          addBtn.title = "Remove from mixer";
          renderSignalPathBar();
        }
      });
    }

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

  const latencyValue = document.getElementById("dsp-latency-value");
  if (latencyValue) {
    latencyValue.textContent = formatLatencyValue(currentStat);
  }
}

function formatDb(value: number): string {
  if (!isFinite(value)) {
    return "—";
  }
  return value.toFixed(1);
}

const PEAK_HOLD_WINDOW_MS = 10_000;

function updateSignalPeakHold(diagnostics: import("./types.js").SignalLevelDiagnostics): void {
  const now = Date.now();
  const currentPresetId = uiState.activePresetId ?? null;
  const hold = uiState.signalPeakHold;

  const makeEntry = (peakDbfs: number): import("./types.js").SignalPeakHoldEntry => ({
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

function sortGraphEdges(edges: GraphEdge[]): GraphEdge[] {
  return edges.slice().sort((left, right) => (
    (left.fromPort - right.fromPort)
    || (left.toPort - right.toPort)
    || left.from.localeCompare(right.from)
    || left.to.localeCompare(right.to)
  ));
}

function getFriendlyGraphNodeName(node: GraphNode | undefined, fallbackNodeType: string, fallbackNodeId: string): string {
  if (node) {
    if (node.id === "__input__" || node.type === "input") return "Input";
    if (node.id === "__output__" || node.type === "output") return "Output";
    if (typeof node.displayName === "string" && node.displayName.trim()) {
      return node.displayName.trim();
    }
    const nodeTypeInfo = node.type ? EffectTypeRegistry.get(node.type) : undefined;
    return nodeTypeInfo?.displayName || node.type || fallbackNodeId || "(Unknown)";
  }

  const typeInfo = fallbackNodeType ? EffectTypeRegistry.get(fallbackNodeType) : undefined;
  return typeInfo?.displayName || fallbackNodeType || fallbackNodeId || "(Unknown)";
}

function getDiagnosticsGraphContext(node: SignalLevelNodeMetrics): { graph?: SignalGraph; presetName?: string } {
  if (node.scope === "pre") {
    return { graph: uiState.globalSignalChain?.preChainGraph };
  }

  if (node.scope === "post") {
    return { graph: uiState.globalSignalChain?.postChainGraph };
  }

  const presetId = node.presetId ?? uiState.activePresetId ?? "";
  if (!presetId) {
    return {};
  }

  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((entry) => entry.id === presetId);
  const mixerPresetName = uiState.mixer?.presets?.[presetId]?.name;
  return {
    graph: preset?.graph,
    presetName: mixerPresetName || preset?.name || presetId,
  };
}

function getSplitterBranchSuffix(graph: SignalGraph | undefined, nodeId: string): string {
  if (!graph || nodeId === "__input__" || nodeId === "__output__") {
    return "";
  }

  const nodeById = new Map(graph.nodes.map((node) => [node.id, node]));
  const incomingByNode = new Map<string, GraphEdge[]>();
  const outgoingByNode = new Map<string, GraphEdge[]>();

  for (const edge of graph.edges) {
    const incoming = incomingByNode.get(edge.to);
    if (incoming) {
      incoming.push(edge);
    } else {
      incomingByNode.set(edge.to, [edge]);
    }

    const outgoing = outgoingByNode.get(edge.from);
    if (outgoing) {
      outgoing.push(edge);
    } else {
      outgoingByNode.set(edge.from, [edge]);
    }
  }

  let currentNodeId = nodeId;
  const visited = new Set<string>();

  while (currentNodeId && !visited.has(currentNodeId)) {
    visited.add(currentNodeId);
    const incomingEdges = sortGraphEdges(incomingByNode.get(currentNodeId) ?? []);
    if (incomingEdges.length !== 1) {
      return "";
    }

    const incomingEdge = incomingEdges[0];
    const upstreamNode = nodeById.get(incomingEdge.from);
    if (!upstreamNode) {
      return "";
    }

    const upstreamOutgoing = sortGraphEdges(outgoingByNode.get(upstreamNode.id) ?? []);
    const isSplitter = upstreamNode.type === EffectGuids.kSplitter || upstreamOutgoing.length > 1;
    if (isSplitter) {
      const splitterName = getFriendlyGraphNodeName(upstreamNode, upstreamNode.type, upstreamNode.id);
      return ` (${splitterName} ch ${incomingEdge.fromPort + 1})`;
    }

    currentNodeId = upstreamNode.id;
  }

  return "";
}

function getSignalDiagnosticsNodeLabel(node: SignalLevelNodeMetrics): string {
  const { graph, presetName } = getDiagnosticsGraphContext(node);
  const graphNode = graph?.nodes.find((entry) => entry.id === node.nodeId);
  const nodeName = getFriendlyGraphNodeName(graphNode, node.nodeType, node.nodeId);
  const branchSuffix = getSplitterBranchSuffix(graph, node.nodeId);
  const presetPrefix = node.scope === "preset" && presetName ? `${presetName} · ` : "";
  return `${presetPrefix}${nodeName}${branchSuffix}`;
}

function getSignalDiagnosticsPerformanceKey(node: SignalLevelNodeMetrics): string {
  if (node.scope === "preset") {
    return `${node.presetId ?? uiState.activePresetId ?? ""}::${node.nodeId}`;
  }
  return `${node.scope}::${node.nodeId}`;
}

function getSignalDiagnosticsNodeTimeUs(node: SignalLevelNodeMetrics): number | null {
  const performance = uiState.dspPerformance;
  if (!performance) {
    return null;
  }

  const scopedKey = getSignalDiagnosticsPerformanceKey(node);
  const scopedTime = performance.scopedNodeProcessingTimesUs?.[scopedKey];
  if (typeof scopedTime === "number" && isFinite(scopedTime)) {
    return scopedTime;
  }

  const legacyTime = performance.nodeProcessingTimesUs?.[node.nodeId];
  return typeof legacyTime === "number" && isFinite(legacyTime) ? legacyTime : null;
}

function getSignalDiagnosticsNodeLatencySamples(node: SignalLevelNodeMetrics): number | null {
  const performance = uiState.dspPerformance;
  if (!performance) {
    return null;
  }

  const scopedKey = getSignalDiagnosticsPerformanceKey(node);
  const scopedLatency = performance.scopedNodeLatencySamples?.[scopedKey];
  if (typeof scopedLatency === "number" && isFinite(scopedLatency)) {
    return scopedLatency;
  }

  const legacyLatency = performance.nodeLatencySamples?.[node.nodeId];
  return typeof legacyLatency === "number" && isFinite(legacyLatency) ? legacyLatency : null;
}

function getSignalDiagnosticsNodeProcessingShare(timeUs: number | null | undefined): number | null {
  if (typeof timeUs !== "number" || !isFinite(timeUs)) {
    return null;
  }

  const totalUs = uiState.dspPerformance?.totalProcessingTimeUs ?? 0;
  if (!(totalUs > 0)) {
    return null;
  }

  return (timeUs / totalUs) * 100.0;
}

function formatTimeUs(value: number | null | undefined): string {
  if (typeof value !== "number" || !isFinite(value)) {
    return "—";
  }
  return `${value.toFixed(1)} μs`;
}

function formatTimeShare(value: number | null | undefined): string {
  const timeText = formatTimeUs(value);
  if (timeText === "—") {
    return timeText;
  }

  const share = getSignalDiagnosticsNodeProcessingShare(value);
  if (share == null || !isFinite(share)) {
    return timeText;
  }

  return `${timeText} (${share.toFixed(1)}%)`;
}

function formatNodeLatencySamples(value: number | null | undefined, sampleRate?: number): string {
  if (typeof value !== "number" || !isFinite(value) || value <= 0) {
    return "—";
  }

  if (sampleRate && sampleRate > 0) {
    const latencyMs = (value / sampleRate) * 1000.0;
    return `${value} smp (${latencyMs.toFixed(2)} ms)`;
  }

  return `${value} smp`;
}

function formatLatencyValue(stats: import("./types.js").DSPPerformanceStats | null | undefined): string {
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

/** Returns a map of nodeId → topological position (0-based) for ordering. */
function getGraphTopologicalOrder(graph: SignalGraph | undefined): Map<string, number> {
  const order = new Map<string, number>();
  if (!graph || !graph.nodes.length) return order;

  const indegree = new Map<string, number>();
  const outgoingByNode = new Map<string, string[]>();

  for (const node of graph.nodes) {
    indegree.set(node.id, 0);
  }
  for (const edge of graph.edges) {
    indegree.set(edge.to, (indegree.get(edge.to) ?? 0) + 1);
    const out = outgoingByNode.get(edge.from);
    if (out) out.push(edge.to);
    else outgoingByNode.set(edge.from, [edge.to]);
  }

  const queue = graph.nodes.map((n) => n.id).filter((id) => (indegree.get(id) ?? 0) === 0);
  let idx = 0;
  while (queue.length) {
    const nodeId = queue.shift()!;
    order.set(nodeId, idx++);
    for (const to of outgoingByNode.get(nodeId) ?? []) {
      const remaining = (indegree.get(to) ?? 0) - 1;
      indegree.set(to, remaining);
      if (remaining === 0) queue.push(to);
    }
  }
  return order;
}

function computeGraphCriticalPathNodeIds(graph: SignalGraph | undefined, scopedPrefix: string): Set<string> {
  if (!graph || !graph.nodes.length) {
    return new Set<string>();
  }

  const scopedLatencies = uiState.dspPerformance?.scopedNodeLatencySamples ?? {};
  const incomingByNode = new Map<string, GraphEdge[]>();
  const outgoingByNode = new Map<string, GraphEdge[]>();
  const indegree = new Map<string, number>();

  for (const node of graph.nodes) {
    indegree.set(node.id, 0);
  }

  for (const edge of graph.edges) {
    const incoming = incomingByNode.get(edge.to);
    if (incoming) {
      incoming.push(edge);
    } else {
      incomingByNode.set(edge.to, [edge]);
    }

    const outgoing = outgoingByNode.get(edge.from);
    if (outgoing) {
      outgoing.push(edge);
    } else {
      outgoingByNode.set(edge.from, [edge]);
    }

    indegree.set(edge.to, (indegree.get(edge.to) ?? 0) + 1);
  }

  const queue = graph.nodes
    .map((node) => node.id)
    .filter((nodeId) => (indegree.get(nodeId) ?? 0) === 0);
  const topoOrder: string[] = [];

  while (queue.length) {
    const nodeId = queue.shift() ?? "";
    if (!nodeId) {
      continue;
    }

    topoOrder.push(nodeId);
    for (const edge of outgoingByNode.get(nodeId) ?? []) {
      const remaining = (indegree.get(edge.to) ?? 0) - 1;
      indegree.set(edge.to, remaining);
      if (remaining === 0) {
        queue.push(edge.to);
      }
    }
  }

  const score = new Map<string, number>();
  const previous = new Map<string, string | null>();

  for (const nodeId of topoOrder) {
    let bestIncoming = 0;
    let bestPrevious: string | null = null;
    for (const edge of incomingByNode.get(nodeId) ?? []) {
      const candidate = score.get(edge.from) ?? 0;
      if (candidate >= bestIncoming) {
        bestIncoming = candidate;
        bestPrevious = edge.from;
      }
    }

    const ownLatency = scopedLatencies[`${scopedPrefix}${nodeId}`] ?? 0;
    score.set(nodeId, bestIncoming + ownLatency);
    previous.set(nodeId, bestPrevious);
  }

  let cursor = graph.nodes.find((node) => node.id === "__output__")?.id ?? null;
  if (!cursor) {
    let bestNode: string | null = null;
    let bestScore = -1;
    for (const nodeId of topoOrder) {
      const candidate = score.get(nodeId) ?? 0;
      if (candidate >= bestScore) {
        bestScore = candidate;
        bestNode = nodeId;
      }
    }
    cursor = bestNode;
  }

  const path = new Set<string>();
  while (cursor) {
    path.add(cursor);
    cursor = previous.get(cursor) ?? null;
  }

  return path;
}

function getOverallCriticalPathMembership(diagnostics: import("./types.js").SignalLevelDiagnostics | null | undefined): Set<string> {
  const membership = new Set<string>();
  if (!diagnostics?.nodes?.length) {
    return membership;
  }

  for (const nodeId of computeGraphCriticalPathNodeIds(uiState.globalSignalChain?.preChainGraph, "pre::")) {
    membership.add(`pre::${nodeId}`);
  }

  for (const nodeId of computeGraphCriticalPathNodeIds(uiState.globalSignalChain?.postChainGraph, "post::")) {
    membership.add(`post::${nodeId}`);
  }

  const presetIds = Array.from(new Set(
    diagnostics.nodes
      .filter((node) => node.scope === "preset" && typeof node.presetId === "string" && node.presetId)
      .map((node) => node.presetId as string)
  ));

  let winningPresetId: string | null = null;
  let winningPresetNodes = new Set<string>();
  let winningPresetLatency = -1;
  const scopedLatencies = uiState.dspPerformance?.scopedNodeLatencySamples ?? {};

  for (const presetId of presetIds) {
    const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((entry) => entry.id === presetId);
    const criticalNodes = computeGraphCriticalPathNodeIds(preset?.graph, `${presetId}::`);
    let totalLatency = 0;
    for (const nodeId of criticalNodes) {
      totalLatency += scopedLatencies[`${presetId}::${nodeId}`] ?? 0;
    }
    if (totalLatency >= winningPresetLatency) {
      winningPresetLatency = totalLatency;
      winningPresetId = presetId;
      winningPresetNodes = criticalNodes;
    }
  }

  if (winningPresetId) {
    for (const nodeId of winningPresetNodes) {
      membership.add(`${winningPresetId}::${nodeId}`);
    }
  }

  return membership;
}

function isNodeOnCriticalPath(node: SignalLevelNodeMetrics, criticalPathMembership: Set<string>): boolean {
  return criticalPathMembership.has(getSignalDiagnosticsPerformanceKey(node));
}

// Threshold (dBFS) for each segment, top → bottom in the DOM (rendered bottom-up via flex column-reverse).
// Matches the data-db attributes on .vu-seg elements in index.html.
const VU_SEGMENT_THRESHOLDS = [-3, -6, -9, -12, -18, -24, -36, -48] as const;

// Cached DOM references for the VU meter (populated on first call).
let vuSegments: HTMLElement[] | null = null;
let vuPeakHold: HTMLElement | null = null;
let vuDbValue: HTMLElement | null = null;
let vuPeakHoldDbfs = -Infinity;
let vuPeakHoldTimer: ReturnType<typeof setTimeout> | null = null;

function updateInputVuMeter(levels: import("./types.js").SignalLevelMetrics | null): void {
  if (!vuSegments) {
    const container = document.getElementById("vu-segments");
    vuSegments = container
      ? Array.from(container.querySelectorAll<HTMLElement>(".vu-seg"))
      : [];
    vuPeakHold = document.getElementById("vu-peak-hold");
    vuDbValue = document.getElementById("vu-db-value");
  }

  if (!vuSegments.length) return;

  if (!levels || !isFinite(levels.peakDbfs)) {
    vuSegments.forEach((s) => s.classList.remove("active"));
    //if (vuDbValue) vuDbValue.textContent = "—";
    if (vuPeakHold) vuPeakHold.classList.remove("visible");
    return;
  }

  const dbfs = levels.peakDbfs;

  // Light all segments whose threshold is ≤ current peak
  vuSegments.forEach((seg, i) => {
    const threshold = VU_SEGMENT_THRESHOLDS[i];
    seg.classList.toggle("active", dbfs >= threshold);
  });

  // dB readout
  /*if (vuDbValue) {
    vuDbValue.textContent = `${dbfs.toFixed(1)}`;
  }*/

  // Peak-hold tick: update if new peak is higher
  if (dbfs >= vuPeakHoldDbfs) {
    vuPeakHoldDbfs = dbfs;

    if (vuPeakHoldTimer !== null) clearTimeout(vuPeakHoldTimer);
    vuPeakHoldTimer = setTimeout(() => {
      vuPeakHoldDbfs = -Infinity;
      if (vuPeakHold) vuPeakHold.classList.remove("visible");
    }, 2000);

    // Position peak-hold tick at the top of the topmost lit segment.
    // With top-to-bottom DOM order (red at top), firstLitIdx is the loudest lit segment.
    const segHeight = 7; // px per segment (5px height + 2px gap)
    const firstLitIdx = vuSegments.findIndex((s) => s.classList.contains("active"));
    if (firstLitIdx >= 0 && vuPeakHold) {
      vuPeakHold.style.top = `${firstLitIdx * segHeight}px`;
      vuPeakHold.classList.add("visible");
    }
  }
}

export function updateSignalDiagnosticsView(): void {
  const diagnostics = uiState.signalDiagnostics;

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
    if (listEl) listEl.innerHTML = "<div class=\"performance-detail-item\">Diagnostics disabled.</div>";
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

  updateSignalPeakHold(diagnostics);
  const hold = uiState.signalPeakHold;

  if (inputPeak) inputPeak.textContent = `${formatDb(hold?.input.peakDbfs ?? input.peakDbfs)} dBFS`;
  if (inputRms) inputRms.textContent = `${formatDb(input.rmsDbfs)} dBFS`;
  if (inputHeadroom) inputHeadroom.textContent = `${formatDb(input.headroomDb)} dB`;
  if (inputClip) {
    inputClip.classList.toggle("clip-on", input.clipped);
    inputClip.classList.toggle("clip-off", !input.clipped);
    setClipStatusText(inputClip, input.clipped ? `Clipping (${input.clipCount})` : "OK");
  }

  if (outputPeak) outputPeak.textContent = `${formatDb(hold?.output.peakDbfs ?? output.peakDbfs)} dBFS`;
  if (outputRms) outputRms.textContent = `${formatDb(output.rmsDbfs)} dBFS`;
  if (outputHeadroom) outputHeadroom.textContent = `${formatDb(output.headroomDb)} dB`;
  if (outputClip) {
    outputClip.classList.toggle("clip-on", output.clipped);
    outputClip.classList.toggle("clip-off", !output.clipped);
    setClipStatusText(outputClip, output.clipped ? `Clipping (${output.clipCount})` : "OK");
  }

  // Update the designed peak input control
  const designedPeakCurrentEl = document.getElementById("designed-peak-current-value");
  const designedPeakStoredEl = document.getElementById("designed-peak-stored-value");
  const applyDesignedPeakBtn = document.getElementById("apply-designed-peak-btn") as HTMLButtonElement | null;
  if (!enabled || !diagnostics) {
    if (designedPeakCurrentEl) designedPeakCurrentEl.textContent = "\u2014";
    if (applyDesignedPeakBtn) applyDesignedPeakBtn.disabled = true;
  } else {
    const rawPeakDbfs = hold?.rawInput.peakDbfs ?? diagnostics.rawInput?.peakDbfs ?? input.peakDbfs;
    if (designedPeakCurrentEl) designedPeakCurrentEl.textContent = `${formatDb(rawPeakDbfs)} dBFS`;
    if (applyDesignedPeakBtn) applyDesignedPeakBtn.disabled = false;
    if (applyDesignedPeakBtn) (applyDesignedPeakBtn as HTMLButtonElement & { _peakDbfs?: number })._peakDbfs = rawPeakDbfs;
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
    const criticalPathMembership = getOverallCriticalPathMembership(diagnostics);

    const makeNodeRow = (
      scope: string,
      label: string,
      isCriticalPath: boolean,
      timeUs: number | null,
      latencySamples: number | null,
      peakDbfs: number,
      headroomDb: number,
      clipped: boolean,
      clipCount: number
    ): string => {
      const clipClass = clipped ? "clip-on" : "clip-off";
      const clipText = clipped ? `Clipping (${clipCount})` : "OK";
      const criticalBadge = isCriticalPath
        ? '<span class="critical-path-badge" title="Contributes to reported plugin latency">Critical</span>'
        : "";
      return `
        <div class="signal-diagnostics-item">
          <span class="signal-diagnostics-cell scope">${escapeHtml(scope)}</span>
          <span class="signal-diagnostics-cell node">${criticalBadge}${label}</span>
          <span class="signal-diagnostics-cell">${formatTimeShare(timeUs)}</span>
          <span class="signal-diagnostics-cell">${formatNodeLatencySamples(latencySamples, uiState.dspPerformance?.sampleRate)}</span>
          <span class="signal-diagnostics-cell">${formatDb(peakDbfs)} dBFS</span>
          <span class="signal-diagnostics-cell">${formatDb(headroomDb)} dB</span>
          <span class="signal-diagnostics-cell clip ${clipClass}">
            <span class="clip-indicator"></span>
            ${clipText}
          </span>
        </div>
      `;
    };

    const inputRow = makeNodeRow(
      "in",
      "chain input",
      false,
      null,
      null,
      hold?.input.peakDbfs ?? input.peakDbfs,
      input.headroomDb,
      input.clipped,
      input.clipCount
    );

    // Build topological order maps for each graph so nodes render in signal-chain order.
    const preOrder = getGraphTopologicalOrder(uiState.globalSignalChain?.preChainGraph);
    const postOrder = getGraphTopologicalOrder(uiState.globalSignalChain?.postChainGraph);
    const presetOrderCache = new Map<string, Map<string, number>>();
    const getPresetOrder = (presetId: string): Map<string, number> => {
      if (!presetOrderCache.has(presetId)) {
        const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((p) => p.id === presetId);
        presetOrderCache.set(presetId, getGraphTopologicalOrder(preset?.graph));
      }
      return presetOrderCache.get(presetId)!;
    };

    const scopeRank = (scope: string) => scope === "pre" ? 0 : scope === "preset" ? 1 : 2;

    const sortedNodes = (diagnostics.nodes ?? []).slice().sort((a, b) => {
      const scopeDiff = scopeRank(a.scope) - scopeRank(b.scope);
      if (scopeDiff !== 0) return scopeDiff;
      if (a.scope === "pre") {
        return (preOrder.get(a.nodeId) ?? 9999) - (preOrder.get(b.nodeId) ?? 9999);
      }
      if (a.scope === "post") {
        return (postOrder.get(a.nodeId) ?? 9999) - (postOrder.get(b.nodeId) ?? 9999);
      }
      // preset scope — group by preset, then topological order within preset
      const aPreset = a.presetId ?? "";
      const bPreset = b.presetId ?? "";
      if (aPreset !== bPreset) return aPreset.localeCompare(bPreset);
      const presetOrder = getPresetOrder(aPreset);
      return (presetOrder.get(a.nodeId) ?? 9999) - (presetOrder.get(b.nodeId) ?? 9999);
    });

    const nodeRows = sortedNodes
      .map((node) => {
        const nodeLabel = escapeHtml(getSignalDiagnosticsNodeLabel(node));
        const isCriticalPath = isNodeOnCriticalPath(node, criticalPathMembership);
        const nodeTimeUs = getSignalDiagnosticsNodeTimeUs(node);
        const nodeLatencySamples = getSignalDiagnosticsNodeLatencySamples(node);
        const levels = node.levels ?? { peakDbfs: Number.NaN, headroomDb: Number.NaN, clipped: false, clipCount: 0 };
        const holdEntry = hold?.nodes[node.nodeId];
        return makeNodeRow(node.scope, nodeLabel, isCriticalPath, nodeTimeUs, nodeLatencySamples, holdEntry?.peakDbfs ?? levels.peakDbfs, levels.headroomDb, levels.clipped, levels.clipCount);
      })
      .join("");

    const rows = inputRow + nodeRows;

    listEl.innerHTML = `
      <div class=\"signal-diagnostics-header\">
        <span class=\"signal-diagnostics-cell scope\">Scope</span>
        <span class=\"signal-diagnostics-cell node\">Node</span>
        <span class="signal-diagnostics-cell">CPU</span>
        <span class="signal-diagnostics-cell">Latency</span>
        <span class=\"signal-diagnostics-cell\">Peak</span>
        <span class=\"signal-diagnostics-cell\">Headroom</span>
        <span class=\"signal-diagnostics-cell\">Clip</span>
      </div>
      <div class=\"signal-diagnostics-columns\">
        ${rows}
      </div>
    `;
  }

  updateInputVuMeter(enabled ? (diagnostics.rawInput ?? diagnostics.input) : null);
  updateSignalPathClipIndicators();
}
