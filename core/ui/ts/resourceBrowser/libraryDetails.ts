/**
 * The Library tab's details panel (the row's ℹ): a resource's id, file, tags and metadata,
 * and, for a model that came from Tone3000, a way back to its tone on the Tone3000 tab, where
 * the rest of the pack can be auditioned and imported. A model from anywhere else can be looked
 * up on Tone3000 by its content hash (tone3000ContentHash.ts).
 */

import type { LibraryResource } from "../types.js";
import { escapeHtml } from "../utils.js";
import { getResourceTags } from "./helpers.js";

const METADATA_LABELS: Record<string, string> = {
  provider: "Provider",
  authorUsername: "Author",
  modeledBy: "Modeled By",
  sourceUrl: "Source",
  architectureVersion: "Architecture",
  architecture_version: "Architecture",
  architecture: "Architecture",
  namFileVersion: "NAM File Version",
  sampleRate: "Sample Rate (Hz)",
  namName: "Model Name",
  gearMake: "Gear Make",
  gearModel: "Gear Model",
  gear_type: "Gear Type",
  toneType: "Tone Type",
  inputLevelDbu: "Input Level (dBu)",
  outputLevelDbu: "Output Level (dBu)",
  modelDate: "Model Date",
  trainingFinalLoss: "Training Final Loss",
  archive: "Pack Archive",
  factoryArchiveKey: "Pack",
  factoryArchiveHash: "Pack Hash",
  originalId: "Source ID",
  sourceFileName: "Source File",
  contentHash: "Content Hash",
  tone3000MatchedBy: "Tone3000 Match",
};

export interface LibraryDetailsOptions {
  /** The title of the Tone3000 tone the resource came from, when the Tone3000 tab can open it. */
  tone3000Title?: string;
  /** Offer to look the resource up on Tone3000 by its content hash. */
  tone3000Lookup?: boolean;
}

export function renderLibraryItemDetailsPanel(res: LibraryResource, options: LibraryDetailsOptions = {}): string {
  const metadata = res.metadata ?? {};
  const description = (res.description ?? "").trim();
  const rows: string[] = [];

  rows.push(`
    <tr>
      <td class="resource-browser-details-label">ID</td>
      <td class="resource-browser-details-value resource-browser-details-mono">${escapeHtml(res.id)}</td>
    </tr>
  `);

  const filePath = (res.filePath ?? "").trim();
  if (filePath) {
    rows.push(`
      <tr>
        <td class="resource-browser-details-label">File Path</td>
        <td class="resource-browser-details-value">
          <span class="resource-browser-details-path-group">
            <button class="resource-browser-action-icon-btn resource-browser-local-path-copy-btn" type="button" data-resource-id="${escapeHtml(res.id)}" title="Copy local file path" aria-label="Copy local file path"><svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg></button>
            <span class="resource-browser-details-mono">${escapeHtml(filePath)}</span>
          </span>
        </td>
      </tr>
    `);
  }

  const tags = getResourceTags(res);
  if (tags.length > 0) {
    rows.push(`
      <tr>
        <td class="resource-browser-details-label">Tags</td>
        <td class="resource-browser-details-value">${tags.map((tag) => `<span class="resource-browser-tag-pill">${escapeHtml(tag)}</span>`).join(" ")}</td>
      </tr>
    `);
  }

  for (const [key, value] of Object.entries(metadata)) {
    if (!value) continue;
    const label = METADATA_LABELS[key] ?? key.replace(/_/g, " ").replace(/([A-Z])/g, " $1").trim();
    let displayValue: string;
    if (key === "sourceUrl" && value.startsWith("http")) {
      displayValue = `<a class="resource-browser-details-link" href="${escapeHtml(value)}" target="_blank" rel="noopener noreferrer">${escapeHtml(value)}</a>`;
    } else if (key === "contentHash") {
      displayValue = `<span class="resource-browser-details-mono" title="${escapeHtml(value)}">${escapeHtml(value.slice(0, value.indexOf(":") + 13))}…</span>`;
    } else if (key === "tone3000MatchedBy" && value === "contentHash") {
      displayValue = "Found by content hash";
    } else {
      displayValue = escapeHtml(value);
    }
    rows.push(`
      <tr>
        <td class="resource-browser-details-label">${escapeHtml(label)}</td>
        <td class="resource-browser-details-value">${displayValue}</td>
      </tr>
    `);
  }

  const tone3000Action = options.tone3000Title
    ? `<button class="resource-browser-details-tone3000-btn" type="button" data-resource-id="${escapeHtml(res.id)}"
               title="Open this model's tone on the Tone3000 tab, with the rest of its models">
         Open "${escapeHtml(options.tone3000Title)}" in Tone3000
       </button>`
    : "";

  const tone3000Lookup = options.tone3000Lookup
    ? `<button class="resource-browser-details-tone3000-lookup-btn" type="button" data-resource-id="${escapeHtml(res.id)}"
               title="Look for a Tone3000 model with the same ${res.metadata?.contentHash?.startsWith("ir-") ? "samples" : "weights"}, whatever this file is called, and take its tone's details">
         Find on Tone3000
       </button>`
    : "";

  return `
    <div class="resource-browser-item-details-panel">
      ${tone3000Action}
      ${tone3000Lookup}
      ${description ? `<p class="resource-browser-details-description">${escapeHtml(description)}</p>` : ""}
      <table class="resource-browser-details-table">
        <tbody>
          ${rows.join("")}
        </tbody>
      </table>
    </div>
  `;
}
