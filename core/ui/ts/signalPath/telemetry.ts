import { uiState } from "../state.js";
import { nodeDspLatencySamples, nodeDspProcessingTimeUs } from "../dspPerformance.js";
import type {
  InputAnalyzerLevelTelemetry,
  SignalLevelMetrics,
  SignalLevelNodeMetrics,
} from "../types.js";
import {
  ANALYZER_SPECTROGRAM_HISTORY_FRAMES,
  DSP_STATUS_AVERAGE_RENDER_INTERVAL_MS,
  DSP_STATUS_AVERAGE_SMOOTHING,
  analyzerSpectrogramHistoryByNode,
  getSelectedNodeId,
  nodeParamsPanelElement,
  selectedNodeDspStatusAverages,
} from "./state.js";

let selectedNodeDspStatusVisible = false;

let lastDspStatusAverageRenderAt = 0;

export function getSelectedNodeDiagnosticsEntry(): SignalLevelNodeMetrics | null {
  if (!getSelectedNodeId()) {
    return null;
  }

  const diagnostics = uiState.signalDiagnostics;
  if (!diagnostics) {
    return null;
  }

  if (getSelectedNodeId() === "__input__") {
    if (!diagnostics.input) {
      return null;
    }
    return {
      scope: "pre",
      nodeId: "__input__",
      nodeType: "input",
      levels: diagnostics.input,
    };
  }
  if (getSelectedNodeId() === "__output__") {
    if (!diagnostics.output) {
      return null;
    }
    return {
      scope: "post",
      nodeId: "__output__",
      nodeType: "output",
      levels: diagnostics.output,
    };
  }

  return diagnostics.nodes.find((entry) => entry.nodeId === getSelectedNodeId()) ?? null;
}

export function getSelectedNodeDiagnostics(): SignalLevelMetrics | null {
  return getSelectedNodeDiagnosticsEntry()?.levels ?? null;
}

export function normalizePeakDbfsForShellMeter(peakDbfs: number): number {
  const minDbfs = -48;
  const maxDbfs = 0;
  const normalized = (peakDbfs - minDbfs) / (maxDbfs - minDbfs);
  return Math.max(0, Math.min(1, normalized));
}

export function updateSelectedNodePeakMeter(): void {
  const rail = nodeParamsPanelElement?.querySelector(".default-effect-shell-rail") as HTMLElement | null;
  if (!rail) {
    return;
  }

  const meter = rail.querySelector<HTMLElement>(".default-effect-shell-meter");
  if (!meter) {
    return;
  }

  const metrics = getSelectedNodeDiagnostics();
  rail.classList.remove("is-inactive", "is-clipped");

  if (!metrics || !Number.isFinite(metrics.peakDbfs)) {
    rail.classList.add("is-inactive");
    rail.title = "No diagnostics data for this node";
    meter.style.setProperty("--meter-fill-scale", "0");
    return;
  }

  const normalized = normalizePeakDbfsForShellMeter(metrics.peakDbfs);
  meter.style.setProperty("--meter-fill-scale", normalized.toFixed(3));

  if (metrics.clipped || metrics.peakDbfs >= -0.3) {
    rail.classList.add("is-clipped");
  }

  rail.title = `Node peak: ${metrics.peakDbfs.toFixed(1)} dBFS · Headroom: ${metrics.headroomDb.toFixed(1)} dB`;
  // Keep the 3D amp glow in step with the same diagnostics stream as the meter.
  // Prefer the DSP-status peak average when available (updated just after this).
}

export function formatDspStatusDb(value: number | null | undefined): string {
  return Number.isFinite(value) ? `${value!.toFixed(1)} dBFS` : "—";
}

export function formatDspStatusHeadroom(value: number | null | undefined): string {
  return Number.isFinite(value) ? `${value!.toFixed(1)} dB` : "—";
}

export function getSelectedNodeDspStatusTimeUs(
  node: SignalLevelNodeMetrics | null,
): number | null {
  return node ? nodeDspProcessingTimeUs(node) : null;
}

export function getSelectedNodeDspStatusLatencySamples(
  node: SignalLevelNodeMetrics | null,
): number | null {
  return node ? nodeDspLatencySamples(node) : null;
}

export function formatDspStatusTime(timeUs: number | null): string {
  if (timeUs === null) {
    return "—";
  }

  const totalTimeUs = uiState.dspPerformance?.totalProcessingTimeUs;
  const share = typeof totalTimeUs === "number" && totalTimeUs > 0
    ? ` (${((timeUs / totalTimeUs) * 100).toFixed(1)}%)`
    : "";
  return `${timeUs.toFixed(1)} μs${share}`;
}

export function formatDspStatusLatency(latencySamples: number | null): string {
  if (latencySamples === null || latencySamples <= 0) {
    return "—";
  }

  const sampleRate = uiState.dspPerformance?.sampleRate;
  const milliseconds = typeof sampleRate === "number" && sampleRate > 0
    ? ` (${((latencySamples / sampleRate) * 1000).toFixed(2)} ms)`
    : "";
  return `${latencySamples} smp${milliseconds}`;
}

export function addDspStatusSample(name: string, value: number | null): number | null {
  if (value === null || !Number.isFinite(value)) {
    return null;
  }

  const previous = selectedNodeDspStatusAverages.get(name);
  const average = previous === undefined
    ? value
    : previous + DSP_STATUS_AVERAGE_SMOOTHING * (value - previous);
  selectedNodeDspStatusAverages.set(name, average);
  return average;
}

export function updateSelectedNodeDspStatus(): void {
  const status = nodeParamsPanelElement?.querySelector<HTMLElement>(".effect-dsp-status");
  const diagnostics = getSelectedNodeDiagnosticsEntry();
  const metrics = diagnostics?.levels;
  // Always keep the peak average warm so the 3D amp glow can use it even when
  // the DSP status panel is hidden.
  addDspStatusSample("peak", metrics?.peakDbfs ?? null);

  if (!status || !selectedNodeDspStatusVisible) {
    return;
  }

  const timeUs = getSelectedNodeDspStatusTimeUs(diagnostics);
  const latencySamples = getSelectedNodeDspStatusLatencySamples(diagnostics);
  const values: Record<string, string> = {
    peak: formatDspStatusDb(selectedNodeDspStatusAverages.get("peak") ?? null),
    rms: formatDspStatusDb(addDspStatusSample("rms", metrics?.rmsDbfs ?? null)),
    headroom: formatDspStatusHeadroom(addDspStatusSample("headroom", metrics?.headroomDb ?? null)),
    processing: formatDspStatusTime(addDspStatusSample("processing", timeUs)),
    latency: formatDspStatusLatency(addDspStatusSample("latency", latencySamples)),
  };

  const now = performance.now();
  const shouldRenderAverage = lastDspStatusAverageRenderAt === 0
    || now - lastDspStatusAverageRenderAt >= DSP_STATUS_AVERAGE_RENDER_INTERVAL_MS;
  Object.entries(values).forEach(([name, value]) => {
    const average = status.querySelector<HTMLElement>(`[data-dsp-status-average="${name}"]`);
    if (average && shouldRenderAverage && average.textContent !== value) {
      average.textContent = value;
    }
  });
  if (shouldRenderAverage) {
    lastDspStatusAverageRenderAt = now;
  }
}

export function bindSelectedNodeDspStatusToggle(): void {
  const meterToggle = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".default-effect-shell-meter-toggle");
  const dspBadge = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".dsp-badge-toggle");
  const status = nodeParamsPanelElement?.querySelector<HTMLElement>(".effect-dsp-status");
  if (!status) {
    return;
  }

  function setDspVisible(visible: boolean): void {
    selectedNodeDspStatusVisible = visible;
    meterToggle?.setAttribute("aria-expanded", String(visible));
    if (meterToggle) {
      meterToggle.title = visible ? "Hide DSP status" : "Show DSP status";
    }
    if (dspBadge) {
      dspBadge.setAttribute("aria-expanded", String(visible));
      dspBadge.title = visible ? "Hide DSP status" : "Show DSP status";
      dspBadge.classList.toggle("is-active", visible);
    }
    status!.hidden = !visible;
    updateSelectedNodeDspStatus();
  }

  meterToggle?.addEventListener("click", () => setDspVisible(!selectedNodeDspStatusVisible));
  dspBadge?.addEventListener("click", () => setDspVisible(!selectedNodeDspStatusVisible));

  status.querySelector<HTMLButtonElement>(".effect-dsp-status-close")?.addEventListener("click", () => {
    setDspVisible(false);
  });
}

export function formatAnalyzerNumeric(value: number, unit: string, fractionDigits = 1): string {
  if (!Number.isFinite(value)) {
    return "—";
  }
  return `${value.toFixed(fractionDigits)} ${unit}`;
}

export function formatAnalyzerChannelMode(levels: InputAnalyzerLevelTelemetry): string {
  const isStereo = typeof levels.channelMode === "string"
    ? levels.channelMode.toLowerCase() === "stereo"
    : Boolean(levels.stereo);
  const label = isStereo ? "Stereo" : "Mono";
  const channels = levels.activeChannelCount;
  if (Number.isFinite(channels) && (channels as number) > 0) {
    return `${label} (${channels} ch)`;
  }
  return label;
}

export function formatAnalyzerLufs(value: number | undefined, enabled = true): string {
  if (!enabled || !Number.isFinite(value)) {
    return "—";
  }
  return `${value!.toFixed(1)} LUFS`;
}

export function percentFsToDbfs(percentFs: number): number {
  if (!Number.isFinite(percentFs)) {
    return Number.NaN;
  }
  const linear = Math.max(0, Math.min(1, percentFs / 100));
  if (linear <= 1.0e-9) {
    return -120;
  }
  return 20 * Math.log10(linear);
}

export function drawAnalyzerSpectrogram(canvas: HTMLCanvasElement, history: number[][], minDbfs: number, maxDbfs: number): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }

  const width = Math.max(1, Math.floor(canvas.clientWidth || 1));
  const height = Math.max(1, Math.floor(canvas.clientHeight || 1));
  const dpr = window.devicePixelRatio || 1;
  const targetWidth = Math.max(1, Math.round(width * dpr));
  const targetHeight = Math.max(1, Math.round(height * dpr));
  if (canvas.width !== targetWidth || canvas.height !== targetHeight) {
    canvas.width = targetWidth;
    canvas.height = targetHeight;
  }

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "rgba(8, 10, 18, 0.92)";
  ctx.fillRect(0, 0, width, height);

  if (!history.length) {
    return;
  }

  const bins = history[0]?.length ?? 0;
  if (!bins) {
    return;
  }

  const dbRange = Math.max(1, maxDbfs - minDbfs);
  const columns = Math.min(width, history.length);
  const start = Math.max(0, history.length - columns);
  const rowHeight = height / bins;

  for (let x = 0; x < columns; ++x) {
    const frame = history[start + x];
    if (!Array.isArray(frame)) {
      continue;
    }
    for (let y = 0; y < bins; ++y) {
      const db = Number(frame[y]);
      const norm = Math.max(0, Math.min(1, (db - minDbfs) / dbRange));
      if (norm <= 0.001) {
        continue;
      }
      const hue = 230 - Math.round(norm * 190);
      const saturation = 76 + Math.round(norm * 18);
      const lightness = 12 + Math.round(norm * 56);
      ctx.fillStyle = `hsl(${hue} ${saturation}% ${lightness}%)`;
      ctx.fillRect(x, height - (y + 1) * rowHeight, 1, Math.max(1, rowHeight + 0.5));
    }
  }
}

export function drawAnalyzerBarkBands(canvas: HTMLCanvasElement, bandsDb: number[], minDbfs: number, maxDbfs: number): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }

  const width = Math.max(1, Math.floor(canvas.clientWidth || 1));
  const height = Math.max(1, Math.floor(canvas.clientHeight || 1));
  const dpr = window.devicePixelRatio || 1;
  const targetWidth = Math.max(1, Math.round(width * dpr));
  const targetHeight = Math.max(1, Math.round(height * dpr));
  if (canvas.width !== targetWidth || canvas.height !== targetHeight) {
    canvas.width = targetWidth;
    canvas.height = targetHeight;
  }

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "rgba(8, 10, 18, 0.92)";
  ctx.fillRect(0, 0, width, height);

  if (!Array.isArray(bandsDb) || !bandsDb.length) {
    return;
  }

  const barCount = bandsDb.length;
  const barGap = 1;
  const barWidth = Math.max(1, (width - (barCount - 1) * barGap) / barCount);
  const dbRange = Math.max(1, maxDbfs - minDbfs);

  for (let i = 0; i < barCount; ++i) {
    const db = Number(bandsDb[i]);
    const normalized = Math.max(0, Math.min(1, (db - minDbfs) / dbRange));
    const barHeight = Math.max(0, normalized * height);
    const x = i * (barWidth + barGap);
    const hue = 232 - Math.round(normalized * 168);
    const saturation = 76 + Math.round(normalized * 14);
    const lightness = 14 + Math.round(normalized * 58);
    ctx.fillStyle = `hsl(${hue} ${saturation}% ${lightness}%)`;
    ctx.fillRect(x, height - barHeight, barWidth, barHeight);
  }
}

export function updateSelectedNodeAnalyzerPanel(): void {
  const analyzerPanel = nodeParamsPanelElement?.querySelector<HTMLElement>(".input-analyzer-panel");
  const nodeId = getSelectedNodeId();
  if (!analyzerPanel || !nodeId) {
    return;
  }

  const diagnostics = getSelectedNodeDiagnosticsEntry();
  const analyzer = diagnostics?.analyzer;
  const levels = analyzer?.levels;
  const spectrogram = analyzer?.spectrogram;
  const bark = analyzer?.bark;

  const peakPercentEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="peakPercent"]');
  const rmsPercentEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsPercent"]');
  const rmsDbuEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsDbu"]');
  const rmsDbvEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsDbv"]');
  const rmsVoltsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsVolts"]');
  const momentaryLufsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="momentaryLufs"]');
  const shortTermLufsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="shortTermLufs"]');
  const integratedLufsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="integratedLufs"]');
  const channelModeEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="channelMode"]');
  const peakDbfsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="peakDbfs"]');
  const rmsDbfsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsDbfs"]');
  const updatedAtEl = analyzerPanel.querySelector<HTMLElement>(".input-analyzer-updated");
  const canvas = analyzerPanel.querySelector<HTMLCanvasElement>(".input-analyzer-spectrogram-canvas");
  const barkCanvas = analyzerPanel.querySelector<HTMLCanvasElement>(".input-analyzer-bark-canvas");

  if (!levels || !spectrogram || !Array.isArray(spectrogram.binsDb)) {
    if (peakPercentEl) peakPercentEl.textContent = "—";
    if (rmsPercentEl) rmsPercentEl.textContent = "—";
    if (rmsDbuEl) rmsDbuEl.textContent = "—";
    if (rmsDbvEl) rmsDbvEl.textContent = "—";
    if (rmsVoltsEl) rmsVoltsEl.textContent = "—";
    if (momentaryLufsEl) momentaryLufsEl.textContent = "—";
    if (shortTermLufsEl) shortTermLufsEl.textContent = "—";
    if (integratedLufsEl) integratedLufsEl.textContent = "—";
    if (channelModeEl) channelModeEl.textContent = "—";
    if (peakDbfsEl) peakDbfsEl.textContent = "—";
    if (rmsDbfsEl) rmsDbfsEl.textContent = "—";
    if (updatedAtEl) updatedAtEl.textContent = "Waiting for live analyzer data…";
    if (canvas) {
      drawAnalyzerSpectrogram(canvas, [], -120, 0);
    }
    if (barkCanvas) {
      drawAnalyzerBarkBands(barkCanvas, [], -96, 0);
    }
    return;
  }

  if (peakPercentEl) peakPercentEl.textContent = formatAnalyzerNumeric(levels.peakPercent, "%");
  if (rmsPercentEl) rmsPercentEl.textContent = formatAnalyzerNumeric(levels.rmsPercent, "%");
  if (rmsDbuEl) rmsDbuEl.textContent = formatAnalyzerNumeric(levels.rmsDbu, "dBu");
  if (rmsDbvEl) rmsDbvEl.textContent = formatAnalyzerNumeric(levels.rmsDbv, "dBV");
  if (rmsVoltsEl) rmsVoltsEl.textContent = formatAnalyzerNumeric(levels.rmsVolts, "Vrms", 3);
  if (momentaryLufsEl) momentaryLufsEl.textContent = formatAnalyzerLufs(levels.momentaryLufs, levels.loudnessValid !== false);
  if (shortTermLufsEl) shortTermLufsEl.textContent = formatAnalyzerLufs(levels.shortTermLufs, levels.loudnessValid !== false);
  if (integratedLufsEl) integratedLufsEl.textContent = formatAnalyzerLufs(levels.integratedLufs, levels.loudnessValid !== false);
  if (channelModeEl) channelModeEl.textContent = formatAnalyzerChannelMode(levels);
  if (peakDbfsEl) peakDbfsEl.textContent = formatAnalyzerNumeric(percentFsToDbfs(levels.peakPercent), "dBFS");
  if (rmsDbfsEl) rmsDbfsEl.textContent = formatAnalyzerNumeric(percentFsToDbfs(levels.rmsPercent), "dBFS");

  if (updatedAtEl) {
    updatedAtEl.textContent = Number.isFinite(spectrogram.generatedAtMs)
      ? `Updated: ${new Date(Number(spectrogram.generatedAtMs)).toLocaleTimeString()}`
      : "Updated: live";
  }

  const history = analyzerSpectrogramHistoryByNode.get(nodeId) ?? [];
  history.push(spectrogram.binsDb.slice());
  while (history.length > ANALYZER_SPECTROGRAM_HISTORY_FRAMES) {
    history.shift();
  }
  analyzerSpectrogramHistoryByNode.set(nodeId, history);

  if (canvas) {
    drawAnalyzerSpectrogram(
      canvas,
      history,
      Number.isFinite(spectrogram.minDbfs) ? spectrogram.minDbfs : -120,
      Number.isFinite(spectrogram.maxDbfs) ? spectrogram.maxDbfs : 0,
    );
  }
  if (barkCanvas) {
    drawAnalyzerBarkBands(
      barkCanvas,
      Array.isArray(bark?.bandsDb) ? bark.bandsDb : [],
      Number.isFinite(bark?.minDbfs) ? Number(bark?.minDbfs) : -96,
      Number.isFinite(bark?.maxDbfs) ? Number(bark?.maxDbfs) : 0,
    );
  }
}

/**
 * Clears the smoothed DSP averages. Called when the selected node changes so
 * the readout does not blend one effect's numbers into the next.
 */
export function resetDspStatusAverages(): void {
  selectedNodeDspStatusAverages.clear();
  lastDspStatusAverageRenderAt = 0;
}

/** Whether the DSP status readout is expanded in the node parameters panel. */
export function isDspStatusVisible(): boolean {
  return selectedNodeDspStatusVisible;
}
