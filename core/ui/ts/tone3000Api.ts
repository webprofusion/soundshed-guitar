import type { Tone3000Architecture, Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import { uiState } from "./state.js";

export const TONE3000_OFFICIAL_API_BASE = "https://www.tone3000.com/api/v1";
export const SOUNDSHED_TONE3000_PROXY_API_BASE = "https://api-guitar.soundshed.com/v1/resourcesearch";
const TONE3000_USE_SOUNDSHED_API_SETTING = "tone3000.useSoundshedToneSearchApi";
const TONE3000_API_MODE_SETTING = "tone3000.apiMode";
const TONE3000_PROXY_API_BASE_SETTING = "tone3000.proxyApiBaseUrl";

export type Tone3000ApiMode = "official" | "proxy";

export type Tone3000ApiClientConfig = {
  mode: Tone3000ApiMode;
  baseUrl: string;
  usingProxy: boolean;
};

export type Tone3000PaginatedLike = {
  page?: unknown;
  current_page?: unknown;
  total?: unknown;
  total_count?: unknown;
  count?: unknown;
  total_pages?: unknown;
  totalPages?: unknown;
  pages?: unknown;
};

function asRecord(value: unknown): Record<string, unknown> | null {
  if (!value || typeof value !== "object") {
    return null;
  }
  return value as Record<string, unknown>;
}

function getStringSetting(key: string): string {
  const value = uiState.appSettings?.[key];
  return typeof value === "string" ? value.trim() : "";
}

function getBooleanSetting(key: string): boolean {
  const value = uiState.appSettings?.[key];
  return value === true;
}

function withTrailingSlash(value: string): string {
  return value.endsWith("/") ? value : `${value}/`;
}

function toApiBaseUrl(candidate: string, fallback: string): string {
  const trimmed = candidate.trim();
  if (!trimmed) {
    return fallback;
  }

  try {
    const normalized = withTrailingSlash(trimmed);
    return new URL(".", normalized).toString().replace(/\/$/, "");
  } catch {
    return fallback;
  }
}

export function getTone3000ApiClientConfig(): Tone3000ApiClientConfig {
  const useSoundshedProxy = getBooleanSetting(TONE3000_USE_SOUNDSHED_API_SETTING);
  const proxyBaseOverride = toApiBaseUrl(getStringSetting(TONE3000_PROXY_API_BASE_SETTING), "");
  if (useSoundshedProxy) {
    return {
      mode: "proxy",
      baseUrl: proxyBaseOverride || SOUNDSHED_TONE3000_PROXY_API_BASE,
      usingProxy: true,
    };
  }

  const modeSetting = getStringSetting(TONE3000_API_MODE_SETTING).toLowerCase();
  const mode: Tone3000ApiMode = modeSetting === "proxy" ? "proxy" : "official";

  const proxyBase = proxyBaseOverride;
  if (mode === "proxy" && proxyBase) {
    return {
      mode,
      baseUrl: proxyBase,
      usingProxy: true,
    };
  }

  return {
    mode,
    baseUrl: TONE3000_OFFICIAL_API_BASE,
    usingProxy: false,
  };
}

function buildTone3000ApiUrl(path: string, params?: URLSearchParams): string {
  const { baseUrl } = getTone3000ApiClientConfig();
  const url = new URL(path, withTrailingSlash(baseUrl));
  if (params) {
    url.search = params.toString();
  }
  return url.toString();
}

export function getTone3000SessionUrl(): string {
  return buildTone3000ApiUrl("auth/session");
}

export function extractTone3000Tones(payload: unknown): Tone3000Tone[] {
  if (Array.isArray(payload)) {
    return payload as Tone3000Tone[];
  }

  const obj = asRecord(payload);
  if (!obj) {
    return [];
  }

  if (Array.isArray(obj.tones)) return obj.tones as Tone3000Tone[];
  if (Array.isArray(obj.results)) return obj.results as Tone3000Tone[];
  if (Array.isArray(obj.items)) return obj.items as Tone3000Tone[];
  if (Array.isArray(obj.data)) return obj.data as Tone3000Tone[];
  return [];
}

export function extractTone3000Models(payload: unknown): Tone3000Model[] {
  if (Array.isArray(payload)) {
    return payload as Tone3000Model[];
  }

  const obj = asRecord(payload);
  if (!obj) {
    return [];
  }

  if (Array.isArray(obj.models)) return obj.models as Tone3000Model[];
  if (Array.isArray(obj.data)) return obj.data as Tone3000Model[];
  if (Array.isArray(obj.results)) return obj.results as Tone3000Model[];
  return [];
}

/**
 * A tone's models in name order, so "Gain 2" comes before "Gain 10". Returns a
 * new array; ties fall back to the model id so the order is stable.
 */
export function sortTone3000ModelsByName(models: readonly Tone3000Model[]): Tone3000Model[] {
  return [...models].sort((a, b) =>
    (a.name ?? "").localeCompare(b.name ?? "", undefined, { numeric: true, sensitivity: "base" })
    || String(a.id).localeCompare(String(b.id), undefined, { numeric: true }));
}

export function buildTone3000ModelsUrl(
  toneId: string | number,
  page = 1,
  pageSize = 100,
  architecture?: Tone3000Architecture,
): string {
  const params = new URLSearchParams({
    tone_id: String(toneId),
    page: String(page),
    page_size: String(pageSize),
  });
  if (architecture) {
    params.set("architecture", architecture);
  }
  return buildTone3000ApiUrl("models", params);
}

/** Models asked for per page when listing a tone's models (the API allows up to 300). */
export const TONE3000_MODELS_PAGE_SIZE = 100;

/**
 * How many pages one listing follows at most: a guard against an API (or proxy) that ignores
 * `page`, not a limit any real tone reaches.
 */
const TONE3000_MODELS_MAX_PAGES = 50;

function reportsPageCount(data: Tone3000PaginatedLike | null): boolean {
  return [data?.total_pages, data?.totalPages, data?.pages, data?.total, data?.total_count, data?.count]
    .some((value) => typeof value === "number");
}

/**
 * All of a tone's models, following `/models` pagination: one page holds at most
 * `page_size` of them, so a tone with more was cut off at the first page. `fetchJson`
 * fetches one page URL and returns its parsed body, throwing on failure.
 *
 * Paging stops at the reported page count, or where none is reported at a short page. A
 * page that adds no model not already seen also ends it.
 */
export async function fetchAllTone3000ModelPages(
  fetchJson: (url: string) => Promise<unknown>,
  toneId: string | number,
  architecture?: Tone3000Architecture,
): Promise<Tone3000Model[]> {
  const models: Tone3000Model[] = [];
  const seenIds = new Set<string>();

  for (let page = 1; page <= TONE3000_MODELS_MAX_PAGES; page += 1) {
    const data = await fetchJson(buildTone3000ModelsUrl(toneId, page, TONE3000_MODELS_PAGE_SIZE, architecture));
    const pageModels = extractTone3000Models(data);
    const added = pageModels.filter((model) => {
      const id = String(model.id);
      if (seenIds.has(id)) {
        return false;
      }
      seenIds.add(id);
      return true;
    });
    models.push(...added);

    if (!added.length) {
      break;
    }

    const pagination = asRecord(data) as Tone3000PaginatedLike | null;
    const lastPage = reportsPageCount(pagination)
      ? page >= parseTone3000Pagination(pagination ?? undefined, page, TONE3000_MODELS_PAGE_SIZE).totalPages
      : pageModels.length < TONE3000_MODELS_PAGE_SIZE;
    if (lastPage) {
      break;
    }
  }

  return models;
}

export function buildTone3000SearchUrl(params: URLSearchParams): string {
  return buildTone3000ApiUrl("tones/search", params);
}

export function buildTone3000FavoritesUrl(params: URLSearchParams): string {
  return buildTone3000ApiUrl("tones/favorited", params);
}

export function parseTone3000Pagination(
  data: Tone3000PaginatedLike | undefined,
  currentPage: number,
  pageSize: number,
): { page: number; totalPages: number; total: number | null } {
  const page = typeof data?.page === "number"
    ? data.page
    : typeof data?.current_page === "number"
      ? data.current_page
      : currentPage;

  const total = typeof data?.total === "number"
    ? data.total
    : typeof data?.total_count === "number"
      ? data.total_count
      : typeof data?.count === "number"
        ? data.count
        : null;

  const totalPages = typeof data?.total_pages === "number"
    ? data.total_pages
    : typeof data?.totalPages === "number"
      ? data.totalPages
      : typeof data?.pages === "number"
        ? data.pages
        : total
          ? Math.max(1, Math.ceil(total / pageSize))
          : currentPage;

  return { page, totalPages: totalPages || currentPage, total };
}
