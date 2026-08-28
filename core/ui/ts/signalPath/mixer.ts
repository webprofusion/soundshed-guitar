import { uiState, clonePreset, setActivePresetDraft, setFocusedMixerPresetId, isCompositeEditMode } from "../state.js";
import type {
  Preset,
} from "../types.js";
import { postMessage, setPresetMix, setPresetPan, setPresetMute, setPresetSolo, setMasterGain, setLimiterEnabled, removeActivePreset, focusMixerPreset } from "../bridge.js";
import { escapeHtml, idAccentColor } from "../utils.js";
import { GenericKnob } from "../controls.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { isMixTabActive, setMixTabActive } from "./state.js";
import { requestSignalPathRender } from "./render.js";
export const mixerPresetTabCollator = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });

export function renderMixerPresetTabs(): void {
  let tabBar = document.getElementById("mixer-preset-tabs");
  const signalPathBar = document.getElementById("signal-path-bar");
  const mixer = uiState.mixer;

  // Show tabs whenever there are 2+ active mixer slots — do NOT require the
  // preset to be in the cache first, because "+Mixer" list items are stubs
  // without graph data and getSignalPathPreset() returns null until the C++
  // round-trip completes.
  const multiPresetMode = !isCompositeEditMode()
    && !!mixer
    && mixer.activePresetIds.length > 1;

  if (!multiPresetMode) {
    if (tabBar) tabBar.remove();
    setMixTabActive(false);
    return;
  }

  if (!tabBar) {
    tabBar = document.createElement("div");
    tabBar.id = "mixer-preset-tabs";
    tabBar.className = "mixer-preset-tabs";
    // Insert as a sibling immediately before ".signal-path-visualizer" — NOT
    // necessarily a direct child of #signal-path-bar. The current layout wraps
    // everything in ".signal-path-body", so insertBefore() must target the
    // actual parent of the reference node or it throws NotFoundError.
    const visualizer = signalPathBar?.querySelector(".signal-path-visualizer");
    if (visualizer?.parentElement) {
      visualizer.parentElement.insertBefore(tabBar, visualizer);
    } else if (signalPathBar) {
      signalPathBar.prepend(tabBar);
    }
  }

  const presetIds = [...mixer.activePresetIds];
  // Determine the focused tab: prefer the explicitly focused slot, then the
  // currently active preset if it's in the mixer, then the first slot.
  const focusedId = (uiState.focusedMixerPresetId && presetIds.includes(uiState.focusedMixerPresetId))
    ? uiState.focusedMixerPresetId
    : (uiState.activePresetId && presetIds.includes(uiState.activePresetId))
      ? uiState.activePresetId
      : presetIds[0];

  const presetTabsHtml = presetIds.map((id) => {
    const name = uiState.presetCache.get(id)?.name ?? mixer?.presets[id]?.name ?? id;
    const ps = mixer?.presets[id];
    const muted = ps?.mute ?? false;
    const soloed = ps?.solo ?? false;
    const active = !isMixTabActive() && id === focusedId;
    const indicators = [
      muted ? `<span class="tab-indicator muted" title="Muted">M</span>` : "",
      soloed ? `<span class="tab-indicator soloed" title="Solo">S</span>` : "",
    ].join("");
    const closeBtn = `<span class="mixer-tab-close" data-close-preset-id="${escapeHtml(id)}" title="Remove from mixer" role="button" aria-label="Remove ${escapeHtml(name)}">×</span>`;
    return `<button class="mixer-preset-tab${active ? " active" : ""}" data-preset-id="${escapeHtml(id)}" type="button">${escapeHtml(name)}${indicators}${closeBtn}</button>`;
  }).join("");

  const mixTabHtml = `<button class="mixer-preset-tab mixer-tab-mix${isMixTabActive() ? " active" : ""}" data-mix-tab="1" type="button">⚖ Mix</button>`;

  tabBar.innerHTML = `<div class="mixer-preset-tab-row">${presetTabsHtml}${mixTabHtml}</div>`;

  tabBar.querySelectorAll<HTMLButtonElement>(".mixer-preset-tab-row .mixer-preset-tab:not([data-mix-tab])").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      // Don't switch tab when close button was clicked
      if ((e.target as HTMLElement).closest(".mixer-tab-close")) return;
      const pid = btn.dataset.presetId ?? "";
      if (pid) {
        setMixTabActive(false);
        uiState.activePresetId = pid;
        setFocusedMixerPresetId(pid);
        focusMixerPreset(pid);
        document.dispatchEvent(new CustomEvent("mixerPresetTabSelected", {
          detail: { presetId: pid },
        }));
        requestSignalPathRender();
      }
    });
  });

  // Close (×) buttons — remove preset from mixer
  tabBar.querySelectorAll<HTMLElement>(".mixer-tab-close").forEach((closeEl) => {
    closeEl.addEventListener("click", (e) => {
      e.stopPropagation();
      const pid = closeEl.dataset.closePresetId ?? "";
      if (!pid) return;
      removeActivePreset(pid);
      if (uiState.mixer) {
        uiState.mixer.activePresetIds = uiState.mixer.activePresetIds.filter((id) => id !== pid);
        delete uiState.mixer.presets[pid];
      }
      if (uiState.focusedMixerPresetId === pid) {
        uiState.focusedMixerPresetId = uiState.mixer?.activePresetIds[0] ?? null;
      }
      // The mixer's membership no longer matches whatever Multi-Rig preset it
      // was loaded from/saved as, if any — a subsequent Save should create a
      // new one rather than silently overwrite the old one.
      uiState.activeCompositePresetId = null;
      // Update any "✓ In Mixer" button in the preset list for this preset
      document.querySelectorAll<HTMLButtonElement>(`.preset-add-to-mixer-btn[data-preset-id="${CSS.escape(pid)}"]`).forEach((btn) => {
        btn.textContent = "+ Mixer";
        btn.classList.remove("in-mixer");
        btn.title = "Add to mixer";
      });
      requestSignalPathRender();
    });
  });

  tabBar.querySelector<HTMLButtonElement>(".mixer-preset-tab-row [data-mix-tab]")?.addEventListener("click", () => {
    setMixTabActive(!isMixTabActive());
    requestSignalPathRender();
  });
}

export function getEditableSignalPathPreset(sourcePreset: Preset): Preset {
  const existingDraft = uiState.activePresetDraft;
  if (existingDraft && existingDraft.id === sourcePreset.id) {
    normalizePresetScenes(existingDraft, uiState.activePresetSceneId ?? undefined);
    return existingDraft;
  }

  const draft = clonePreset(sourcePreset);
  uiState.activePresetId = sourcePreset.id;
  setFocusedMixerPresetId(sourcePreset.id);
  focusMixerPreset(sourcePreset.id);
  uiState.activePresetSceneId = normalizePresetScenes(draft, uiState.activePresetSceneId ?? undefined);
  setActivePresetDraft(draft);
  return uiState.activePresetDraft ?? draft;
}

export function pushScenePresetToBackend(preset: Preset): void {
  const sceneId = normalizePresetScenes(preset, uiState.activePresetSceneId ?? undefined);
  uiState.activePresetSceneId = sceneId;
  uiState.activePresetId = preset.id;
  setFocusedMixerPresetId(preset.id);
  setActivePresetDraft(preset);
  postMessage({
    type: "loadPreset",
    preset: uiState.activePresetDraft ?? preset,
    ...(sceneId ? { sceneId } : {}),
  });
}

export /** Matches the L/C/R pan convention used by node-param knobs elsewhere in the app. */
function formatMixerPanValue(value: number): string {
  if (Math.abs(value) < 0.01) return "C";
  return value < 0 ? `L${Math.abs(value * 100).toFixed(0)}` : `R${(value * 100).toFixed(0)}`;
}

export function formatMixerPercentValue(value: number): string {
  return `${Math.round(value * 100)}%`;
}

export function buildInlineMixerHtml(): string {
  const mixer = uiState.mixer;
  if (!mixer || !mixer.activePresetIds.length) return "";

  const presetIds = [...mixer.activePresetIds].sort((leftId, rightId) => {
    const leftName = uiState.presetCache.get(leftId)?.name ?? mixer.presets[leftId]?.name ?? leftId;
    const rightName = uiState.presetCache.get(rightId)?.name ?? mixer.presets[rightId]?.name ?? rightId;
    const nameComparison = mixerPresetTabCollator.compare(leftName, rightName);
    if (nameComparison !== 0) {
      return nameComparison;
    }
    return mixerPresetTabCollator.compare(leftId, rightId);
  });

  const strips = presetIds.map((id) => {
    const name = uiState.presetCache.get(id)?.name ?? mixer.presets[id]?.name ?? id;
    const ps = mixer.presets[id] ?? { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
    return `
      <div class="iml-strip" data-preset-id="${escapeHtml(id)}" style="--accent:${idAccentColor(id)}">
        <div class="iml-strip-name" title="${escapeHtml(name)}">${escapeHtml(name)}</div>
        <div class="iml-strip-row">
          <div class="iml-knobs">
            <div class="knob-control iml-knob">
              <span class="knob-label">Mix</span>
              <div class="knob iml-mix-knob" data-value="${ps.mix}"><div class="knob-indicator"></div></div>
              <span class="knob-value">${formatMixerPercentValue(ps.mix)}</span>
            </div>
            <div class="knob-control iml-knob">
              <span class="knob-label">Pan</span>
              <div class="knob iml-pan-knob" data-value="${ps.pan}"><div class="knob-indicator"></div></div>
              <span class="knob-value">${formatMixerPanValue(ps.pan)}</span>
            </div>
          </div>
          <div class="iml-toggles">
            <div class="toggle-control mini-toggle-control iml-mute-toggle">
              <span class="toggle-label">Mute</span>
              <label class="toggle-switch"><input type="checkbox" class="iml-mute"${ps.mute ? " checked" : ""}/><span class="toggle-slider"></span></label>
            </div>
            <div class="toggle-control mini-toggle-control iml-solo-toggle">
              <span class="toggle-label">Solo</span>
              <label class="toggle-switch"><input type="checkbox" class="iml-solo"${ps.solo ? " checked" : ""}/><span class="toggle-slider"></span></label>
            </div>
          </div>
        </div>
      </div>`;
  }).join("");

  return `
    <div class="iml-strips">${strips}</div>
    <div class="iml-master">
      <div class="iml-strip-name">Master</div>
      <div class="iml-strip-row">
        <div class="iml-knobs">
          <div class="knob-control iml-knob">
            <span class="knob-label">Gain</span>
            <div class="knob" id="iml-master-gain-knob" data-value="${mixer.masterGain}"><div class="knob-indicator"></div></div>
            <span class="knob-value">${formatMixerPercentValue(mixer.masterGain)}</span>
          </div>
        </div>
        <div class="iml-toggles">
          <div class="toggle-control mini-toggle-control">
            <span class="toggle-label">Limiter</span>
            <label class="toggle-switch"><input type="checkbox" id="iml-limiter"${mixer.limiterEnabled ? " checked" : ""}/><span class="toggle-slider"></span></label>
          </div>
          <div class="iml-toolbar">
            <button type="button" id="iml-save-multi-rig" class="btn btn-secondary btn-sm iml-toolbar-btn" title="Save current mixer as a Multi-Rig preset">Save</button>
            <button type="button" id="iml-delete-multi-rig" class="btn btn-secondary btn-sm iml-toolbar-btn"${uiState.activeCompositePresetId ? "" : " disabled"} title="Delete this Multi-Rig preset">Delete</button>
          </div>
        </div>
      </div>
    </div>`;
}

export function renderInlineMixer(): void {
  const signalPathBar = document.getElementById("signal-path-bar");
  const resizeHandle = document.getElementById("signal-path-resize-handle");
  let panel = document.getElementById("inline-mixer-panel");
  if (!panel) {
    panel = document.createElement("div");
    panel.id = "inline-mixer-panel";
    panel.className = "inline-mixer-panel";
    // Place mixer where the scroll area sits (above the resize handle).
    // resizeHandle lives inside ".signal-path-body", not directly inside
    // #signal-path-bar, so insertBefore() must target its real parent.
    if (resizeHandle?.parentElement) {
      resizeHandle.parentElement.insertBefore(panel, resizeHandle);
    } else {
      signalPathBar?.appendChild(panel);
    }
  }
  panel.innerHTML = buildInlineMixerHtml();
  bindInlineMixerControls(panel);
}

export function removeInlineMixer(): void {
  document.getElementById("inline-mixer-panel")?.remove();
}

export /** Drag sensitivity matching the convention used for node-param knobs: full range over ~200px. */
function knobSensitivity(minValue: number, maxValue: number): number {
  return (maxValue - minValue) / 200;
}

export function bindInlineMixerControls(panel: HTMLElement): void {
  panel.querySelectorAll<HTMLElement>(".iml-strip").forEach((strip) => {
    const pid = strip.dataset.presetId ?? "";
    if (!pid) return;

    const mixKnob = strip.querySelector<HTMLElement>(".iml-mix-knob");
    if (mixKnob) {
      new GenericKnob({
        knobElement: mixKnob,
        paramId: `mixer_${pid}_mix`,
        minValue: 0,
        maxValue: 1,
        defaultValue: 1,
        displayFormat: formatMixerPercentValue,
        valueDisplay: mixKnob.parentElement?.querySelector<HTMLElement>(".knob-value"),
        sensitivity: knobSensitivity(0, 1),
        sendParameter: false,
        onValueChange: (v) => setPresetMix(pid, v),
      });
    }

    const panKnob = strip.querySelector<HTMLElement>(".iml-pan-knob");
    if (panKnob) {
      new GenericKnob({
        knobElement: panKnob,
        paramId: `mixer_${pid}_pan`,
        minValue: -1,
        maxValue: 1,
        defaultValue: 0,
        displayFormat: formatMixerPanValue,
        valueDisplay: panKnob.parentElement?.querySelector<HTMLElement>(".knob-value"),
        sensitivity: knobSensitivity(-1, 1),
        sendParameter: false,
        onValueChange: (v) => setPresetPan(pid, v),
      });
    }

    const muteToggle = strip.querySelector<HTMLInputElement>(".iml-mute");
    muteToggle?.addEventListener("change", () => {
      const nowMuted = muteToggle.checked;
      setPresetMute(pid, nowMuted);
      if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].mute = nowMuted;
      renderMixerPresetTabs(); // refresh M/S indicators in tabs
    });

    const soloToggle = strip.querySelector<HTMLInputElement>(".iml-solo");
    soloToggle?.addEventListener("change", () => {
      const nowSolo = soloToggle.checked;
      setPresetSolo(pid, nowSolo);
      if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].solo = nowSolo;
      renderMixerPresetTabs();
    });
  });

  const masterGainKnob = panel.querySelector<HTMLElement>("#iml-master-gain-knob");
  if (masterGainKnob) {
    new GenericKnob({
      knobElement: masterGainKnob,
      paramId: "mixer_master_gain",
      minValue: 0,
      maxValue: 2,
      defaultValue: 1,
      displayFormat: formatMixerPercentValue,
      valueDisplay: masterGainKnob.parentElement?.querySelector<HTMLElement>(".knob-value"),
      sensitivity: knobSensitivity(0, 2),
      sendParameter: false,
      onValueChange: (v) => setMasterGain(v),
    });
  }

  panel.querySelector<HTMLInputElement>("#iml-limiter")?.addEventListener("change", (e) => {
    setLimiterEnabled((e.target as HTMLInputElement).checked);
  });

  panel.querySelector<HTMLButtonElement>("#iml-save-multi-rig")?.addEventListener("click", () => {
    document.dispatchEvent(new CustomEvent("mixerSaveMultiRig"));
  });

  panel.querySelector<HTMLButtonElement>("#iml-delete-multi-rig")?.addEventListener("click", () => {
    document.dispatchEvent(new CustomEvent("mixerDeleteMultiRig"));
  });
}
