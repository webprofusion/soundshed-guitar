/**
 * The dispatcher: given the selected node, decide which controls it gets, build
 * them, and bind them.
 */

import { EffectGuids } from "../../effectGuids.js";
import { GRAPHIC_EQ_FREQUENCIES, graphicEqFrequencyBounds } from "../../eqCurve.js";
import { isNodeBypassed } from "../../graphNodes.js";
import { renderIcon } from "../../iconAssets.js";
import { resolveLayoutForNode } from "../../layoutPreferences.js";
import { formatParamValue, renderCustomLayout, renderCustomLayoutBackdrop } from "../../layoutRenderer.js";
import { effectiveTaper } from "../../paramTaper.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../../presetV2.js";
import { blendKnobDataAttributes, type BlendParamDef } from "../../blendUtils.js";
import { bindBlendEditorControls, computeBlendParamRange, denormalizeBlendValue, getBlendKnobBinding, getBlendState, normalizeBlendValue } from "../../signalPathBlend.js";
import type { BlendParamRange } from "../../signalPathBlend.js";
import { uiState } from "../../state.js";
import type { GraphNode, Preset } from "../../types.js";
import { clampValue, escapeHtml } from "../../utils.js";
import { shouldShowFullRigCabModelNote } from "../chainRules.js";
import { bindEffectPresetsButton } from "../effectPresets.js";
import { bindLayoutSwitchButton, renderLayoutSwitchButtonHtml } from "../layoutSwitch.js";
import { buildCustomEffectActions, buildNodeLayoutMatchText, getNodeArchitectureBadge, getNodeDisplayName, getNodeNamCalibrationMetadataChip } from "../nodeLabels.js";
import { getNodeCategory, isNeuralModelNode } from "../nodeTypes.js";
import { bindEquipmentImageFallback, bindResourceControls } from "../resourceControls.js";
import { analyzerSpectrogramHistoryByNode, getSelectedNodeDspStatusNodeId, nodeParamsPanelElement, setLastSelectedNode, setSelectedNodeDspStatusNodeId } from "../state.js";
import { bindSelectedNodeDspStatusToggle, isDspStatusVisible, resetDspStatusAverages, updateSelectedNodeAnalyzerPanel, updateSelectedNodeDspStatus, updateSelectedNodePeakMeter } from "../telemetry.js";
import { getEffectVisualizationEquipmentImage, getEffectVisualizationStockImage, updateEffectVisualization } from "../visualization.js";
import { bindCabResponseControls, buildCabResponseSectionHtml } from "./cabResponse.js";
import { bindGraphicEqControls } from "./eq.js";
import { bindHostedPluginActionControls, bindHostedPluginListControls } from "./hostedPlugins.js";
import { applyCustomLayoutScaling, bindLayoutOverlayBypassToggles } from "./layoutOverlay.js";
import { buildMixerInputControlsHtml } from "./mixerInput.js";
import { bindBlendModeOverride, bindBypassButton, bindCustomEffectActionControls } from "./nodeActions.js";
import { bindNodeParamControls, bindParamTabs, formatParamLabel, isToggleParam } from "./paramControls.js";
import { isPitchShiftType, semitoneKnobRange } from "./pitchShiftRange.js";
import { buildNodeResourceSelector, preloadResourceNavigationCaches } from "./resourceSelector.js";
import { setNodeParamsPanelRenderer } from "../render.js";
import { nodeParamKnobs, paramsPanelInteractions } from "./state.js";
import { refreshNodeVisualizations } from "./visualizations.js";

export function showNodeParamsPanel(node: GraphNode, preset: Preset): void {
  if (!nodeParamsPanelElement) {
    return;
  }

  // Tear down any existing interactive EQ curve before replacing panel content
  if (paramsPanelInteractions.eq) {
    paramsPanelInteractions.eq.destroy();
    paramsPanelInteractions.eq = null;
  }
  paramsPanelInteractions.eqSpectrum?.destroy();
  paramsPanelInteractions.eqSpectrum = null;
  if (paramsPanelInteractions.spatial) {
    paramsPanelInteractions.spatial.destroy();
    paramsPanelInteractions.spatial = null;
    paramsPanelInteractions.spatialNodeId = null;
  }
  nodeParamKnobs.clear();

  // Ensure node.params exists
  if (!node.params) {
    node.params = {};
  }
  if (getSelectedNodeDspStatusNodeId() !== node.id) {
    setSelectedNodeDspStatusNodeId(node.id);
    resetDspStatusAverages();
  }

  nodeParamsPanelElement.classList.add("visible");
  setLastSelectedNode(node.type || null, getNodeCategory(node) || null);
  updateEffectVisualization(node);
  
  // Get parameter definitions from registry
  const typeInfo = getNodeEffectInfo(node);
  let paramDefs: BlendParamDef[] = typeInfo?.parameters || [];
  if (EffectTypeRegistry.resolve(node.type) === EffectGuids.kEqGraphic) {
    paramDefs = [];
  }

  const blendState = getBlendState(node);
  const blendParamRanges = new Map<string, BlendParamRange>();
  if (blendState) {
    blendState.paramIds.forEach((paramId) => {
      const currentValue = node.params[paramId];
      const range = computeBlendParamRange(paramId, blendState.mappings, currentValue);
      blendParamRanges.set(paramId, range);
    });

    const blendParamDefs = blendState.paramIds.map((paramId) => {
      const range = blendParamRanges.get(paramId);
      return {
        key: paramId,
        name: range?.spec?.label ?? formatParamLabel(paramId),
        default: range?.defaultValue ?? 0,
        min: range?.min ?? -1,
        max: range?.max ?? 1,
        unit: "amount",
        step: 0.1,
        blend: getBlendKnobBinding(paramId, blendState),
      };
    });

    // With no model captured at any setting there are no mapped knobs, and the Blend sweep
    // is what picks the model, so it stays.
    const nonBlendParams = blendParamDefs.length
      ? paramDefs.filter((paramDef) => paramDef.key !== "blend")
      : paramDefs;
    paramDefs = [...blendParamDefs, ...nonBlendParams];
  }
  
  const isPitchShift = isPitchShiftType(node.type);

  const renderParamControl = (paramDef: BlendParamDef): string => {
    const key = paramDef.key;
    const rawValue = node.params[key];
    const label = paramDef.name || formatParamLabel(key);
    const isBlendParam = blendParamRanges.has(key);
    const blendRange = blendParamRanges.get(key);
    const pitchRange = isPitchShift && key === "semitones" ? semitoneKnobRange(node.params) : null;
    const min = blendRange?.min ?? pitchRange?.min ?? paramDef.min ?? 0;
    const max = blendRange?.max ?? pitchRange?.max ?? paramDef.max ?? 1;
    const unit = paramDef.unit || "amount";
    // The engine holds a pitch shift inside its range, so the knob shows the held value.
    const inPitchRange = (v: number): number => (pitchRange ? clampValue(v, pitchRange.min, pitchRange.max) : v);
    const defaultValue = inPitchRange(blendRange?.defaultValue ?? paramDef.default ?? 0);
    const normalizedValue = typeof rawValue === "number"
      ? rawValue
      : (isBlendParam ? normalizeBlendValue(defaultValue, blendRange?.spec ?? null) : defaultValue);
    const displayValue = inPitchRange(isBlendParam
      ? denormalizeBlendValue(normalizedValue, blendRange?.spec ?? null)
      : (typeof normalizedValue === "number" ? normalizedValue : defaultValue));
    const value = typeof rawValue === "number" ? rawValue : (isBlendParam ? normalizedValue : defaultValue);
    const isToggle = isToggleParam(paramDef);
    const step = pitchRange ? pitchRange.step : (typeof paramDef.step === "number" ? paramDef.step : undefined);
    const enumLabels = Array.isArray(paramDef.labels) ? paramDef.labels : [];
    const isEnum = unit === "enum" && enumLabels.length > 0;
    // A blend knob shows its blend spec's scale, and a pitch shift's the node's own range;
    // neither is the range the taper was declared over.
    const taper = isBlendParam || pitchRange ? "linear" : effectiveTaper(paramDef.taper, min, max);

    if (isToggle) {
      const checked = value >= 0.5;
      return `
        <div class="node-param-group">
          <span class="node-param-label">${label}</span>
          <label class="toggle-switch">
            <input class="node-param-toggle" type="checkbox" data-node-id="${node.id}" data-param-key="${key}" ${checked ? "checked" : ""}>
            <span class="toggle-slider"></span>
          </label>
          <span class="node-param-value">${checked ? "On" : "Off"}</span>
        </div>
      `;
    }

    if (unit === "blend") {
      const blendLabel = value <= 0.01 ? "A" : value >= 0.99 ? "B" : `${Math.round(value * 100)}%`;
      return `
        <div class="node-param-group node-param-blend-group">
          <span class="node-param-label">${label}</span>
          <div class="blend-slider-container">
            <span class="blend-endpoint-label">A</span>
            <input
              type="range"
              class="node-param-blend-slider"
              data-node-id="${node.id}"
              data-param-key="${key}"
              min="${min}"
              max="${max}"
              step="0.01"
              value="${value}"
              data-default="${defaultValue}"
            >
            <span class="blend-endpoint-label">B</span>
          </div>
          <span class="node-param-value">${blendLabel}</span>
        </div>
      `;
    }

    return `
      <div class="node-param-group">
        <span class="node-param-label">${label}</span>
        <div 
          class="knob node-param-knob" 
          data-node-id="${node.id}" 
          data-param-key="${key}"
          data-value="${displayValue}"
          data-default="${defaultValue}"
          data-min="${min}"
          data-max="${max}"
          data-unit="${unit}"
          ${step !== undefined ? `data-step="${step}"` : ""}
          ${isEnum ? `data-labels="${enumLabels.join("|")}"` : ""}
          ${taper === "log" ? `data-taper="log"` : ""}
          ${paramDef.blend ? blendKnobDataAttributes(paramDef.blend) : ""}
        >
          ${isBlendParam ? `<div class="knob-mapped-points"></div>` : ""}
          <div class="knob-indicator"></div>
        </div>
        <span class="node-param-value">${formatParamValue(displayValue, unit, enumLabels, taper)}</span>
       
      </div>
    `;
  };

  const buildParamControls = (defs: BlendParamDef[]): string => {
    const hasGroups = defs.some((paramDef) => typeof paramDef.group === "string" && paramDef.group.trim().length > 0);
    if (!hasGroups) {
      return defs.map(renderParamControl).join("");
    }

    const groupOrder: string[] = [];
    const groupMap = new Map<string, string[]>();

    defs.forEach((paramDef) => {
      const group = paramDef.group?.trim() || "Other";
      if (!groupMap.has(group)) {
        groupMap.set(group, []);
        groupOrder.push(group);
      }
      groupMap.get(group)?.push(renderParamControl(paramDef));
    });

    return groupOrder.map((group) => `
      <div class="node-param-group-block">
        <div class="node-param-group-title">${group}</div>
        <div class="node-param-group-items">
          ${(groupMap.get(group) || []).join("")}
        </div>
      </div>
    `).join("");
  };

  let advancedParamDefs = paramDefs.filter((paramDef) => Boolean(paramDef.advanced));
  let mainParamDefs = paramDefs.filter((paramDef) => !paramDef.advanced);
  if (mainParamDefs.length === 0) {
    mainParamDefs = paramDefs;
    advancedParamDefs = [];
  }
  const hasAdvancedTab = advancedParamDefs.length > 0;

  const isEqNode = typeInfo?.category === "eq" || node.type.startsWith("eq_");
  const isGraphicEqNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kEqGraphic;
  const customEffectActions = buildCustomEffectActions(node);
  const eqVisualizer = isEqNode && !isGraphicEqNode ? `
    <div class="eq-visualizer" data-node-id="${node.id}">
      <div class="eq-visualizer-header">
        <span>EQ Curve</span>
        <span class="eq-visualizer-range">±18 dB</span>
      </div>
      <canvas class="eq-curve-canvas" data-node-id="${node.id}"></canvas>
    </div>
  ` : "";
  const isSpatialNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kSpatial3D;
  const spatialSpeakerMode = (node.params?.listenMode ?? 0) >= 0.5;
  const spatialVisualizer = isSpatialNode ? `
    <div class="spatial-visualizer" data-node-id="${node.id}">
      <div class="spatial-visualizer-header">
        <span>Source Position</span>
        <span class="spatial-visualizer-hint">${spatialSpeakerMode
          ? "Speaker mode &mdash; height and behind are reduced"
          : "Best on headphones"}</span>
      </div>
      <canvas class="spatial-panner-canvas" data-node-id="${node.id}" tabindex="0"></canvas>
      <p class="spatial-visualizer-help">Drag the radar to pan and set distance, drag the arc for height. Shift for fine, double-click to reset.</p>
    </div>
  ` : "";
  const graphicEqControls = isGraphicEqNode ? `
    <section class="graphic-eq-controls" data-node-id="${node.id}">
      <canvas class="graphic-eq-curve-canvas" aria-hidden="true"></canvas>
      <div class="graphic-eq-toolbar">
        <button type="button" class="graphic-eq-reset-btn" title="Reset all band gains to 0 dB">Reset</button>
      </div>
      <div class="graphic-eq-bands">
        ${GRAPHIC_EQ_FREQUENCIES.map((defaultFreq, index) => {
          const number = index + 1;
          const active = number <= (node.params.bandCount ?? 10);
          const enabled = (node.params[`band${number}Enabled`] ?? 1) >= 0.5;
          if (!active || !enabled) {
            return "";
          }
          const gain = node.params[`band${number}Gain`] ?? 0;
          const frequencyBounds = graphicEqFrequencyBounds(node.params, number);
          return `<div class="graphic-eq-band" data-band-number="${number}">
            <label class="graphic-eq-gain-value"><input class="graphic-eq-gain-value-input" data-param-key="band${number}Gain" type="number" inputmode="decimal" min="-18" max="18" step="0.1" value="${gain.toFixed(1)}"><span>dB</span></label>
            <input class="graphic-eq-gain" data-param-key="band${number}Gain" type="range" min="-18" max="18" step="0.1" value="${gain}" style="--graphic-eq-gain: ${((gain + 18) / 36) * 100}%">
            <label class="graphic-eq-frequency-label"><input class="graphic-eq-frequency" data-param-key="band${number}Freq" type="number" inputmode="numeric" min="${Math.ceil(frequencyBounds.min)}" max="${Math.floor(frequencyBounds.max)}" step="1" value="${Math.round(node.params[`band${number}Freq`] ?? defaultFreq)}"><span>Hz</span></label>
          </div>`;
        }).join("")}
      </div>
    </section>
  ` : "";

  const cabResponseSection = buildCabResponseSectionHtml(node);
  const mixerInputControls = buildMixerInputControlsHtml(node, preset);

  preloadResourceNavigationCaches(node, preset, typeInfo);

  const { html: resourceSelector, layoutControls: customLayoutResourceControls } =
    buildNodeResourceSelector(node, preset, typeInfo, blendState);

  // Resolve the layout to render. Preference rules (preset > keyword > effect type)
  // decide between the standard controls and any available custom layout; without
  // rules this falls back to the layout library default, i.e. previous behaviour.
  const nodeBlendId = blendState?.blend?.id || "";
  const nodeLayoutMatchText = buildNodeLayoutMatchText(node);
  const customLayout = resolveLayoutForNode({
    effectType: node.type,
    blendId: nodeBlendId || undefined,
    matchText: nodeLayoutMatchText,
    presetId: uiState.activePresetId,
  });

  // When useDefaultControls is true the layout provides only the visual backdrop; the
  // standard auto-generated controls are rendered on top rather than positioned controls.
  const useDefaultControls = customLayout?.useDefaultControls === true;
  const hasCustomLayoutPresentation = Boolean(customLayout);

  const customLayoutHtml = customLayout && !useDefaultControls
    ? renderCustomLayout(node, customLayout, paramDefs, customLayoutResourceControls)
    : null;
  const placeNeuralResourceInControls = isNeuralModelNode(node) && !customLayoutHtml;
  const layoutIncludesResourceControls = Boolean(
    customLayout && !useDefaultControls && customLayout.controls.some((control) => control.bindingType === "resource" || control.paramKey.startsWith("__resource__:")),
  );
  const placeCabIrResourcesInControls = node.type === EffectGuids.kCabIr && !layoutIncludesResourceControls && Boolean(resourceSelector);
  const cabIrResourceSelectors = placeCabIrResourcesInControls
    ? `<div class="effect-inline-resource-selectors cab-ir-resource-selectors">${resourceSelector}</div>`
    : "";
  const shellTitle = escapeHtml(getNodeDisplayName(node));
  const isNeuralModel = isNeuralModelNode(node);
  const shellCategoryLabel = escapeHtml(
    getNodeCategory(node)
      .replace(/[-_]/g, " ")
      .replace(/\b\w/g, (char) => char.toUpperCase())
  );
  const shellTypeLabel = escapeHtml(typeInfo?.displayName || shellCategoryLabel);
  const nodeIsBypassed = isNodeBypassed(node);
  const shellStatusLabel = nodeIsBypassed ? "Off" : "On";
  const shellBypassTitle = isNodeBypassed(node) ? "Enable effect" : "Bypass effect";
  const architectureBadge = getNodeArchitectureBadge(node);
  const calibrationMetadataChip = getNodeNamCalibrationMetadataChip(node);
  const shellBlendId = getBlendState(node)?.blend?.id || "";
  const fullRigCabModelNote = shouldShowFullRigCabModelNote(node, preset)
    ? `<div class="default-effect-shell-inline-note" role="status" aria-live="polite">Signal chain already includes a Cabinet Model (a "Full Rig").</div>`
    : "";
  const equipmentImage = getEffectVisualizationEquipmentImage(node);
  const layoutSwitchButton = renderLayoutSwitchButtonHtml(node, shellBlendId, Boolean(customLayout));
  const isInputAnalyzerNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kInputAnalyzer;
  if (isInputAnalyzerNode) {
    analyzerSpectrogramHistoryByNode.delete(node.id);
  }
  const analyzerSection = isInputAnalyzerNode ? `
    <section class="default-effect-section input-analyzer-panel" data-node-id="${node.id}">
      <div class="input-analyzer-header">
        <div class="input-analyzer-title">Signal Analyzer</div>
        <div class="input-analyzer-updated">Waiting for live analyzer data…</div>
      </div>
      <div class="input-analyzer-stats-grid">
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Peak (dBFS)</span><span class="input-analyzer-value" data-analyzer-field="peakDbfs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (dBFS)</span><span class="input-analyzer-value" data-analyzer-field="rmsDbfs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Peak (%FS)</span><span class="input-analyzer-value" data-analyzer-field="peakPercent">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (%FS)</span><span class="input-analyzer-value" data-analyzer-field="rmsPercent">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (dBu)</span><span class="input-analyzer-value" data-analyzer-field="rmsDbu">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (dBV)</span><span class="input-analyzer-value" data-analyzer-field="rmsDbv">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (Vrms)</span><span class="input-analyzer-value" data-analyzer-field="rmsVolts">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Momentary (LUFS)</span><span class="input-analyzer-value" data-analyzer-field="momentaryLufs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Short-term (LUFS)</span><span class="input-analyzer-value" data-analyzer-field="shortTermLufs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Integrated (LUFS)</span><span class="input-analyzer-value" data-analyzer-field="integratedLufs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Channels</span><span class="input-analyzer-value" data-analyzer-field="channelMode">—</span></div>
      </div>
      <div class="input-analyzer-spectrogram-wrap">
        <canvas class="input-analyzer-spectrogram-canvas" aria-label="Live spectrogram"></canvas>
      </div>
      <div class="input-analyzer-bark-wrap">
        <div class="input-analyzer-bark-title">Bark Perception</div>
        <canvas class="input-analyzer-bark-canvas" aria-label="Bark-band perception"></canvas>
      </div>
    </section>
  ` : "";
  const showEquipmentImage = Boolean(equipmentImage) && !hasCustomLayoutPresentation;
  // Capture artwork is remote, so keep the stock image as a fallback for when it
  // cannot be fetched (offline, or the author removed it).
  const stockEquipmentImage = getEffectVisualizationStockImage(node);
  const equipmentImageFallbackAttr = stockEquipmentImage && stockEquipmentImage !== equipmentImage
    ? ` data-fallback-src="${escapeHtml(stockEquipmentImage)}"`
    : "";
  const shellEquipmentPanel = showEquipmentImage ? `
    <aside class="default-effect-shell-equipment-panel" aria-hidden="true">
      <img class="default-effect-shell-equipment-image" src="${escapeHtml(equipmentImage)}" alt="" loading="lazy" decoding="async"${equipmentImageFallbackAttr} />
    </aside>
  ` : "";
  const shellMainContent = customLayoutHtml ? `
    ${fullRigCabModelNote}
    ${layoutIncludesResourceControls || placeCabIrResourcesInControls ? "" : resourceSelector}
    ${customEffectActions}
    ${analyzerSection}
    ${eqVisualizer}
    ${spatialVisualizer}
    ${graphicEqControls}
    ${cabResponseSection}
    ${mixerInputControls}
    <div class="default-effect-section default-effect-section-controls default-effect-section-custom-layout">
      ${cabIrResourceSelectors}
      ${customLayoutHtml}
    </div>
  ` : (() => {
    // Build the standard default controls HTML
    const neuralResourceSelector = placeNeuralResourceInControls && resourceSelector
      ? `<div class="neural-model-resource-selector">${resourceSelector}</div>`
      : "";
    const defaultControlsHtml = `
      ${neuralResourceSelector}
      ${hasAdvancedTab ? `
      <div class="node-param-tabs" role="tablist" aria-label="Parameter Groups">
        <button class="node-param-tab is-active" data-tab="main" type="button" role="tab" aria-selected="true">Main</button>
        <button class="node-param-tab" data-tab="advanced" type="button" role="tab" aria-selected="false">Advanced</button>
      </div>
      <div class="node-param-tab-panels">
        <div class="node-param-tab-panel is-active" data-tab="main" role="tabpanel">
          <div class="params-controls">
            ${buildParamControls(mainParamDefs)}
          </div>
        </div>
        <div class="node-param-tab-panel" data-tab="advanced" role="tabpanel">
          <div class="params-controls">
            ${buildParamControls(advancedParamDefs)}
          </div>
        </div>
      </div>
      ` : `
      <div class="params-controls">
        ${buildParamControls(paramDefs)}
      </div>
      `}
    `;
    // If a backdrop layout exists, wrap the default controls inside it
    const renderedControls = useDefaultControls && customLayout
      ? renderCustomLayoutBackdrop(node, customLayout, defaultControlsHtml)
      : defaultControlsHtml;
    return `
      ${fullRigCabModelNote}
      ${layoutIncludesResourceControls || placeNeuralResourceInControls || placeCabIrResourcesInControls ? "" : resourceSelector}
      ${customEffectActions}
      ${analyzerSection}
      ${eqVisualizer}
      ${spatialVisualizer}
      ${graphicEqControls}
      ${cabResponseSection}
      ${mixerInputControls}
      <div class="default-effect-section default-effect-section-controls">
        ${cabIrResourceSelectors}
        ${renderedControls}
      </div>
    `;
  })();

  // Gate on the parameters the registry declares, not on paramDefs: effects that
  // render their own controls (Graphic EQ) blank paramDefs for presentation and
  // would otherwise be denied presets despite having plenty to save.
  const hasSavableParams = (typeInfo?.parameters?.length ?? 0) > 0;
  const effectPresetsButton = hasSavableParams
    ? `<button class="default-effect-shell-chip default-effect-shell-chip-presets" type="button" data-effect-presets-open title="Factory and saved presets for this effect" aria-label="Effect presets" aria-haspopup="dialog" aria-expanded="false"><span>Presets</span>${renderIcon("chevron-down", "default-effect-shell-chip-caret")}</button>`
    : "";

  nodeParamsPanelElement.innerHTML = `

    <div class="node-params-body">
      <section class="default-effect-shell${isNeuralModel ? " neural-model-effect" : ""}${isNodeBypassed(node) ? " is-bypassed" : ""}${hasCustomLayoutPresentation ? " has-custom-layout" : ""}">
        <div class="default-effect-shell-header">
          <div class="default-effect-shell-identity">
            <span class="default-effect-shell-led" aria-hidden="true"></span>
            <div class="default-effect-shell-titles">
              <div class="default-effect-shell-title">${shellTitle}</div>
              <div class="default-effect-shell-subtitle">
                <span class="default-effect-shell-subtitle-text">${shellCategoryLabel} · ${shellTypeLabel}</span>
                ${architectureBadge ? `<span class="default-effect-shell-architecture-badge" title="Loaded model architecture">${escapeHtml(architectureBadge)}</span>` : ""}
              </div>
            </div>
          </div>
          <div class="default-effect-shell-rail">
            <button class="default-effect-shell-meter-toggle" type="button" aria-expanded="${isDspStatusVisible()}" title="${isDspStatusVisible() ? "Hide DSP status" : "Show DSP status"}" aria-label="Toggle DSP status">
              <span class="default-effect-shell-meter" style="--meter-fill-scale: 0"></span>
            </button>
            <div class="effect-dsp-status" aria-label="Live DSP status" ${isDspStatusVisible() ? "" : "hidden"}>
              <button class="effect-dsp-status-close" type="button" aria-label="Close DSP status" title="Close DSP status">×</button>
              <div><span>Peak avg</span><strong data-dsp-status-average="peak">—</strong></div>
              <div><span>RMS avg</span><strong data-dsp-status-average="rms">—</strong></div>
              <div><span>Headroom avg</span><strong data-dsp-status-average="headroom">—</strong></div>
              <div><span>Processing avg</span><strong data-dsp-status-average="processing">—</strong></div>
              <div><span>Latency avg</span><strong data-dsp-status-average="latency">—</strong></div>
            </div>
          </div>
          <div class="default-effect-shell-meta" aria-label="Module status">
            <button class="effect-visualization-toolbar-btn default-effect-shell-dsp-toggle dsp-badge-toggle${isDspStatusVisible() ? " is-active" : ""}" type="button" aria-expanded="${isDspStatusVisible()}" title="${isDspStatusVisible() ? "Hide DSP status" : "Show DSP status"}" aria-label="Toggle DSP status">${renderIcon("meter", "effect-visualization-toolbar-icon")}</button>
            ${effectPresetsButton}
            ${calibrationMetadataChip}
            <button
              class="default-effect-shell-toggle node-bypass-btn ${nodeIsBypassed ? "bypassed" : ""}"
              data-node-id="${node.id}"
              type="button"
              role="switch"
              aria-checked="${nodeIsBypassed ? "false" : "true"}"
              title="${shellBypassTitle}"
              aria-label="${shellBypassTitle}"
            ><span class="default-effect-shell-toggle-track" aria-hidden="true"></span><span class="default-effect-shell-toggle-label">${shellStatusLabel}</span></button>
            ${layoutSwitchButton}
          </div>
        </div>
        <div class="default-effect-shell-content${showEquipmentImage ? " has-equipment-image" : ""}">
          ${shellEquipmentPanel}
          <div class="default-effect-shell-main">
            ${shellMainContent}
          </div>
        </div>
      </section>
    </div>
  `;

  refreshNodeVisualizations(node);

  // Bind controls
  bindNodeParamControls(node, preset);
  bindEffectPresetsButton(node);
  bindGraphicEqControls(node, preset);
  bindCabResponseControls(node, preset);
  bindLayoutOverlayBypassToggles(node, preset);
  bindResourceControls(node, preset);
  bindEquipmentImageFallback();
  bindHostedPluginActionControls(node);
  bindHostedPluginListControls(node);
  bindCustomEffectActionControls(node);
  bindBlendEditorControls(nodeParamsPanelElement, node);
  bindBlendModeOverride(node);
  bindBypassButton(node, preset);
  bindSelectedNodeDspStatusToggle();
  bindLayoutSwitchButton(node, preset);
  bindParamTabs();
  applyCustomLayoutScaling(nodeParamsPanelElement);
  updateSelectedNodePeakMeter();
  updateSelectedNodeDspStatus();
  updateSelectedNodeAnalyzerPanel();
}

setNodeParamsPanelRenderer(showNodeParamsPanel);
