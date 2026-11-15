/**
 * The Tone3000 card at the top of a details panel (the Library's and the Folder tab's ℹ): the
 * tone's picture, its title and creator linked to tone3000.com, and the model's and tone's ids.
 * It reads the metadata a Tone3000 import or a content-hash match records
 * (tone3000ResourceMetadata in tone3000Shared.ts), so a model found by its hash shows the same
 * card as one imported from the Tone3000 tab.
 */

import { escapeHtml } from "../utils.js";

const TONE3000_ORIGIN = /^https:\/\/(www\.)?tone3000\.com\//;

/** Metadata keys the card shows, which the panel's table need not repeat. */
export const TONE3000_CARD_KEYS = new Set(["imageUrl"]);

/** Whether the metadata names a Tone3000 tone, so has a card to show. */
export function hasTone3000Card(metadata: Record<string, string> | undefined): boolean {
  return Boolean(metadata?.toneId?.trim());
}

/** The tone's page on tone3000.com: the recorded source when it is there, else built from the id. */
export function tone3000TonePageUrl(metadata: Record<string, string>): string {
  const source = metadata.sourceUrl?.trim() ?? "";
  if (TONE3000_ORIGIN.test(source)) {
    return source;
  }
  const toneId = metadata.toneId?.trim() ?? "";
  return toneId ? `https://www.tone3000.com/tones/${encodeURIComponent(toneId)}` : "";
}

function httpsOnly(value: string | undefined): string {
  const trimmed = value?.trim() ?? "";
  return trimmed.startsWith("https://") ? trimmed : "";
}

function link(href: string, text: string): string {
  return href
    ? `<a class="resource-browser-details-link" href="${escapeHtml(href)}" target="_blank" rel="noopener noreferrer">${escapeHtml(text)}</a>`
    : escapeHtml(text);
}

export function renderTone3000Card(metadata: Record<string, string> | undefined): string {
  if (!metadata || !hasTone3000Card(metadata)) {
    return "";
  }
  const toneUrl = tone3000TonePageUrl(metadata);
  const imageUrl = httpsOnly(metadata.imageUrl);
  const title = metadata.toneTitle?.trim() || metadata.groupName?.trim() || "Tone3000 tone";
  const creator = metadata.creatorName?.trim() || metadata.authorUsername?.trim() || "";
  const modelName = metadata.modelName?.trim() || metadata.entryName?.trim() || "";
  const modelId = metadata.modelId?.trim() ?? "";
  const facts: string[] = [];
  if (modelName || modelId) {
    const id = modelId ? ` <span class="resource-browser-details-mono">#${escapeHtml(modelId)}</span>` : "";
    facts.push(`<div>Model: ${escapeHtml(modelName || "unnamed")}${id}</div>`);
  }
  facts.push(`<div>Tone: <span class="resource-browser-details-mono">#${escapeHtml(metadata.toneId.trim())}</span></div>`);
  if (metadata.licence?.trim()) {
    facts.push(`<div>Licence: ${link(httpsOnly(metadata.licenceUrl), metadata.licence.trim())}</div>`);
  }
  if (metadata.tone3000MatchedBy === "contentHash") {
    facts.push(`<div class="resource-browser-tone3000-card-note">Found by content hash</div>`);
  }
  const image = imageUrl
    ? `<img class="resource-browser-tone3000-card-image" src="${escapeHtml(imageUrl)}" alt="" loading="lazy" referrerpolicy="no-referrer" />`
    : "";
  return `
    <div class="resource-browser-tone3000-card">
      ${image}
      <div class="resource-browser-tone3000-card-body">
        <div class="resource-browser-tone3000-card-title">${link(toneUrl, title)}</div>
        ${creator ? `<div class="resource-browser-tone3000-card-creator">by ${link(httpsOnly(metadata.creatorUrl), creator)}</div>` : ""}
        <div class="resource-browser-tone3000-card-facts">${facts.join("")}</div>
        ${toneUrl ? `<div class="resource-browser-tone3000-card-open">${link(toneUrl, "Open on tone3000.com ↗")}</div>` : ""}
      </div>
    </div>
  `;
}
