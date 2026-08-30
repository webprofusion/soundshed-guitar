import type { Attachment } from "./types.js";

/**
 * Derives a stable HSL accent colour from an ID string via a simple hash.
 * Hue is spread across the full 360° wheel; saturation/lightness are fixed
 * so every colour looks equally vibrant in both light and dark themes.
 */
export function idAccentColor(id: string): string {
  let hash = 5381;
  for (let i = 0; i < id.length; i++) {
    hash = (((hash * 33) ^ id.charCodeAt(i)) >>> 0);
  }
  const hue = hash % 360;
  return `hsl(${hue}, 62%, 55%)`;
}

export function escapeHtml(value: unknown): string {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

export function arrayBufferToBase64(buffer: ArrayBuffer): string {
  const bytes = new Uint8Array(buffer);
  let binary = "";
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    const slice = bytes.subarray(offset, offset + chunkSize);
    binary += String.fromCharCode(...slice);
  }
  return btoa(binary);
}

export function base64ToArrayBuffer(base64: string): ArrayBuffer {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes.buffer;
}

export async function sha256HexFromBase64(base64: string): Promise<string> {
  if (typeof crypto === "undefined" || !crypto.subtle) {
    return "";
  }
  const buffer = base64ToArrayBuffer(base64);
  const hashBuffer = await crypto.subtle.digest("SHA-256", buffer);
  const hashBytes = new Uint8Array(hashBuffer);
  return Array.from(hashBytes)
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

export function resolveDemoSamplePath(rawPath: string | null | undefined): string | null {
  if (!rawPath || typeof rawPath !== "string") {
    return null;
  }

  if (/^https?:\/\//i.test(rawPath)) {
    return rawPath;
  }

  const normalized = rawPath.replace(/\\/g, "/");
  if (!normalized.includes(":") && !normalized.startsWith("/")) {
    return normalized;
  }

  const uiIndex = normalized.toLowerCase().indexOf("/resources/ui/");
  if (uiIndex >= 0) {
    return normalized.slice(uiIndex + "/resources/ui/".length);
  }

  const lastSlash = normalized.lastIndexOf("/");
  return lastSlash >= 0 ? normalized.slice(lastSlash + 1) : normalized;
}

export interface WavMetadata {
  channels: number;
  sampleRate: number;
  bitsPerSample: number;
  /** Number of audio frames (samples per channel) parsed from the data chunk. */
  numFrames: number;
}

const WAV_FORMAT_PCM = 0x0001;
const WAV_FORMAT_IEEE_FLOAT = 0x0003;
const WAV_FORMAT_EXTENSIBLE = 0xFFFE;

export function parseWavMetadata(arrayBuffer: ArrayBuffer): WavMetadata | null {
  const view = new DataView(arrayBuffer);
  if (view.byteLength < 44) {
    return null;
  }

  const chunkId = String.fromCharCode(
    view.getUint8(0),
    view.getUint8(1),
    view.getUint8(2),
    view.getUint8(3),
  );
  const format = String.fromCharCode(
    view.getUint8(8),
    view.getUint8(9),
    view.getUint8(10),
    view.getUint8(11),
  );
  if (chunkId !== "RIFF" || format !== "WAVE") {
    return null;
  }

  let offset = 12;
  let channels = 0;
  let sampleRate = 0;
  let bitsPerSample = 0;
  let dataSize: number | null = null;

  while (offset + 8 <= view.byteLength) {
    const id = String.fromCharCode(
      view.getUint8(offset),
      view.getUint8(offset + 1),
      view.getUint8(offset + 2),
      view.getUint8(offset + 3),
    );
    const size = view.getUint32(offset + 4, true);
    const chunkStart = offset + 8;
    if (id === "fmt ") {
      if (size < 16 || chunkStart + size > view.byteLength) {
        return null;
      }

      let audioFormat = view.getUint16(chunkStart, true);
      if (audioFormat === WAV_FORMAT_EXTENSIBLE) {
        if (size < 40) {
          return null;
        }
        audioFormat = view.getUint16(chunkStart + 24, true);
      }

      if (audioFormat !== WAV_FORMAT_PCM && audioFormat !== WAV_FORMAT_IEEE_FLOAT) {
        return null;
      }
      channels = view.getUint16(chunkStart + 2, true);
      sampleRate = view.getUint32(chunkStart + 4, true);
      bitsPerSample = view.getUint16(chunkStart + 14, true);
    } else if (id === "data") {
      dataSize = size;
    }
    offset = chunkStart + size + (size % 2);
  }

  if (!channels || !sampleRate || !bitsPerSample || dataSize === null) {
    return null;
  }

  const bytesPerFrame = channels * Math.max(1, Math.ceil(bitsPerSample / 8));
  const numFrames = Math.floor(dataSize / Math.max(1, bytesPerFrame));

  return { channels, sampleRate, bitsPerSample, numFrames };
}

export function isRemoteUrl(url: string | null | undefined): boolean {
  return typeof url === "string" && /^https?:\/\//i.test(url);
}

export function resolveAttachmentUrl(attachment: Attachment, baseUrl: string): string | null {
  const candidates = [
    attachment.downloadUrl,
    attachment.url,
    attachment.href,
    attachment.filePath,
    attachment.path,
  ].filter(Boolean) as string[];

  const sanitizedBase = baseUrl.replace(/\/$/, "");

  for (const candidate of candidates) {
    if (isRemoteUrl(candidate)) {
      return candidate;
    }

    if (candidate.startsWith("/")) {
      return sanitizedBase ? `${sanitizedBase}${candidate}` : candidate;
    }

    if (!sanitizedBase) {
      if (candidate.startsWith("./") || candidate.startsWith("../") || !candidate.includes(":")) {
        return candidate;
      }
      continue;
    }

    return `${sanitizedBase}/${candidate.replace(/^\.\//, "")}`;
  }

  return null;
}

export function findResourceById<T extends { id: string }>(
  resources: T[] | undefined,
  resourceId: string | null | undefined
): T | undefined {
  if (!resources || !resourceId) return undefined;
  const exactMatch = resources.find((res) => res.id === resourceId);
  if (exactMatch) {
    return exactMatch;
  }
  if (resourceId.includes("__")) {
    const suffix = resourceId.split("__").pop();
    if (suffix) {
      const fallbackMatch = resources.find(
        (res) => res.id === suffix || res.id.endsWith("__" + suffix)
      );
      if (fallbackMatch) {
        return fallbackMatch;
      }
    }
  }
  return undefined;
}

// ── Numeric control helpers ─────────────────────────────────────────────────
// Shared by the knob widget (knob.ts) and the enhanced range inputs
// (controls.ts). They were duplicated in spirit before both needed them.

export function clampValue(value: number, minValue: number, maxValue: number): number {
  return Math.max(minValue, Math.min(maxValue, value));
}

export function countStepDecimals(stepValue: number): number {
  if (!isFinite(stepValue) || stepValue <= 0) {
    return 2;
  }

  const normalized = stepValue.toString().toLowerCase();
  if (normalized.includes("e-")) {
    const exponent = Number.parseInt(normalized.split("e-")[1] ?? "0", 10);
    return Number.isFinite(exponent) ? exponent : 2;
  }

  const fractional = normalized.split(".")[1];
  return fractional ? fractional.length : 0;
}

export function deriveRangeStep(minValue: number, maxValue: number, stepValue?: number): number {
  if (typeof stepValue === "number" && isFinite(stepValue) && stepValue > 0) {
    return stepValue;
  }

  const range = Math.abs(maxValue - minValue);
  if (!isFinite(range) || range <= 0) {
    return 0.01;
  }

  if (range <= 2) {
    return 0.01;
  }
  if (range <= 24) {
    return 0.1;
  }
  return Math.max(1, range / 100);
}
