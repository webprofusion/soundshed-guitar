import type { Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import type { Tone3000Architecture } from "./tone3000ApiTypes.js";
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

export function buildTone3000SearchUrl(params: URLSearchParams): string {
  return buildTone3000ApiUrl("tones/search", params);
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
