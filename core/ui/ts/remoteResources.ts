/**
 * Resources that come from a remote provider (a model or IR catalogue), described the same way
 * whichever provider it is.
 *
 * Every remote import records these metadata keys alongside the provider's own, so the library,
 * preset archives and the tone builder can credit a resource, check what its licence allows and
 * fetch it again without knowing which provider it came from:
 *
 *   provider       provider id; the library id is "<provider>:<providerKey>"
 *   providerKey    the provider's own key for the file
 *   licence        licence id ("cc-by", "cc0", or the provider's own licence id)
 *   licenceUrl     where the licence text is
 *   creatorName    who made it, as the provider credits them
 *   creatorUrl     the creator's page
 *   sourceUrl      the resource's page at the provider
 *   captureScope   what a capture covers: amp, preamp, poweramp, amp+cab, pedal, outboard, cab, room
 */

export interface RemoteResourceAttribution {
  provider: string;
  providerKey: string;
  licence?: string | null;
  licenceUrl?: string | null;
  creatorName?: string | null;
  creatorUrl?: string | null;
  sourceUrl?: string | null;
  captureScope?: string | null;
}

/**
 * A preset archive's pointer to a remote resource it does not carry: enough to find it in the
 * library, or fetch it from its provider, and credit it.
 */
export interface RemoteResourceRef extends RemoteResourceAttribution {
  /** Library id the archive's presets use. */
  id: string;
  type: string;
  name?: string;
  category?: string;
  /**
   * The library's content hash of the file (the engine's 64-bit FNV-1a, as `LibraryResource.hash`),
   * which matches only another Soundshed library's copy of the same bytes.
   */
  hash?: string;
  /** SHA-256 of the file, the identity providers and the catalogue use across sources. */
  sha256?: string;
  /**
   * The file's content hash (metadata.contentHash): "nam-content-v1:" of a NAM model's weights or
   * "ir-content-v1:" of an IR's samples. It matches any copy of the same model, renamed or
   * re-tagged, in another library or at the provider, which can look the model up by it.
   */
  contentHash?: string;
  /** Provider-specific detail its adapter needs to fetch the file again. */
  providerData?: Record<string, string>;
}

/** What a licence lets us do with a file. */
export type LicencePolicy = "redistributable" | "redistributable-unmodified" | "noncommercial" | "reference-only";

const CREATIVE_COMMONS: Record<string, { url: string; policy: LicencePolicy }> = {
  "cc0": { url: "https://creativecommons.org/publicdomain/zero/1.0/", policy: "redistributable" },
  "cc-by": { url: "https://creativecommons.org/licenses/by/4.0/", policy: "redistributable" },
  "cc-by-sa": { url: "https://creativecommons.org/licenses/by-sa/4.0/", policy: "redistributable" },
  "cc-by-nd": { url: "https://creativecommons.org/licenses/by-nd/4.0/", policy: "redistributable-unmodified" },
  "cc-by-nc": { url: "https://creativecommons.org/licenses/by-nc/4.0/", policy: "noncommercial" },
  "cc-by-nc-sa": { url: "https://creativecommons.org/licenses/by-nc-sa/4.0/", policy: "noncommercial" },
  "cc-by-nc-nd": { url: "https://creativecommons.org/licenses/by-nc-nd/4.0/", policy: "noncommercial" },
};

/** A licence id in its usual spelling ("CC-BY" → "cc-by", "cco" → "cc0"). */
export function normaliseLicenceId(licence: string | null | undefined): string | null {
  const value = (licence ?? "").trim().toLowerCase().replace(/[\s_]+/g, "-");
  if (!value) return null;
  return value === "cco" || value === "cc-0" ? "cc0" : value;
}

export function licenceUrl(licence: string | null | undefined): string | null {
  return CREATIVE_COMMONS[normaliseLicenceId(licence) ?? ""]?.url ?? null;
}

/**
 * What the licence allows. Anything not a known open licence is reference-only: the file may be
 * named and fetched from its provider by whoever uses it, never bundled.
 */
export function licencePolicy(licence: string | null | undefined): LicencePolicy {
  return CREATIVE_COMMONS[normaliseLicenceId(licence) ?? ""]?.policy ?? "reference-only";
}

/** The provider-neutral metadata keys for an import, empty values left out. */
export function remoteResourceMetadata(attribution: RemoteResourceAttribution): Record<string, string> {
  const licence = normaliseLicenceId(attribution.licence);
  const entries: Array<[string, string | null | undefined]> = [
    ["provider", attribution.provider],
    ["providerKey", attribution.providerKey],
    ["licence", licence],
    ["licenceUrl", attribution.licenceUrl ?? licenceUrl(licence)],
    ["creatorName", attribution.creatorName],
    ["creatorUrl", attribution.creatorUrl],
    ["sourceUrl", attribution.sourceUrl],
    ["captureScope", attribution.captureScope],
  ];
  return Object.fromEntries(entries.filter((entry): entry is [string, string] => typeof entry[1] === "string" && entry[1].trim() !== ""));
}

/** The attribution a library resource's metadata records, or null when it is not remote. */
export function readRemoteAttribution(id: string, metadata: Record<string, string> | undefined): RemoteResourceAttribution | null {
  const provider = metadata?.provider?.trim();
  if (!provider) return null;
  const prefix = `${provider}:`;
  const providerKey = metadata?.providerKey?.trim() || (id.startsWith(prefix) ? id.slice(prefix.length) : "");
  if (!providerKey) return null;
  return {
    provider,
    providerKey,
    licence: normaliseLicenceId(metadata?.licence ?? metadata?.license),
    licenceUrl: metadata?.licenceUrl ?? null,
    creatorName: metadata?.creatorName ?? metadata?.authorUsername ?? null,
    creatorUrl: metadata?.creatorUrl ?? null,
    sourceUrl: metadata?.sourceUrl ?? null,
    captureScope: metadata?.captureScope ?? null,
  };
}
