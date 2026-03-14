import { Hono } from "hono";
import { fail, ok, safeJson } from "../lib/http";
import { hasToneSharingPublishConsent } from "../lib/shareConsent";
import { optionalAuth, requireAuth } from "../middleware/session";
import { Env } from "../types/env";
import { randomId } from "../lib/utils";

type ItemType = "preset" | "blend" | "layout" | "composite" | "combo";
type ItemVisibility = "public" | "unlisted" | "private";

type CreateItemBody = {
  type?: ItemType;
  title?: string;
  description?: string;
  visibility?: ItemVisibility;
  tags?: string[];
  appMinVersion?: string;
  appMaxVersion?: string;
  payloadAssetId?: string;
  privatePayloadAssetId?: string;
  manifestAssetId?: string;
  thumbnailAssetId?: string;
  previewAssetId?: string;
};

type UpdateItemBody = Partial<CreateItemBody>;

const allowedTypes = new Set<ItemType>(["preset", "blend", "layout", "composite", "combo"]);
const allowedVisibility = new Set<ItemVisibility>(["public", "unlisted", "private"]);

type ItemRow = {
  id: string;
  creator_user_id: string;
  type: ItemType;
  title: string;
  moderation_status: "draft" | "pending_review" | "approved" | "rejected";
  config_json: string;
  published_at: string | null;
  created_at: string;
  updated_at: string;
};

type ItemConfig = {
  description: string | null;
  visibility: ItemVisibility;
  tags: string[] | null;
  appMinVersion: string | null;
  appMaxVersion: string | null;
  payloadAssetId: string | null;
  privatePayloadAssetId: string | null;
  manifestAssetId: string | null;
  thumbnailAssetId: string | null;
  previewAssetId: string | null;
};

function parseItemConfig(configJson: string | null | undefined): ItemConfig {
  const defaults: ItemConfig = {
    description: null,
    visibility: "public",
    tags: null,
    appMinVersion: null,
    appMaxVersion: null,
    payloadAssetId: null,
    privatePayloadAssetId: null,
    manifestAssetId: null,
    thumbnailAssetId: null,
    previewAssetId: null
  };

  if (!configJson) {
    return defaults;
  }

  try {
    const parsed = JSON.parse(configJson) as Partial<ItemConfig>;
    const visibility = parsed.visibility;
    const rawTags = parsed.tags;
    return {
      description: typeof parsed.description === "string" ? parsed.description : null,
      visibility: visibility && allowedVisibility.has(visibility) ? visibility : "public",
      tags: Array.isArray(rawTags) ? rawTags.filter((t): t is string => typeof t === "string") : null,
      appMinVersion: typeof parsed.appMinVersion === "string" ? parsed.appMinVersion : null,
      appMaxVersion: typeof parsed.appMaxVersion === "string" ? parsed.appMaxVersion : null,
      payloadAssetId: typeof parsed.payloadAssetId === "string" ? parsed.payloadAssetId : null,
      privatePayloadAssetId: typeof parsed.privatePayloadAssetId === "string" ? parsed.privatePayloadAssetId : null,
      manifestAssetId: typeof parsed.manifestAssetId === "string" ? parsed.manifestAssetId : null,
      thumbnailAssetId: typeof parsed.thumbnailAssetId === "string" ? parsed.thumbnailAssetId : null,
      previewAssetId: typeof parsed.previewAssetId === "string" ? parsed.previewAssetId : null
    };
  } catch {
    return defaults;
  }
}

function stringifyItemConfig(config: ItemConfig): string {
  return JSON.stringify(config);
}

function toItemResponse(item: ItemRow) {
  const config = parseItemConfig(item.config_json);
  return {
    id: item.id,
    creatorUserId: item.creator_user_id,
    type: item.type,
    title: item.title,
    description: config.description,
    tags: config.tags,
    visibility: config.visibility,
    moderationStatus: item.moderation_status,
    appMinVersion: config.appMinVersion,
    appMaxVersion: config.appMaxVersion,
    payloadAssetId: config.payloadAssetId,
    privatePayloadAssetId: config.privatePayloadAssetId,
    manifestAssetId: config.manifestAssetId,
    thumbnailAssetId: config.thumbnailAssetId,
    previewAssetId: config.previewAssetId,
    publishedAt: item.published_at,
    createdAt: item.created_at,
    updatedAt: item.updated_at
  };
}

function downloadFileName(base: string, ext: string): string {
  const normalized = base
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9\-_ ]/g, "")
    .replace(/\s+/g, "-")
    .slice(0, 80);
  const safe = normalized.length > 0 ? normalized : "preset";
  return `${safe}.${ext}`;
}

export function itemRoutes() {
  const app = new Hono<{ Bindings: Env; Variables: { auth?: { userId: string; email: string; role: string; sessionId: string } } }>();

  app.get("/", async (c) => {
    const page = Math.max(1, Number.parseInt((c.req.query("page") ?? "1").trim(), 10) || 1);
    const pageSizeRaw = Number.parseInt((c.req.query("pageSize") ?? "24").trim(), 10) || 24;
    const pageSize = Math.min(100, Math.max(1, pageSizeRaw));
    const offset = (page - 1) * pageSize;

    const type = (c.req.query("type") ?? "").trim();
    const query = (c.req.query("q") ?? "").trim();
    const taxonomy = (c.req.query("taxonomy") ?? "").trim();

    const params: unknown[] = [];
    let sql = `
      SELECT DISTINCT i.id, i.creator_user_id, i.type, i.title, i.moderation_status, i.config_json,
             i.published_at, i.created_at, i.updated_at
      FROM items i
      LEFT JOIN item_taxonomies it ON it.item_id = i.id
      LEFT JOIN taxonomies t ON t.id = it.taxonomy_id
      WHERE i.moderation_status = 'approved'
    `;

    if (type.length > 0) {
      sql += " AND i.type = ?";
      params.push(type);
    }
    if (query.length > 0) {
      sql += " AND i.title LIKE ?";
      params.push(`%${query}%`);
    }
    if (taxonomy.length > 0) {
      sql += " AND t.slug = ?";
      params.push(taxonomy);
    }

    sql += " ORDER BY i.published_at DESC, i.updated_at DESC LIMIT ? OFFSET ?";
    params.push(pageSize, offset);

    const rows = await c.env.DB.prepare(sql).bind(...params).all<ItemRow>();
    return ok(c, {
      page,
      pageSize,
      items: rows.results.map(toItemResponse)
    });
  });

  app.get("/me/list", optionalAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return ok(c, { items: [] });
    }

    const status = (c.req.query("status") ?? "").trim();
    const type = (c.req.query("type") ?? "").trim();

    const params: unknown[] = [auth.userId];
    let sql = `
      SELECT id, creator_user_id, type, title, moderation_status, config_json,
             published_at, created_at, updated_at
      FROM items
      WHERE creator_user_id = ?
    `;

    if (status.length > 0) {
      sql += " AND moderation_status = ?";
      params.push(status);
    }
    if (type.length > 0) {
      sql += " AND type = ?";
      params.push(type);
    }

    sql += " ORDER BY updated_at DESC LIMIT 200";

    const rows = await c.env.DB.prepare(sql).bind(...params).all<ItemRow>();
    return ok(c, { items: rows.results.map(toItemResponse) });
  });

  app.post("/", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const body = await safeJson<CreateItemBody>(c.req.raw);
    const type = body?.type;
    const title = body?.title?.trim();
    const visibility = body?.visibility ?? "public";

    if (!type || !allowedTypes.has(type)) {
      return fail(c, "INVALID_TYPE", "Invalid item type", 422);
    }
    if (!title) {
      return fail(c, "INVALID_TITLE", "title is required", 422);
    }
    if (!allowedVisibility.has(visibility)) {
      return fail(c, "INVALID_VISIBILITY", "Invalid visibility", 422);
    }

    const itemId = randomId("itm");
    const rawTags = body?.tags;
    const config: ItemConfig = {
      description: body?.description?.trim() ?? null,
      visibility,
      tags: Array.isArray(rawTags) ? rawTags.filter((t): t is string => typeof t === "string").slice(0, 20) : null,
      appMinVersion: body?.appMinVersion?.trim() ?? null,
      appMaxVersion: body?.appMaxVersion?.trim() ?? null,
      payloadAssetId: body?.payloadAssetId?.trim() ?? null,
      privatePayloadAssetId: body?.privatePayloadAssetId?.trim() ?? null,
      manifestAssetId: body?.manifestAssetId?.trim() ?? null,
      thumbnailAssetId: body?.thumbnailAssetId?.trim() ?? null,
      previewAssetId: body?.previewAssetId?.trim() ?? null
    };

    await c.env.DB
      .prepare(
        `INSERT INTO items (
          id, creator_user_id, type, title, moderation_status, config_json,
          created_at, updated_at
        ) VALUES (?, ?, ?, ?, 'draft', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)`
      )
      .bind(
        itemId,
        auth.userId,
        type,
        title,
        stringifyItemConfig(config)
      )
      .run();

    const created = await c.env.DB
      .prepare(
        `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
      )
      .bind(itemId)
      .first<ItemRow>();

    return ok(c, { item: created ? toItemResponse(created) : null }, 201);
  });

  app.patch("/:itemId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    const existing = await c.env.DB
      .prepare(
        `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
      )
      .bind(itemId)
      .first<ItemRow>();

    if (!existing) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (existing.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }

    const body = await safeJson<UpdateItemBody>(c.req.raw);
    if (!body) {
      return fail(c, "INVALID_BODY", "Invalid request body", 422);
    }

    const type = body.type ?? existing.type;
    const title = body.title !== undefined ? body.title.trim() : existing.title;
    const existingConfig = parseItemConfig(existing.config_json);
    const visibility = body.visibility ?? existingConfig.visibility;

    if (!allowedTypes.has(type)) {
      return fail(c, "INVALID_TYPE", "Invalid item type", 422);
    }
    if (!title) {
      return fail(c, "INVALID_TITLE", "title cannot be empty", 422);
    }
    if (!allowedVisibility.has(visibility)) {
      return fail(c, "INVALID_VISIBILITY", "Invalid visibility", 422);
    }

    const nextConfig: ItemConfig = {
      description: body.description !== undefined ? body.description?.trim() ?? null : existingConfig.description,
      visibility,
      tags: body.tags !== undefined
        ? (Array.isArray(body.tags) ? body.tags.filter((t): t is string => typeof t === "string").slice(0, 20) : null)
        : existingConfig.tags,
      appMinVersion: body.appMinVersion !== undefined ? body.appMinVersion?.trim() ?? null : existingConfig.appMinVersion,
      appMaxVersion: body.appMaxVersion !== undefined ? body.appMaxVersion?.trim() ?? null : existingConfig.appMaxVersion,
      payloadAssetId: body.payloadAssetId !== undefined ? body.payloadAssetId?.trim() ?? null : existingConfig.payloadAssetId,
      privatePayloadAssetId: body.privatePayloadAssetId !== undefined ? body.privatePayloadAssetId?.trim() ?? null : existingConfig.privatePayloadAssetId,
      manifestAssetId: body.manifestAssetId !== undefined ? body.manifestAssetId?.trim() ?? null : existingConfig.manifestAssetId,
      thumbnailAssetId: body.thumbnailAssetId !== undefined ? body.thumbnailAssetId?.trim() ?? null : existingConfig.thumbnailAssetId,
      previewAssetId: body.previewAssetId !== undefined ? body.previewAssetId?.trim() ?? null : existingConfig.previewAssetId
    };

    await c.env.DB
      .prepare(
        `UPDATE items SET
          type = ?,
          title = ?,
          config_json = ?,
          updated_at = CURRENT_TIMESTAMP
         WHERE id = ?`
      )
      .bind(
        type,
        title,
        stringifyItemConfig(nextConfig),
        itemId
      )
      .run();

    const updated = await c.env.DB
      .prepare(
        `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
      )
      .bind(itemId)
      .first<ItemRow>();

    return ok(c, { item: updated ? toItemResponse(updated) : null });
  });

  app.post("/:itemId/submit", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    const item = await c.env.DB
      .prepare("SELECT id, creator_user_id, moderation_status FROM items WHERE id = ?")
      .bind(itemId)
      .first<{ id: string; creator_user_id: string; moderation_status: string }>();

    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (item.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }

    await c.env.DB
      .prepare("UPDATE items SET moderation_status = 'pending_review', updated_at = CURRENT_TIMESTAMP WHERE id = ?")
      .bind(itemId)
      .run();

    return ok(c, { itemId, moderationStatus: "pending_review" });
  });

  app.post("/:itemId/publish", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    const item = await c.env.DB
      .prepare("SELECT id, creator_user_id, type, config_json FROM items WHERE id = ?")
      .bind(itemId)
      .first<{ id: string; creator_user_id: string; type: ItemType; config_json: string | null }>();

    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (item.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }

    const hasConsent = item.type !== "preset" || await hasToneSharingPublishConsent(c.env.DB, auth.userId);
    if (!hasConsent) {
      return fail(c, "CONSENT_REQUIRED", "Accept the tone sharing consent before publishing presets", 409);
    }

    await c.env.DB
      .prepare(
        "UPDATE items SET moderation_status = 'approved', published_at = COALESCE(published_at, CURRENT_TIMESTAMP), updated_at = CURRENT_TIMESTAMP WHERE id = ?"
      )
      .bind(itemId)
      .run();

    return ok(c, { itemId, moderationStatus: "approved" });
  });

  app.get("/:itemId", optionalAuth, async (c) => {
    const itemId = c.req.param("itemId");
    const auth = c.get("auth");

    const item = await c.env.DB
      .prepare(
        `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
      )
      .bind(itemId)
      .first<ItemRow>();

    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }

    const isOwner = auth?.userId === item.creator_user_id;
    const isPublic = item.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }

    return ok(c, { item: toItemResponse(item) });
  });

  app.get("/:itemId/download", optionalAuth, async (c) => {
    const itemId = c.req.param("itemId");
    const auth = c.get("auth");

    const item = await c.env.DB
      .prepare("SELECT id, creator_user_id, title, moderation_status, config_json FROM items WHERE id = ?")
      .bind(itemId)
      .first<{
        id: string;
        creator_user_id: string;
        title: string;
        moderation_status: string;
        config_json: string | null;
      }>();

    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }

    const isOwner = auth?.userId === item.creator_user_id;
    const isPublic = item.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    const config = parseItemConfig(item.config_json);
    if (!config.payloadAssetId) {
      return fail(c, "MISSING_PAYLOAD", "Item payload is not available", 409);
    }

    const asset = await c.env.DB
      .prepare("SELECT r2_key, mime_type FROM assets WHERE id = ?")
      .bind(config.payloadAssetId)
      .first<{ r2_key: string; mime_type: string }>();

    if (!asset) {
      return fail(c, "ASSET_NOT_FOUND", "Payload asset not found", 404);
    }

    const object = await c.env.ASSETS.get(asset.r2_key);
    if (!object || !object.body) {
      return fail(c, "OBJECT_NOT_FOUND", "Payload object not found", 404);
    }

    await c.env.DB
      .prepare("INSERT INTO downloads (id, user_id, item_id, pack_id, created_at) VALUES (?, ?, ?, NULL, CURRENT_TIMESTAMP)")
      .bind(randomId("dwl"), auth?.userId ?? null, itemId)
      .run();

    const contentType = object.httpMetadata?.contentType ?? asset.mime_type ?? "application/octet-stream";
    const fileName = downloadFileName(item.title, "preset");

    return new Response(object.body, {
      status: 200,
      headers: {
        "content-type": contentType,
        "content-disposition": `attachment; filename=\"${fileName}\"`
      }
    });
  });

  app.delete("/:itemId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    const item = await c.env.DB
      .prepare("SELECT id, creator_user_id FROM items WHERE id = ?")
      .bind(itemId)
      .first<{ id: string; creator_user_id: string }>();

    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (item.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }

    await c.env.DB.prepare("DELETE FROM pack_items WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM featured_row_items WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM favorites WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM ratings WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM item_taxonomies WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM downloads WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM reports WHERE item_id = ?").bind(itemId).run();
    await c.env.DB
      .prepare("DELETE FROM moderation_actions WHERE target_type = 'item' AND target_id = ?")
      .bind(itemId)
      .run();
    await c.env.DB.prepare("DELETE FROM items WHERE id = ?").bind(itemId).run();

    return ok(c, { itemId, deleted: true });
  });

  return app;
}
