import { Hono } from "hono";
import { fail, ok, safeJson } from "../lib/http";
import { requireAuth } from "../middleware/session";
import { Env } from "../types/env";
import { randomId } from "../lib/utils";

type UploadKind = "item_payload" | "item_manifest" | "pack_zip" | "thumbnail" | "preview_audio";

type UploadInitBody = {
  kind?: UploadKind;
  mimeType?: string;
  byteSize?: number;
};

type UploadCompleteBody = {
  uploadId?: string;
};

type ValidationResult = { ok: true } | { ok: false; reason: string };

const allowedKinds = new Set<UploadKind>(["item_payload", "item_manifest", "pack_zip", "thumbnail", "preview_audio"]);
const decoder = new TextDecoder();

function hasSignature(bytes: Uint8Array, signature: number[], offset = 0): boolean {
  if (offset < 0 || bytes.length < offset + signature.length) {
    return false;
  }
  for (let index = 0; index < signature.length; index++) {
    if (bytes[offset + index] !== signature[index]) {
      return false;
    }
  }
  return true;
}

function getExtension(path: string): string {
  const fileName = path.split("/").pop() ?? path;
  const dotIndex = fileName.lastIndexOf(".");
  if (dotIndex <= 0 || dotIndex === fileName.length - 1) {
    return "";
  }
  return fileName.slice(dotIndex).toLowerCase();
}

function isZipBuffer(bytes: Uint8Array): boolean {
  if (bytes.length < 4) {
    return false;
  }
  return bytes[0] === 0x50 && bytes[1] === 0x4b && (bytes[2] === 0x03 || bytes[2] === 0x05 || bytes[2] === 0x07) && (bytes[3] === 0x04 || bytes[3] === 0x06 || bytes[3] === 0x08);
}

function parseZipEntryNames(body: ArrayBuffer): string[] {
  const view = new DataView(body);
  const bytes = new Uint8Array(body);
  const minimumEocdLength = 22;
  let eocdOffset = -1;
  const searchStart = Math.max(0, view.byteLength - 65557);

  for (let offset = view.byteLength - minimumEocdLength; offset >= searchStart; offset--) {
    if (view.getUint32(offset, true) === 0x06054b50) {
      eocdOffset = offset;
      break;
    }
  }

  if (eocdOffset < 0) {
    throw new Error("ZIP end-of-central-directory record missing");
  }

  const totalEntries = view.getUint16(eocdOffset + 10, true);
  const centralDirectoryOffset = view.getUint32(eocdOffset + 16, true);
  if (centralDirectoryOffset >= view.byteLength) {
    throw new Error("ZIP central directory offset is invalid");
  }

  let cursor = centralDirectoryOffset;
  const entries: string[] = [];

  for (let index = 0; index < totalEntries; index++) {
    if (cursor + 46 > view.byteLength || view.getUint32(cursor, true) !== 0x02014b50) {
      throw new Error("ZIP central directory entry is invalid");
    }

    const fileNameLength = view.getUint16(cursor + 28, true);
    const extraLength = view.getUint16(cursor + 30, true);
    const commentLength = view.getUint16(cursor + 32, true);
    const nameStart = cursor + 46;
    const nameEnd = nameStart + fileNameLength;

    if (nameEnd > view.byteLength) {
      throw new Error("ZIP file name range is invalid");
    }

    const entryName = decoder.decode(bytes.subarray(nameStart, nameEnd));
    entries.push(entryName);

    cursor = nameEnd + extraLength + commentLength;
  }

  return entries;
}

function validatePresetArchive(body: ArrayBuffer): ValidationResult {
  const bytes = new Uint8Array(body);
  if (!isZipBuffer(bytes)) {
    return { ok: false, reason: "Preset payload must be a ZIP archive" };
  }

  let entries: string[];
  try {
    entries = parseZipEntryNames(body);
  } catch (error) {
    return { ok: false, reason: error instanceof Error ? error.message : "Invalid ZIP archive" };
  }

  const fileEntries = entries.filter((entry) => !entry.endsWith("/"));
  if (fileEntries.length === 0) {
    return { ok: false, reason: "Preset archive is empty" };
  }

  const allowedExtensions = new Set([".json", ".wav", ".nam"]);
  let hasJson = false;
  let hasResource = false;

  for (const entry of fileEntries) {
    const extension = getExtension(entry);
    if (!allowedExtensions.has(extension)) {
      return { ok: false, reason: `Unsupported file extension in preset archive: ${extension || "(none)"}` };
    }
    if (extension === ".json") {
      hasJson = true;
    }
    if (extension === ".wav" || extension === ".nam") {
      hasResource = true;
    }
  }

  if (!hasJson) {
    return { ok: false, reason: "Preset archive must include at least one JSON file" };
  }
  if (!hasResource) {
    return { ok: false, reason: "Preset archive must include at least one .wav or .nam file" };
  }

  return { ok: true };
}

function validatePackArchive(body: ArrayBuffer): ValidationResult {
  const bytes = new Uint8Array(body);
  if (!isZipBuffer(bytes)) {
    return { ok: false, reason: "Pack upload must be a ZIP archive" };
  }

  try {
    const entries = parseZipEntryNames(body);
    const fileEntries = entries.filter((entry) => !entry.endsWith("/"));
    if (fileEntries.length === 0) {
      return { ok: false, reason: "Pack archive is empty" };
    }
  } catch (error) {
    return { ok: false, reason: error instanceof Error ? error.message : "Invalid ZIP archive" };
  }

  return { ok: true };
}

function validateJsonDocument(body: ArrayBuffer): ValidationResult {
  try {
    const text = decoder.decode(new Uint8Array(body));
    JSON.parse(text);
  } catch {
    return { ok: false, reason: "Manifest must be valid JSON" };
  }
  return { ok: true };
}

function validateThumbnail(body: ArrayBuffer): ValidationResult {
  const bytes = new Uint8Array(body);
  const isPng = hasSignature(bytes, [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  const isJpeg = hasSignature(bytes, [0xff, 0xd8, 0xff]);
  const isWebp = hasSignature(bytes, [0x52, 0x49, 0x46, 0x46]) && hasSignature(bytes, [0x57, 0x45, 0x42, 0x50], 8);
  if (!isPng && !isJpeg && !isWebp) {
    return { ok: false, reason: "Thumbnail must be PNG, JPEG, or WEBP" };
  }
  return { ok: true };
}

function validatePreviewAudio(body: ArrayBuffer): ValidationResult {
  const bytes = new Uint8Array(body);
  const isWav = hasSignature(bytes, [0x52, 0x49, 0x46, 0x46]) && hasSignature(bytes, [0x57, 0x41, 0x56, 0x45], 8);
  const isMp3 = hasSignature(bytes, [0x49, 0x44, 0x33]) || (bytes.length > 1 && bytes[0] === 0xff && (bytes[1] & 0xe0) === 0xe0);
  const isOgg = hasSignature(bytes, [0x4f, 0x67, 0x67, 0x53]);
  if (!isWav && !isMp3 && !isOgg) {
    return { ok: false, reason: "Preview audio must be WAV, MP3, or OGG" };
  }
  return { ok: true };
}

function validateUploadedContent(kind: UploadKind, body: ArrayBuffer): ValidationResult {
  if (kind === "item_payload") {
    return validatePresetArchive(body);
  }
  if (kind === "pack_zip") {
    return validatePackArchive(body);
  }
  if (kind === "item_manifest") {
    return validateJsonDocument(body);
  }
  if (kind === "thumbnail") {
    return validateThumbnail(body);
  }
  if (kind === "preview_audio") {
    return validatePreviewAudio(body);
  }
  return { ok: false, reason: "Unknown upload kind" };
}

export function uploadRoutes() {
  const app = new Hono<{ Bindings: Env; Variables: { auth?: { userId: string; email: string; role: string; sessionId: string } } }>();

  app.post("/init", requireAuth, async (c) => {
    const body = await safeJson<UploadInitBody>(c.req.raw);
    const kind = body?.kind;
    const mimeType = body?.mimeType?.trim();
    const byteSize = body?.byteSize ?? 0;

    if (!kind || !allowedKinds.has(kind)) {
      return fail(c, "INVALID_KIND", "Invalid upload kind", 422);
    }
    if (!mimeType || byteSize <= 0) {
      return fail(c, "INVALID_METADATA", "mimeType and byteSize are required", 422);
    }

    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const uploadId = randomId("upl");
    const r2Key = `users/${auth.userId}/drafts/${uploadId}/payload.bin`;

    await c.env.DB
      .prepare("INSERT INTO assets (id, owner_user_id, r2_key, kind, mime_type, byte_size, uploaded_at) VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)")
      .bind(uploadId, auth.userId, r2Key, kind, mimeType, byteSize)
      .run();

    return ok(c, {
      uploadId,
      kind,
      mimeType,
      byteSize,
      uploadUrl: `/v1/uploads/${uploadId}`,
      r2Key
    });
  });

  app.put("/:uploadId", requireAuth, async (c) => {
    const uploadId = c.req.param("uploadId");
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const mimeType = c.req.header("content-type") ?? "application/octet-stream";
    const body = await c.req.arrayBuffer();
    if (body.byteLength === 0) {
      return fail(c, "EMPTY_BODY", "Upload payload is empty", 422);
    }

    const asset = await c.env.DB
      .prepare("SELECT id, owner_user_id, kind, byte_size FROM assets WHERE id = ?")
      .bind(uploadId)
      .first<{ id: string; owner_user_id: string; kind: UploadKind; byte_size: number }>();

    if (!asset || asset.owner_user_id !== auth.userId) {
      return fail(c, "NOT_FOUND", "Upload session not found", 404);
    }

    if (Number(asset.byte_size) !== body.byteLength) {
      return fail(c, "INVALID_SIZE", "Uploaded byte size does not match initialized byteSize", 422);
    }

    const validation = validateUploadedContent(asset.kind, body);
    if (!validation.ok) {
      return fail(c, "INVALID_UPLOAD_CONTENT", validation.reason, 422);
    }

    const r2Key = `users/${auth.userId}/drafts/${uploadId}/payload.bin`;
    await c.env.ASSETS.put(r2Key, body, {
      httpMetadata: {
        contentType: mimeType
      }
    });

    await c.env.DB
      .prepare("UPDATE assets SET r2_key = ?, mime_type = ?, byte_size = ?, uploaded_at = CURRENT_TIMESTAMP WHERE id = ?")
      .bind(r2Key, mimeType, body.byteLength, uploadId)
      .run();

    return ok(c, {
      uploadId,
      r2Key,
      byteSize: body.byteLength
    });
  });

  app.post("/complete", requireAuth, async (c) => {
    const body = await safeJson<UploadCompleteBody>(c.req.raw);
    const uploadId = body?.uploadId?.trim();
    if (!uploadId) {
      return fail(c, "INVALID_UPLOAD_ID", "uploadId is required", 422);
    }

    const asset = await c.env.DB
      .prepare("SELECT id, owner_user_id, r2_key, byte_size FROM assets WHERE id = ?")
      .bind(uploadId)
      .first<{ id: string; owner_user_id: string; r2_key: string; byte_size: number }>();

    const auth = c.get("auth");
    if (!asset || !auth || asset.owner_user_id !== auth.userId) {
      return fail(c, "NOT_FOUND", "Upload not found", 404);
    }

    const object = await c.env.ASSETS.head(asset.r2_key);
    if (!object) {
      return fail(c, "MISSING_OBJECT", "Uploaded object not found in storage", 409);
    }

    return ok(c, {
      assetId: asset.id,
      r2Key: asset.r2_key,
      byteSize: asset.byte_size
    });
  });

  return app;
}
