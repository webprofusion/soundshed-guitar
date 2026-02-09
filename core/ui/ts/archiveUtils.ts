import { postMessage } from "./bridge.js";
import { arrayBufferToBase64 } from "./utils.js";
import type { LibraryResource } from "./types.js";

export type ResourceDataResponse = {
  requestId: string;
  data?: string;
  fileName?: string;
  message?: string;
};

const resourceRequests = new Map<string, (data?: string) => void>();

export function sanitizeFilename(raw: string, fallback = "file"): string {
  const trimmed = raw.trim() || fallback;
  return trimmed.replace(/[^a-z0-9-_\.]+/gi, "-");
}

export function buildArchiveFileName(resource: LibraryResource, resourceType: string): string {
  const pathName = resource.filePath ? resource.filePath.split(/[\\/]/).pop() ?? "" : "";
  if (pathName) {
    return pathName;
  }
  const ext = resourceType === "ir" ? ".wav" : resourceType === "nam" ? ".nam" : ".bin";
  return `${sanitizeFilename(resource.id || resource.name || "resource")}${ext}`;
}

export function generateResourceId(seed: string): string {
  if (typeof crypto !== "undefined" && "randomUUID" in crypto) {
    return crypto.randomUUID();
  }
  return `${Date.now()}-${sanitizeFilename(seed)}-${Math.random().toString(16).slice(2)}`;
}

export async function requestResourceData(resourceType: string, resourceId: string): Promise<string | undefined> {
  const requestId = generateResourceId(resourceId);
  const promise = new Promise<string | undefined>((resolve) => {
    resourceRequests.set(requestId, resolve);
  });

  postMessage({
    type: "requestResourceData",
    requestId,
    resourceType,
    resourceId,
  });

  return promise;
}

export function handleResourceDataMessage(payload: ResourceDataResponse): void {
  const resolve = resourceRequests.get(payload.requestId);
  if (!resolve) {
    return;
  }
  resourceRequests.delete(payload.requestId);
  resolve(payload.data);
}

export { arrayBufferToBase64 };
