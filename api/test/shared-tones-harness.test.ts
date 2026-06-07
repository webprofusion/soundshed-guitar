import { describe, expect, it } from "vitest";

type ApiSuccess<T> = {
  ok: true;
  data: T;
};

type ApiFailure = {
  ok: false;
  error: {
    code: string;
    message: string;
  };
};

type ApiEnvelope<T> = ApiSuccess<T> | ApiFailure;

type ItemListEntry = {
  id: string;
  title: string;
  type: string;
  publishedAt: string | null;
};

type ItemListResponse = {
  page: number;
  pageSize: number;
  items: ItemListEntry[];
};

type ItemDetailResponse = {
  item: {
    id: string;
    title: string;
    moderationStatus: string;
  };
};

type DownloadProbeResult = {
  itemId: string;
  title: string;
  publishedAt: string | null;
  detailStatus: number;
  downloadStatus: number;
  contentType: string;
  contentDisposition: string;
  downloadedBytes: number;
  errorCode: string | null;
  errorMessage: string | null;
};

const shouldRunLiveHarness = process.env.SOUNDSHED_LIVE_HARNESS === "1";
const API_BASE_URL = (process.env.SOUNDSHED_API_BASE_URL ?? "https://api-guitar.soundshed.com").replace(/\/$/, "");
const MAX_PRESETS = Math.max(1, Number.parseInt(process.env.SOUNDSHED_PRESET_LIMIT ?? "10", 10) || 10);
const SESSION_COOKIE = process.env.SOUNDSHED_API_COOKIE?.trim() ?? "";

async function fetchJson<T>(url: string): Promise<{ status: number; body: ApiEnvelope<T> | null }> {
  const headers = new Headers();
  if (SESSION_COOKIE.length > 0) {
    headers.set("cookie", SESSION_COOKIE);
  }

  const response = await fetch(url, { headers });
  let body: ApiEnvelope<T> | null = null;
  try {
    body = await response.json() as ApiEnvelope<T>;
  } catch {
    body = null;
  }

  return {
    status: response.status,
    body,
  };
}

async function probePreset(item: ItemListEntry): Promise<DownloadProbeResult> {
  const detailUrl = `${API_BASE_URL}/v1/items/${encodeURIComponent(item.id)}`;
  const detail = await fetchJson<ItemDetailResponse>(detailUrl);

  const headers = new Headers();
  if (SESSION_COOKIE.length > 0) {
    headers.set("cookie", SESSION_COOKIE);
  }

  const downloadUrl = `${API_BASE_URL}/v1/items/${encodeURIComponent(item.id)}/download`;
  const downloadResponse = await fetch(downloadUrl, { headers });
  const contentType = (downloadResponse.headers.get("content-type") ?? "").toLowerCase();
  const contentDisposition = downloadResponse.headers.get("content-disposition") ?? "";

  if (downloadResponse.ok) {
    const payload = await downloadResponse.arrayBuffer();
    return {
      itemId: item.id,
      title: item.title,
      publishedAt: item.publishedAt,
      detailStatus: detail.status,
      downloadStatus: downloadResponse.status,
      contentType,
      contentDisposition,
      downloadedBytes: payload.byteLength,
      errorCode: null,
      errorMessage: null,
    };
  }

  let errorCode: string | null = null;
  let errorMessage: string | null = null;

  if (contentType.includes("application/json")) {
    try {
      const json = await downloadResponse.json() as ApiEnvelope<unknown>;
      if (json && !json.ok) {
        errorCode = json.error.code;
        errorMessage = json.error.message;
      }
    } catch {
      // Keep null error fields when the response is malformed.
    }
  }

  return {
    itemId: item.id,
    title: item.title,
    publishedAt: item.publishedAt,
    detailStatus: detail.status,
    downloadStatus: downloadResponse.status,
    contentType,
    contentDisposition,
    downloadedBytes: 0,
    errorCode,
    errorMessage,
  };
}

const liveIt = shouldRunLiveHarness ? it : it.skip;

describe("shared tones live download harness", () => {
  liveIt("fetches latest presets and resolves downloadable payloads", { timeout: 180_000 }, async () => {
    const listUrl = `${API_BASE_URL}/v1/items?page=1&pageSize=${MAX_PRESETS}&type=preset`;
    const listResponse = await fetchJson<ItemListResponse>(listUrl);

    expect(listResponse.status).toBe(200);
    expect(listResponse.body?.ok).toBe(true);

    if (!listResponse.body || !listResponse.body.ok) {
      throw new Error(`Unable to query preset list from ${listUrl}`);
    }

    const latest = listResponse.body.data.items.slice(0, MAX_PRESETS);
    expect(latest.length).toBeGreaterThan(0);

    const probeResults: DownloadProbeResult[] = [];
    for (const preset of latest) {
      probeResults.push(await probePreset(preset));
    }

    const failures = probeResults.filter((result) => {
      if (result.detailStatus !== 200) {
        return true;
      }
      if (result.downloadStatus !== 200) {
        return true;
      }
      if (result.downloadedBytes <= 0) {
        return true;
      }
      if (!result.contentDisposition.toLowerCase().includes("attachment")) {
        return true;
      }
      return false;
    });

    if (failures.length > 0) {
      const failureLines = failures.map((failure) => {
        const reasonParts = [
          `detail=${failure.detailStatus}`,
          `download=${failure.downloadStatus}`,
          `bytes=${failure.downloadedBytes}`,
          failure.errorCode ? `code=${failure.errorCode}` : null,
          failure.errorMessage ? `message=${failure.errorMessage}` : null,
        ].filter((part): part is string => Boolean(part));
        return `- ${failure.itemId} (${failure.title}): ${reasonParts.join(", ")}`;
      });

      throw new Error(
        [
          `Shared tone download harness failed against ${API_BASE_URL}.`,
          `Checked ${probeResults.length} latest preset(s).`,
          ...failureLines,
        ].join("\n")
      );
    }
  });
});
