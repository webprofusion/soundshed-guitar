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
}

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
      const audioFormat = view.getUint16(chunkStart, true);
      if (audioFormat !== 1 && audioFormat !== 3) {
        return null;
      }
      channels = view.getUint16(chunkStart + 2, true);
      sampleRate = view.getUint32(chunkStart + 4, true);
      bitsPerSample = view.getUint16(chunkStart + 14, true);
      break;
    }
    offset = chunkStart + size + (size % 2);
  }

  if (!channels || !sampleRate || !bitsPerSample) {
    return null;
  }

  return { channels, sampleRate, bitsPerSample };
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
