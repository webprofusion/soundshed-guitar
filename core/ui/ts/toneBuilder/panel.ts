/**
 * The Tone Builder: name an artist, song or style, and the tone builder service researches the
 * rig, picks NAM captures and IRs, and proposes presets. The user reviews what it chose — every
 * capture with its creator and licence, every built-in stand-in for gear no capture covers —
 * before anything is downloaded, then the presets are built, saved, filed in a folder and
 * published on Tone Sharing as Soundshed (share.ts). Publishing is not optional.
 *
 * Opened from AI Tone Search by a "tone-builder:open" document event (so neither feature imports
 * the other), behind the toneBuilder feature flag and the service's kill switch (availability.ts).
 * Needs a Tone Sharing sign-in: the service is metered per account.
 */

import { switchMainPanel } from "../navigation.js";
import { showNotification } from "../notifications.js";
import { applyPresetFromLibrary } from "../presets/load.js";
import { ensureToneSharingSignedIn, openToneSharingSignInModal } from "../toneSharingPanel.js";
import { escapeHtml } from "../utils.js";
import { readAppBuildInventory } from "./appInventory.js";
import type { ProposalNode, ProposalSplit, ToneBrief, ToneBuilderJob, ToneProposal } from "./jobTypes.js";
import { requestPresetLevel } from "../presetLevel.js";
import { TARGET_LUFS } from "./levels.js";
import type { SceneBalance, SceneMeasure } from "./levels.js";
import { balanceProposal, buildAndSaveProposal, collectProposalCaptures, downloadProposalCaptures, isCaptureInstalled, planCaptureDownloads } from "./realise.js";
import { ensureEffectCatalogue, startToneBuilderJob, waitForToneBuilderJob } from "./service.js";
import { publishToneBuilderPresets } from "./share.js";
import { refreshToneBuilderStatus } from "./availability.js";
import { findCommunityMatches } from "./communityCheck.js";
import { packText } from "./text.js";
import { apiFetch, toneSharingFetch } from "../toneSharingPanel/api.js";
import type { CommunityMatch } from "./communityCheck.js";
import { downloadAsset } from "../toneSharingPanel/assets.js";
import { previewPreset } from "../toneSharingPanel/preview.js";
import type { ToneBuilderPublishResult } from "./share.js";
import type { Preset } from "../types.js";

export const TONE_BUILDER_OPEN_EVENT = "tone-builder:open";

type Stage = "brief" | "running" | "matches" | "review" | "building" | "done";

interface PanelState {
  stage: Stage;
  artist: string;
  song: string;
  notes: string;
  instrument: "guitar" | "bass";
  openOnly: boolean;
  thinking: ThinkingLevel;
  shareSummary: string;
  /** Built presets whose publishing failed, kept so it can be tried again. */
  publishPending: Array<{ key: string; preset: Preset }> | null;
  status: string;
  error: string;
  job: ToneBuilderJob | null;
  savedPresetIds: string[];
  folderName: string;
  levelReport: SceneBalance[];
  abort: AbortController | null;
  /** What Tone Sharing already has for this subject, found before researching. */
  matches: CommunityMatch[];
  /** The subject the community check last ran for, so "Build anyway" does not check again. */
  checkedSubject: string;
  /** The job's pack artwork, shown on review and used as the shared pack's thumbnail. */
  artworkUrl: string | null;
  artworkBlob: Blob | null;
  artworkRemaining: number;
  artworkBusy: boolean;
}

const state: PanelState = {
  stage: "brief", artist: "", song: "", notes: "", instrument: "guitar", openOnly: false, thinking: "thorough", shareSummary: "", publishPending: null,
  status: "", error: "", job: null, savedPresetIds: [], folderName: "", levelReport: [], abort: null, matches: [], checkedSubject: "", artworkUrl: null, artworkBlob: null, artworkRemaining: 0, artworkBusy: false,
};

let modal: HTMLElement | null = null;
let initialized = false;

export function initializeToneBuilderPanel(): void {
  if (initialized) return;
  initialized = true;
  document.addEventListener(TONE_BUILDER_OPEN_EVENT, (event) => {
    const detail = (event as CustomEvent<{ artist?: string; song?: string }>).detail ?? {};
    void openToneBuilder(detail.artist ?? "", detail.song ?? "");
  });
}

export async function openToneBuilder(artist = "", song = ""): Promise<void> {
  let signedIn = false;
  try {
    signedIn = await ensureToneSharingSignedIn();
  } catch {
    signedIn = false;
  }
  if (!signedIn) {
    showNotification("Sign in to build presets", "The Tone Builder uses your Soundshed account.");
    switchMainPanel("tones-community");
    openToneSharingSignInModal();
    return;
  }
  // Work under way, or presets still to publish, carries on; a new tone needs the service to be open.
  const resuming = state.stage === "running" || state.stage === "building" || (state.stage === "done" && state.publishPending !== null);
  if (!resuming) {
    const status = await refreshToneBuilderStatus();
    if (status && !status.available) {
      showNotification("Tone Builder unavailable", status.message ?? "The Tone Builder is not taking new tones right now.");
      return;
    }
  }
  if (!resuming) {
    Object.assign(state, { stage: "brief", status: "", error: "", job: null, savedPresetIds: [] });
    if (artist) state.artist = artist;
    if (song) state.song = song;
  }
  ensureModal().style.display = "flex";
  render();
}

function closeToneBuilder(): void {
  if (state.stage === "running") {
    state.abort?.abort();
  }
  if (state.stage === "building") {
    return;
  }
  if (modal) modal.style.display = "none";
}

function ensureModal(): HTMLElement {
  if (modal) return modal;
  modal = document.createElement("div");
  modal.id = "tone-builder-modal";
  modal.className = "modal tone-builder-modal";
  modal.style.display = "none";
  modal.addEventListener("mousedown", (event) => {
    if (event.target === modal) closeToneBuilder();
  });
  modal.addEventListener("click", (event) => {
    const target = (event.target as Element | null)?.closest<HTMLElement>("[data-tone-builder-action]");
    const action = target?.dataset.toneBuilderAction;
    if (target && action) void runAction(action, target);
  });
  // The hint under Thinking follows the choice.
  modal.addEventListener("change", (event) => {
    if ((event.target as Element | null)?.id === "tone-builder-thinking") {
      readForm();
      render();
    }
  });
  document.body.appendChild(modal);
  return modal;
}

function readForm(): void {
  const value = (id: string): string => (document.getElementById(id) as HTMLInputElement | null)?.value.trim() ?? "";
  state.artist = value("tone-builder-artist");
  state.song = value("tone-builder-song");
  state.notes = value("tone-builder-notes");
  state.instrument = value("tone-builder-instrument") === "bass" ? "bass" : "guitar";
  state.openOnly = (document.getElementById("tone-builder-open-only") as HTMLInputElement | null)?.checked ?? false;
  const thinking = value("tone-builder-thinking") as ThinkingLevel;
  state.thinking = THINKING_LEVELS.some((level) => level.id === thinking) ? thinking : "thorough";
}

async function runAction(action: string, target: HTMLElement): Promise<void> {
  if (action === "close") {
    closeToneBuilder();
  } else if (action === "research") {
    readForm();
    await research();
  } else if (action === "build") {
    await build();
  } else if (action === "again" || action === "back") {
    clearArtwork();
    Object.assign(state, { stage: "brief", status: "", error: "", job: null });
    render();
  } else if (action === "redraw-artwork") {
    await redrawArtwork();
  } else if (action === "publish-retry") {
    await publish();
    render();
  } else if (action === "build-anyway") {
    await research();
  } else if (action === "install-match" || action === "preview-match") {
    await useMatch(action, target);
  } else if (action === "open-first" && state.savedPresetIds[0]) {
    closeToneBuilder();
    await applyPresetFromLibrary(state.savedPresetIds[0]);
  }
}

function subjectLabel(): string {
  return [state.artist, state.song].filter(Boolean).join(" – ") || state.notes.slice(0, 40) || "Tone";
}

async function research(): Promise<void> {
  if (!state.artist && !state.song && !state.notes) {
    state.error = "Name an artist or song, or describe the tone.";
    render();
    return;
  }
  // Stop (closing the panel) aborts this from the first await on. The controller used to be made
  // only after the Tone Sharing check, and nothing looked at it before the job started, so a Stop
  // during the check or the catalogue upload still started the research job, which is metered.
  const abort = new AbortController();
  state.abort = abort;
  const subject = `${state.artist.toLowerCase()}|${state.song.toLowerCase()}`;
  if (subject !== state.checkedSubject && (state.artist || state.song)) {
    Object.assign(state, { stage: "running", error: "", status: "Looking on Tone Sharing first…", job: null });
    render();
    const matches = await findCommunityMatches(state.artist, state.song).catch(() => []);
    if (abort.signal.aborted) {
      Object.assign(state, { stage: "brief", status: "", error: "Cancelled" });
      render();
      return;
    }
    state.matches = matches;
    state.checkedSubject = subject;
    if (state.matches.length) {
      state.stage = "matches";
      render();
      return;
    }
  }
  Object.assign(state, { stage: "running", error: "", status: "Checking the effect catalogue…", job: null });
  render();
  try {
    const effectCatalogHash = await ensureEffectCatalogue(readAppBuildInventory().effects);
    if (abort.signal.aborted) throw new Error("Cancelled");
    const brief: ToneBrief = {
      subject: { artist: state.artist || null, song: state.song || null, freeText: state.notes || null },
      player: { instrument: state.instrument },
      constraints: { licencePolicy: state.openOnly ? "redistributable-only" : "any", providers: ["soundshed", "tone3000"] },
      options: { thinking: state.thinking },
      client: { app: "soundshed-guitar", effectCatalogHash },
    };
    state.status = "Starting…";
    render();
    const { jobId } = await startToneBuilderJob(brief);
    const job = await waitForToneBuilderJob(jobId, (update) => {
      state.job = update;
      state.status = update.progress[update.progress.length - 1]?.message ?? update.status;
      render();
    }, abort.signal);
    state.job = job;
    if (job.status === "failed" || !job.proposal) {
      throw new Error(job.error?.message ?? "The tone builder could not finish this tone");
    }
    state.stage = "review";
    clearArtwork();
    void loadArtwork(job);
  } catch (error) {
    state.stage = "brief";
    state.error = (error as Error).message;
  }
  render();
}

async function build(): Promise<void> {
  const job = state.job;
  const proposal = job?.proposal;
  if (!job || !proposal) return;
  const { toFetch, unfetchable } = planCaptureDownloads(proposal);
  if (unfetchable.length) {
    state.error = `This app cannot download captures from ${[...new Set(unfetchable.map((ref) => ref.provider))].join(", ")} yet.`;
    render();
    return;
  }
  Object.assign(state, { stage: "building", error: "", status: "", shareSummary: "", publishPending: null });
  render();
  try {
    await downloadProposalCaptures(toFetch, (done, total, ref) => {
      state.status = done < total ? `Downloading ${done + 1} of ${total}: ${ref.title}` : "Downloads done";
      render();
    });
    const { specs, report } = await balanceProposal(job.jobId, proposal, measureScene, (name) => {
      state.status = `Balancing levels: ${name}`;
      render();
    });
    state.levelReport = report;
    state.status = "Saving presets…";
    render();
    state.folderName = `Tone Builder – ${subjectLabel()}`;
    const { presets } = buildAndSaveProposal(job.jobId, proposal, state.folderName, specs);
    state.savedPresetIds = presets.map((preset) => preset.id);
    // Built in spec order, so each preset's key is its spec's.
    state.publishPending = job.published ? null : presets.map((preset, index) => ({ key: specs[index].key, preset }));
    if (job.published) state.shareSummary = "Already published on Tone Sharing.";
  } catch (error) {
    state.stage = "review";
    state.error = `Build failed: ${(error as Error).message}`;
    render();
    return;
  }
  // The presets are saved; publishing them is the last step, and is tried again if it fails.
  await publish();
  state.stage = "done";
  showNotification("Presets built", `${state.savedPresetIds.length} saved to "${state.folderName}"`);
  render();
}

/** Publishes the built presets on Tone Sharing as Soundshed. Every Tone Builder result is published. */
async function publish(): Promise<void> {
  const job = state.job;
  const pending = state.publishPending;
  if (!job || !pending) return;
  state.error = "";
  try {
    const published = await publishToneBuilderPresets(job.jobId, pending, (message) => {
      state.status = message;
      render();
    }, state.artworkBlob);
    state.publishPending = null;
    state.shareSummary = publishSummary(published);
  } catch (error) {
    state.shareSummary = "";
    state.error = `Publishing on Tone Sharing failed: ${(error as Error).message}`;
  }
}

function renderPackPreview(proposal: ToneProposal): string {
  const pack = packText(proposal, subjectLabel(), state.job?.dossier?.summary ?? "");
  const art = state.artworkUrl
    ? `<img class="tone-builder-artwork-image" src="${escapeHtml(state.artworkUrl)}" alt="Pack artwork" />`
    : `<div class="tone-builder-artwork-empty">${state.artworkBusy ? "Drawing…" : "No artwork yet"}</div>`;
  const redraw = state.artworkRemaining > 0
    ? `<button type="button" class="btn btn-secondary" data-tone-builder-action="redraw-artwork"${state.artworkBusy ? " disabled" : ""}>New artwork (${state.artworkRemaining} left)</button>`
    : "";
  return `
    <div class="tone-builder-pack">
      <figure class="tone-builder-artwork">${art}${redraw}</figure>
      <div class="tone-builder-pack-text">
        <h4>${escapeHtml(pack.title)}</h4>
        ${pack.description.split("\n\n").map((paragraph) => `<p>${escapeHtml(paragraph)}</p>`).join("")}
      </div>
    </div>`;
}

function clearArtwork(): void {
  if (state.artworkUrl) URL.revokeObjectURL(state.artworkUrl);
  Object.assign(state, { artworkUrl: null, artworkBlob: null, artworkRemaining: 0, artworkBusy: false });
}

/** Fetches the job's artwork. The image already shown stays until a new one has arrived. */
async function loadArtwork(job: ToneBuilderJob): Promise<void> {
  state.artworkRemaining = job.artwork?.remaining ?? 0;
  if (job.artwork?.available) {
    try {
      const response = await toneSharingFetch(`/tone-builder/jobs/${encodeURIComponent(job.jobId)}/artwork`);
      if (response.ok) {
        const blob = await response.blob();
        if (state.artworkUrl) URL.revokeObjectURL(state.artworkUrl);
        state.artworkBlob = blob;
        state.artworkUrl = URL.createObjectURL(blob);
      }
    } catch {
      // No artwork is fine: the pack is shared without a thumbnail.
    }
  }
  if (state.stage === "review") render();
}

async function redrawArtwork(): Promise<void> {
  const job = state.job;
  if (!job || state.artworkBusy) return;
  state.artworkBusy = true;
  render();
  try {
    const result = await apiFetch<{ artwork: { available: boolean; regenerations: number; remaining: number } }>(
      `/tone-builder/jobs/${encodeURIComponent(job.jobId)}/artwork`, { method: "POST" });
    state.artworkBusy = false;
    await loadArtwork({ ...job, artwork: result.artwork });
  } catch (error) {
    state.artworkBusy = false;
    state.error = `New artwork failed: ${(error as Error).message}`;
    render();
  }
}

function renderMatches(): string {
  return `
    <p>Tone Sharing already has ${state.matches.length === 1 ? "something" : "these"} for <strong>${escapeHtml(subjectLabel())}</strong>. Try ${state.matches.length === 1 ? "it" : "them"} first; building new presets costs a research run.</p>
    <ul class="tone-builder-matches">
      ${state.matches.map((match) => `
        <li>
          <div class="tone-builder-match-info">
            <span class="tone-builder-match-title">${escapeHtml(match.title)}</span>
            <span class="tone-builder-muted">${match.kind === "pack" ? "Pack" : "Preset"}${match.creator ? ` by ${escapeHtml(match.creator)}` : ""}</span>
            ${match.description ? `<span class="tone-builder-muted tone-builder-match-desc">${escapeHtml(match.description.slice(0, 160))}</span>` : ""}
          </div>
          <div class="tone-builder-match-actions">
            ${match.kind === "item" ? `<button type="button" class="btn btn-secondary" data-tone-builder-action="preview-match" data-kind="item" data-id="${escapeHtml(match.id)}" data-title="${escapeHtml(match.title)}">Preview</button>` : ""}
            <button type="button" class="btn btn-secondary" data-tone-builder-action="install-match" data-kind="${match.kind}" data-id="${escapeHtml(match.id)}"${match.installed ? " disabled" : ""}>${match.installed ? "Installed" : "Install"}</button>
          </div>
        </li>`).join("")}
    </ul>`;
}

async function useMatch(action: string, target: HTMLElement): Promise<void> {
  const kind = target.dataset.kind === "pack" ? "pack" : "item";
  const id = target.dataset.id ?? "";
  if (!id) return;
  const label = target.textContent;
  (target as HTMLButtonElement).disabled = true;
  target.textContent = action === "preview-match" ? "Previewing…" : "Installing…";
  try {
    if (action === "preview-match") {
      await previewPreset(id, target.dataset.title ?? "");
      target.textContent = "Previewing";
    } else {
      await downloadAsset(kind, id);
      target.textContent = "Installed";
      const match = state.matches.find((m) => m.id === id && m.kind === kind);
      if (match) match.installed = true;
    }
  } catch (error) {
    (target as HTMLButtonElement).disabled = false;
    target.textContent = label;
    state.error = `${action === "preview-match" ? "Preview" : "Install"} failed: ${(error as Error).message}`;
    render();
  }
}

type ThinkingLevel = NonNullable<NonNullable<ToneBrief["options"]>["thinking"]>;

/** The Thinking levels offered: the model's effort, and how much it searches. */
const THINKING_LEVELS: Array<{ id: ThinkingLevel; label: string; hint: string }> = [
  { id: "quick", label: "Quick", hint: "Fastest and cheapest: a short search, good for well-known tones." },
  { id: "balanced", label: "Balanced", hint: "A moderate search and a careful proposal." },
  { id: "thorough", label: "Thorough", hint: "The default: a full search with cited sources." },
  { id: "deep", label: "Deep", hint: "The longest research, for obscure rigs; slower and costs more." },
];

function publishSummary(published: ToneBuilderPublishResult): string {
  if (published.alreadyPublished) return "Already published on Tone Sharing.";
  return `Published on Tone Sharing as Soundshed: ${published.itemIds.length} preset(s)${published.packId ? " in a pack" : ""}.`;
}

/** Most of the demo DI is enough to judge a scene's level, and keeps a pack of scenes quick. */
const LEVEL_MEASURE_SECONDS = 45;

const measureScene: SceneMeasure = async (preset, sceneId) => {
  const level = await requestPresetLevel(preset, { sceneId, maxSeconds: LEVEL_MEASURE_SECONDS });
  return { integratedLufs: level.integratedLufs, peakDbfs: level.peakDbfs };
};

function chainLabel(item: ProposalNode | ProposalSplit): string {
  if ("split" in item && Array.isArray(item.split)) {
    return `Split (${item.split.map((branch) => branch.map(chainLabel).join(" → ") || "dry").join(" | ")})`;
  }
  const node = item as ProposalNode;
  return `${nodeLabel(node)}${node.off ? " (off)" : ""}`;
}

function nodeLabel(node: ProposalNode): string {
  const resource = node.resource;
  if (resource && "ref" in resource) return `${node.fx}: ${resource.title}`;
  if (resource) return `${resource.fallback.fx}${resource.fallback.preset ? ` · ${resource.fallback.preset}` : ""} (stand-in)`;
  return node.label || (node.preset ? `${node.fx} · ${node.preset}` : node.fx);
}

function renderReview(proposal: ToneProposal): string {
  const job = state.job;
  const captures = collectProposalCaptures(proposal);
  const toFetch = captures.filter((ref) => !isCaptureInstalled(ref));
  const warnings = proposal.validation.presets.flatMap((p) => p.warnings.map((w) => `${p.key}: ${w}`));
  const gaps = job?.dossier?.gaps ?? [];
  return `
    ${renderPackPreview(proposal)}
    <div class="tone-builder-presets">
      ${proposal.presets.map((preset) => `
        <section class="tone-builder-preset">
          <h4>${escapeHtml(preset.name)}${preset.tuning ? ` <span class="tone-builder-muted">${escapeHtml(preset.tuning)}</span>` : ""}</h4>
          <p>${escapeHtml(preset.description)}</p>
          ${preset.scenes.map((scene) => `
            <div class="tone-builder-scene"><strong>${escapeHtml(scene.title)}</strong>: ${escapeHtml(scene.chain.map(chainLabel).join(" → "))}</div>
          `).join("")}
        </section>`).join("")}
    </div>
    <h4>Captures</h4>
    <ul class="tone-builder-captures">
      ${captures.map((ref) => `
        <li>
          <span>${escapeHtml(ref.title)}</span>
          <span class="tone-builder-muted">${escapeHtml(ref.creator.name ?? "unknown creator")} · ${escapeHtml(ref.provider)} · ${escapeHtml(ref.licence.id ?? "licence unknown")}</span>
          <span class="tone-builder-badge">${isCaptureInstalled(ref) ? "installed" : "to download"}</span>
        </li>`).join("") || `<li class="tone-builder-muted">None: every amp and cab is a built-in stand-in.</li>`}
    </ul>
    ${gaps.length ? `<details><summary>What the research could not confirm (${gaps.length})</summary><ul>${gaps.map((gap) => `<li>${escapeHtml(gap)}</li>`).join("")}</ul></details>` : ""}
    ${warnings.length ? `<details><summary>Checks to look at (${warnings.length})</summary><ul>${warnings.map((w) => `<li>${escapeHtml(w)}</li>`).join("")}</ul></details>` : ""}
    <p class="tone-builder-muted">${toFetch.length ? `${toFetch.length} capture(s) will be downloaded from their providers.` : "Everything needed is already in your library."}
      The finished presets are published on Tone Sharing as Soundshed, for everyone to use.</p>`;
}

function render(): void {
  if (!modal) return;
  const busy = state.stage === "running" || state.stage === "building";
  const proposal = state.job?.proposal;
  let body = "";
  let footer = "";
  if (state.stage === "brief") {
    body = `
      <div class="tone-builder-form">
        <label for="tone-builder-artist">Artist</label>
        <input id="tone-builder-artist" type="text" value="${escapeHtml(state.artist)}" placeholder="e.g. AC/DC" />
        <label for="tone-builder-song">Song or record (optional)</label>
        <input id="tone-builder-song" type="text" value="${escapeHtml(state.song)}" placeholder="e.g. Back in Black" />
        <label for="tone-builder-notes">Style or notes (optional)</label>
        <textarea id="tone-builder-notes" rows="2" placeholder="e.g. the live 1980 rhythm sound">${escapeHtml(state.notes)}</textarea>
        <label for="tone-builder-instrument">Instrument</label>
        <select id="tone-builder-instrument">
          <option value="guitar"${state.instrument === "guitar" ? " selected" : ""}>Guitar</option>
          <option value="bass"${state.instrument === "bass" ? " selected" : ""}>Bass</option>
        </select>
        <label for="tone-builder-thinking">Thinking</label>
        <select id="tone-builder-thinking">
          ${THINKING_LEVELS.map((level) => `<option value="${level.id}"${state.thinking === level.id ? " selected" : ""}>${escapeHtml(level.label)}</option>`).join("")}
        </select>
        <p class="tone-builder-muted tone-builder-hint">${escapeHtml(THINKING_LEVELS.find((level) => level.id === state.thinking)?.hint ?? "")}</p>
        <label class="tone-builder-check"><input id="tone-builder-open-only" type="checkbox"${state.openOnly ? " checked" : ""} /> Only openly licensed captures</label>
      </div>
      <p class="tone-builder-muted">Research takes a minute or two. Nothing is downloaded until you have reviewed what it picks. The presets it builds are published on Tone Sharing as Soundshed.</p>`;
    footer = `<button type="button" class="btn btn-secondary" data-tone-builder-action="close">Cancel</button>
      <button type="button" class="btn btn-primary" data-tone-builder-action="research">Research tone</button>`;
  } else if (state.stage === "running" || state.stage === "building") {
    const progress = state.stage === "running" ? state.job?.progress ?? [] : [];
    body = `
      <div class="tone-sharing-progress tone-builder-progress"><span class="tone-sharing-progress-spinner" aria-hidden="true"></span>
        <span class="tone-sharing-progress-label">${escapeHtml(state.status || "Working…")}</span></div>
      ${progress.length ? `<ol class="tone-builder-steps">${progress.map((p) => `<li>${escapeHtml(p.message)}</li>`).join("")}</ol>` : ""}`;
    footer = state.stage === "running" ? `<button type="button" class="btn btn-secondary" data-tone-builder-action="close">Stop</button>` : "";
  } else if (state.stage === "matches") {
    body = renderMatches();
    footer = `<button type="button" class="btn btn-secondary" data-tone-builder-action="back">Back</button>
      <button type="button" class="btn btn-primary" data-tone-builder-action="build-anyway">Build anyway</button>`;
  } else if (state.stage === "review" && proposal) {
    body = renderReview(proposal);
    footer = `<button type="button" class="btn btn-secondary" data-tone-builder-action="again">Start over</button>
      <button type="button" class="btn btn-primary" data-tone-builder-action="build">${planCaptureDownloads(proposal).toFetch.length ? "Download and build" : "Build presets"}</button>`;
  } else if (state.stage === "done") {
    const unbalanced = state.levelReport.filter((scene) => !scene.balanced);
    body = `<p>${state.savedPresetIds.length} preset(s) saved to <strong>${escapeHtml(state.folderName)}</strong>.</p>
      ${state.shareSummary ? `<p>${escapeHtml(state.shareSummary)}</p>` : ""}
      <p class="tone-builder-muted">${state.levelReport.length - unbalanced.length} of ${state.levelReport.length} scene(s) balanced to ${TARGET_LUFS} LUFS on the demo DI.</p>
      ${unbalanced.length ? `<ul class="tone-builder-steps">${unbalanced.map((scene) => `<li>${escapeHtml(`${scene.presetKey} / ${scene.sceneKey}: ${scene.note ?? (scene.lufs !== null ? `${scene.lufs.toFixed(1)} LUFS after correction` : "not measured")}`)}</li>`).join("")}</ul>` : ""}`;
    footer = `<button type="button" class="btn btn-secondary" data-tone-builder-action="close">Close</button>
      ${state.publishPending ? `<button type="button" class="btn btn-secondary" data-tone-builder-action="publish-retry">Try publishing again</button>` : ""}
      <button type="button" class="btn btn-primary" data-tone-builder-action="open-first">Play the first one</button>`;
  }
  modal.innerHTML = `
    <div class="modal-content tone-builder-content" role="dialog" aria-label="Tone Builder">
      <div class="modal-header">
        <h3>Tone Builder${state.stage !== "brief" ? `: ${escapeHtml(subjectLabel())}` : ""}</h3>
        ${busy && state.stage === "building" ? "" : `<button type="button" class="modal-close" data-tone-builder-action="close" aria-label="Close">×</button>`}
      </div>
      <div class="modal-body">
        ${state.error ? `<div class="tone-sharing-status tone-builder-error">${escapeHtml(state.error)}</div>` : ""}
        ${body}
      </div>
      <div class="modal-footer">${footer}</div>
    </div>`;
}
