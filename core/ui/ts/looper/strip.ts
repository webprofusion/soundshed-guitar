/**
 * The looper strip's markup: the transport, the summary waveform and readout, the record modes and
 * the options, and (when open) the layers panel. renderLooperStrip() redraws it whole when what it
 * shows changes shape; updateLooperStrip() moves the playhead, the time and the bar in place, which
 * is all that changes while the loop runs.
 */

import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";
import { renderLooperLayers, updateLooperLayerControls } from "./layers.js";
import {
  getLivePeaks,
  getLooperLayers,
  getLooperProjects,
  getLooperStatus,
  getLayerPeaks,
  isLooperLayersOpen,
  mainButtonLabel,
  type LooperStatus,
} from "./state.js";
import { drawLooperWaveform } from "./waveform.js";

const STATE_LABELS: Record<string, string> = {
  empty: "Ready",
  countIn: "Count-in",
  armed: "Play to start",
  recording: "Recording",
  playing: "Playing",
  overdubArmed: "Play to overdub",
  overdubbing: "Overdubbing",
  stopped: "Stopped",
};

function formatTime(seconds: number): string {
  const safe = Math.max(0, seconds);
  const minutes = Math.floor(safe / 60);
  const rest = safe - minutes * 60;
  return `${minutes}:${rest.toFixed(1).padStart(4, "0")}`;
}

function segment(key: string, value: string, label: string, current: string): string {
  const on = value === current;
  return `<button type="button" class="looper-segment${on ? " is-on" : ""}" data-looper-set="${escapeHtml(key)}" data-value="${escapeHtml(value)}" aria-pressed="${on}">${label}</button>`;
}

function command(name: string, label: string, title: string, disabled = false): string {
  const attrs = `type="button" data-looper-command="${escapeHtml(name)}" title="${escapeHtml(title)}" aria-label="${escapeHtml(title)}"`;
  return `<button class="looper-btn looper-${escapeHtml(name)}-btn" ${attrs}${disabled ? " disabled" : ""}>${label}</button>`;
}

function option(value: number | string, label: string, current: number | string): string {
  return `<option value="${escapeHtml(value)}"${String(value) === String(current) ? " selected" : ""}>${label}</option>`;
}

function renderOptions(status: LooperStatus): string {
  const s = status.settings;
  const countIn = [0, 1, 2].map((n) => option(n, n === 0 ? "None" : `${n} bar${n > 1 ? "s" : ""}`, s.countInBars)).join("");
  const maxSeconds = [30, 60, 120, 300].map((n) => option(n, `${n} s`, s.maxSeconds)).join("");
  const after = option("play", "Play", s.afterRecord) + option("overdub", "Overdub", s.afterRecord);
  const click = `<input type="checkbox" data-looper-option="clickWhileRecording"${s.clickWhileRecording ? " checked" : ""} />`;
  const align = `<input type="number" min="-50" max="50" step="1" value="${s.alignOffsetMs}" data-looper-option="alignOffsetMs" />`;
  return `
    <details class="looper-options">
      <summary title="Looper options">Options</summary>
      <div class="looper-options-body">
        <label>Count-in <select data-looper-option="countInBars">${countIn}</select></label>
        <label>${click} Click while recording</label>
        <label>After a Bars take <select data-looper-option="afterRecord">${after}</select></label>
        <label>Longest loop <select data-looper-option="maxSeconds">${maxSeconds}</select></label>
        <label>Timing offset (ms) ${align}</label>
        <span class="looper-memory">${status.memoryMb.toFixed(0)} MB in use</span>
      </div>
    </details>`;
}

function renderModes(status: LooperStatus): string {
  const s = status.settings;
  const bars = s.length === "bars"
    ? `<span class="looper-bars-stepper">
         <button type="button" class="looper-step" data-looper-bars="-1" aria-label="Fewer bars">−</button>
         <span class="looper-bars-value">${s.bars} bar${s.bars === 1 ? "" : "s"}</span>
         <button type="button" class="looper-step" data-looper-bars="1" aria-label="More bars">+</button>
       </span>`
    : "";
  const tempo = status.tempoBpm ?? null;
  const lock = status.tempoLocked ? " is-locked" : "";
  const tempoTitle = status.tempoLocked ? "Tempo locked to the loop" : "Open the metronome";
  const clickTitle = "The metronome as the loop's click: it stops with the loop and starts again in time with it. Saved with the loop.";
  const click = `<button type="button" class="looper-click-toggle${status.click ? " is-on" : ""}" data-looper-click aria-pressed="${status.click}" title="${escapeHtml(clickTitle)}">Click</button>`;
  return `
    <div class="looper-modes">
      <span class="looper-segmented" role="group" aria-label="Start">${segment("start", "press", "Press", s.start)}${segment("start", "armed", "Armed", s.start)}</span>
      <span class="looper-segmented" role="group" aria-label="Length">${segment("length", "free", "Free", s.length)}${segment("length", "bars", "Bars", s.length)}</span>
      ${bars}
      ${click}
      <button type="button" class="looper-tempo-chip${lock}" data-looper-tempo title="${tempoTitle}">${tempo ? `${tempo.toFixed(0)} BPM` : "Tempo"}</button>
      ${renderOptions(status)}
    </div>`;
}

function renderProjectMenu(status: LooperStatus): string {
  const recover = status.recoverable
    ? `<div class="looper-project-recover">
         <span>The last session ended with an unsaved loop.</span>
         <button type="button" data-looper-recover>Recover</button>
         <button type="button" data-looper-dismiss>Discard</button>
       </div>`
    : "";
  const rows = getLooperProjects().map((project) => {
    const id = escapeHtml(project.id);
    const current = project.id === status.projectId ? " is-current" : "";
    const detail = `${project.lengthSec.toFixed(1)} s · ${project.layerCount} layer${project.layerCount === 1 ? "" : "s"}${project.bars ? ` · ${project.bars} bars` : ""}`;
    return `
      <li class="looper-project-row${current}" data-project-id="${id}">
        <button type="button" class="looper-project-open" data-looper-open title="Open">${escapeHtml(project.title || "Untitled loop")}</button>
        <span class="looper-project-detail">${escapeHtml(detail)}</span>
        <button type="button" class="looper-project-delete" data-looper-delete title="Delete" aria-label="Delete loop">×</button>
      </li>`;
  }).join("");
  const list = rows ? `<ul class="looper-project-list">${rows}</ul>` : `<p class="looper-project-empty">No saved loops yet.</p>`;
  // Riffs come in as a layer, rendered through the current rig (or as the loop, when there is none).
  const riffRows = (uiState.riffLibrary?.riffs ?? []).map((riff) => {
    const take = riff.takes.find((t) => t.id === riff.preferredTakeId) ?? riff.takes[riff.takes.length - 1];
    return take
      ? `<li class="looper-riff-row"><button type="button" class="looper-project-open" data-looper-add-riff="${escapeHtml(take.id)}" title="Add as a layer">${escapeHtml(riff.title || "Riff")}</button></li>`
      : "";
  }).join("");
  const riffs = riffRows ? `<p class="looper-projects-heading">Add a riff as a layer</p><ul class="looper-project-list">${riffRows}</ul>` : "";
  return `
    <details class="looper-projects-menu" data-looper-projects>
      <summary title="Saved loops">Loops</summary>
      <div class="looper-projects-body">${recover}${list}${riffs}</div>
    </details>`;
}

function renderProject(status: LooperStatus): string {
  const dirty = status.dirty ? `<span class="looper-dirty" title="Not saved">•</span>` : "";
  const title = `<input type="text" class="looper-project-title" maxlength="120" placeholder="Untitled loop" value="${escapeHtml(status.projectTitle)}" data-looper-title aria-label="Loop name" />`;
  const save = `<button type="button" class="looper-save-btn" data-looper-save${status.layerCount === 0 ? " disabled" : ""}>Save</button>`;
  return `<div class="looper-project">${title}${dirty}${save}${renderProjectMenu(status)}</div>`;
}

const MORE_ICON = `<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true" focusable="false"><circle cx="5" cy="12" r="1.75" /><circle cx="12" cy="12" r="1.75" /><circle cx="19" cy="12" r="1.75" /></svg>`;

/**
 * The strip, drawn whole into `root`. `compact` is the one-row strip of a small display: Redo,
 * Clear, the modes and the loop's project go into the "More" sheet instead of the row (they are
 * drawn once either way). The sheet shows while `root` has the is-more-open class.
 */
export function renderLooperStrip(root: HTMLElement, compact = false): void {
  const status = getLooperStatus();

  if (!status) {
    root.innerHTML = "";
    return;
  }

  const state = status.state;
  const empty = state === "empty";
  const countIn = state === "countIn" && status.countInBeatsLeft !== undefined
    ? `<div class="looper-countin" aria-live="assertive">${status.countInBeatsLeft}</div>`
    : "";
  const layersOpen = isLooperLayersOpen();
  const redoClear = `${command("redo", "Redo", "Redo", !status.canRedo)}
        ${command("clear", "Clear", "Clear the loop", empty && !status.canRedo)}`;
  const layersToggle = `<button type="button" class="looper-layers-toggle" data-looper-layers aria-expanded="${layersOpen}">Layers</button>`;
  const moreOpen = root.classList.contains("is-more-open");
  const tail = compact
    ? `${layersToggle}
      <button type="button" class="looper-more-btn" data-looper-more aria-label="More looper controls" title="More looper controls" aria-haspopup="true" aria-expanded="${moreOpen}" aria-controls="looper-more-sheet">${MORE_ICON}</button>
    </div>
    <div class="looper-more-sheet" id="looper-more-sheet" role="group" aria-label="More looper controls">
      <div class="looper-transport looper-more-transport">
        ${redoClear}
      </div>
      ${renderModes(status)}
      ${renderProject(status)}
    </div>`
    : `${renderProject(status)}
      ${renderModes(status)}
      ${layersToggle}
    </div>`;
  root.dataset.looperState = state;
  root.innerHTML = `
    <div class="looper-strip-row">
      <div class="looper-transport">
        ${command("recPlayDub", mainButtonLabel(state), "Record, play, overdub")}
        ${command("stop", "Stop", "Stop", empty)}
        ${command("undo", "Undo", "Undo the last layer", !status.canUndo && !["recording", "overdubbing", "countIn", "armed", "overdubArmed"].includes(state))}
        ${compact ? "" : redoClear}
      </div>
      <div class="looper-display">
        <canvas class="looper-wave" aria-hidden="true"></canvas>
        <div class="looper-progress"><span class="looper-progress-fill"></span></div>
        <div class="looper-readout">
          <span class="looper-status">${escapeHtml(STATE_LABELS[state] ?? state)}</span>
          <span class="looper-time"></span>
          <span class="looper-bar"></span>
          <span class="looper-layer-count">${status.layerCount} layer${status.layerCount === 1 ? "" : "s"}</span>
        </div>
        ${countIn}
      </div>
      ${tail}
    <div class="looper-layers"${layersOpen ? "" : " hidden"}>${layersOpen ? renderLooperLayers(getLooperLayers()) : ""}</div>`;
  updateLooperStrip(root);
}

/** The parts that move while the loop runs: time, bar, playhead, waveform, layer sliders. */
export function updateLooperStrip(root: HTMLElement): void {
  const status = getLooperStatus();

  if (!status) {
    return;
  }

  const live = getLivePeaks();
  const time = root.querySelector<HTMLElement>(".looper-time");
  const bar = root.querySelector<HTMLElement>(".looper-bar");
  const fill = root.querySelector<HTMLElement>(".looper-progress-fill");
  const recordingFirst = status.state === "recording" && status.lengthSec === 0;

  if (time) {
    time.textContent = recordingFirst
      ? formatTime(status.recordedSec ?? 0)
      : `${formatTime(status.positionSec)} / ${formatTime(status.lengthSec)}`;
  }

  if (bar) {
    bar.textContent = status.bars && status.barIndex ? `bar ${status.barIndex} of ${status.bars}` : "";
  }

  if (fill) {
    const ratio = status.lengthSec > 0 ? Math.min(1, status.positionSec / status.lengthSec) : 0;
    fill.style.width = `${(ratio * 100).toFixed(2)}%`;
  }

  const canvas = root.querySelector<HTMLCanvasElement>(".looper-wave");

  if (canvas) {
    drawLooperWaveform(canvas, status, getLooperLayers(), getLayerPeaks, live);
  }

  updateLooperLayerControls(root, getLooperLayers());
}
