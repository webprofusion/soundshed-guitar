import { Hono } from "hono";
import { fail, ok, safeJson } from "../lib/http";
import { optionalAuth, requireAuth } from "../middleware/session";
import { Env } from "../types/env";
import { randomId } from "../lib/utils";

type CreatePackBody = {
  title?: string;
  description?: string;
  zipAssetId?: string;
  thumbnailAssetId?: string;
};

type UpdatePackBody = Partial<CreatePackBody>;

type SetPackItemsBody = {
  itemIds?: string[];
};

type PackRow = {
  id: string;
  creator_user_id: string;
  title: string;
  moderation_status: "draft" | "pending_review" | "approved" | "rejected";
  config_json: string;
  published_at: string | null;
  created_at: string;
  updated_at: string;
};

type PackConfig = {
  description: string | null;
  zipAssetId: string | null;
  thumbnailAssetId: string | null;
};

function parsePackConfig(configJson: string | null | undefined): PackConfig {
  const defaults: PackConfig = {
    description: null,
    zipAssetId: null,
    thumbnailAssetId: null
  };

  if (!configJson) {
    return defaults;
  }

  try {
    const parsed = JSON.parse(configJson) as Partial<PackConfig>;
    return {
      description: typeof parsed.description === "string" ? parsed.description : null,
      zipAssetId: typeof parsed.zipAssetId === "string" ? parsed.zipAssetId : null,
      thumbnailAssetId: typeof parsed.thumbnailAssetId === "string" ? parsed.thumbnailAssetId : null
    };
  } catch {
    return defaults;
  }
}

function stringifyPackConfig(config: PackConfig): string {
  return JSON.stringify(config);
}

function toPackResponse(pack: PackRow) {
  const config = parsePackConfig(pack.config_json);
  return {
    id: pack.id,
    creatorUserId: pack.creator_user_id,
    title: pack.title,
    description: config.description,
    moderationStatus: pack.moderation_status,
    zipAssetId: config.zipAssetId,
    thumbnailAssetId: config.thumbnailAssetId,
    publishedAt: pack.published_at,
    createdAt: pack.created_at,
    updatedAt: pack.updated_at
  };
}

function downloadFileName(base: string, ext: string): string {
  const normalized = base
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9\-_ ]/g, "")
    .replace(/\s+/g, "-")
    .slice(0, 80);
  const safe = normalized.length > 0 ? normalized : "pack";
  return `${safe}.${ext}`;
}

export function packRoutes() {
  const app = new Hono<{ Bindings: Env; Variables: { auth?: { userId: string; email: string; role: string; sessionId: string } } }>();

  app.get("/", async (c) => {
    const page = Math.max(1, Number.parseInt((c.req.query("page") ?? "1").trim(), 10) || 1);
    const pageSizeRaw = Number.parseInt((c.req.query("pageSize") ?? "24").trim(), 10) || 24;
    const pageSize = Math.min(100, Math.max(1, pageSizeRaw));
    const offset = (page - 1) * pageSize;

    const query = (c.req.query("q") ?? "").trim();

    const params: unknown[] = [];
    let sql = `
          SELECT id, creator_user_id, title, moderation_status,
            config_json, published_at, created_at, updated_at
      FROM packs
      WHERE moderation_status = 'approved'
    `;

    if (query.length > 0) {
      sql += " AND title LIKE ?";
      params.push(`%${query}%`);
    }

    sql += " ORDER BY published_at DESC, updated_at DESC LIMIT ? OFFSET ?";
    params.push(pageSize, offset);

    const rows = await c.env.DB.prepare(sql).bind(...params).all<PackRow>();
    return ok(c, {
      page,
      pageSize,
      packs: rows.results.map(toPackResponse)
    });
  });

  app.get("/me/list", optionalAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return ok(c, { packs: [] });
    }

    const status = (c.req.query("status") ?? "").trim();

    const params: unknown[] = [auth.userId];
    let sql = `
          SELECT id, creator_user_id, title, moderation_status,
            config_json, published_at, created_at, updated_at
      FROM packs
      WHERE creator_user_id = ?
    `;

    if (status.length > 0) {
      sql += " AND moderation_status = ?";
      params.push(status);
    }

    sql += " ORDER BY updated_at DESC LIMIT 200";

    const rows = await c.env.DB.prepare(sql).bind(...params).all<PackRow>();
    return ok(c, { packs: rows.results.map(toPackResponse) });
  });

  app.post("/", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const body = await safeJson<CreatePackBody>(c.req.raw);
    const title = body?.title?.trim();
    if (!title) {
      return fail(c, "INVALID_TITLE", "title is required", 422);
    }

    const packId = randomId("pak");
    const config: PackConfig = {
      description: body?.description?.trim() ?? null,
      zipAssetId: body?.zipAssetId?.trim() ?? null,
      thumbnailAssetId: body?.thumbnailAssetId?.trim() ?? null
    };

    await c.env.DB
      .prepare(
        `INSERT INTO packs (
          id, creator_user_id, title, moderation_status,
          config_json, created_at, updated_at
        ) VALUES (?, ?, ?, 'draft', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)`
      )
      .bind(packId, auth.userId, title, stringifyPackConfig(config))
      .run();

    const created = await c.env.DB
      .prepare(
        `SELECT id, creator_user_id, title, moderation_status,
          config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
      )
      .bind(packId)
      .first<PackRow>();

    return ok(c, { pack: created ? toPackResponse(created) : null }, 201);
  });

  app.patch("/:packId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const packId = c.req.param("packId");
    const existing = await c.env.DB
      .prepare(
      `SELECT id, creator_user_id, title, moderation_status,
        config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
      )
      .bind(packId)
      .first<PackRow>();

    if (!existing) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (existing.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }

    const body = await safeJson<UpdatePackBody>(c.req.raw);
    if (!body) {
      return fail(c, "INVALID_BODY", "Invalid request body", 422);
    }

    const title = body.title !== undefined ? body.title.trim() : existing.title;
    if (!title) {
      return fail(c, "INVALID_TITLE", "title cannot be empty", 422);
    }

    const existingConfig = parsePackConfig(existing.config_json);
    const nextConfig: PackConfig = {
      description: body.description !== undefined ? body.description?.trim() ?? null : existingConfig.description,
      zipAssetId: body.zipAssetId !== undefined ? body.zipAssetId?.trim() ?? null : existingConfig.zipAssetId,
      thumbnailAssetId: body.thumbnailAssetId !== undefined ? body.thumbnailAssetId?.trim() ?? null : existingConfig.thumbnailAssetId
    };

    await c.env.DB
      .prepare(
        `UPDATE packs SET
          title = ?,
          config_json = ?,
          updated_at = CURRENT_TIMESTAMP
         WHERE id = ?`
      )
      .bind(title, stringifyPackConfig(nextConfig), packId)
      .run();

    const updated = await c.env.DB
      .prepare(
        `SELECT id, creator_user_id, title, moderation_status,
          config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
      )
      .bind(packId)
      .first<PackRow>();

    return ok(c, { pack: updated ? toPackResponse(updated) : null });
  });

  app.post("/:packId/items", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const packId = c.req.param("packId");
    const pack = await c.env.DB
      .prepare("SELECT id, creator_user_id FROM packs WHERE id = ?")
      .bind(packId)
      .first<{ id: string; creator_user_id: string }>();

    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }

    const body = await safeJson<SetPackItemsBody>(c.req.raw);
    const itemIds = body?.itemIds ?? [];
    if (!Array.isArray(itemIds)) {
      return fail(c, "INVALID_ITEM_IDS", "itemIds must be an array", 422);
    }

    const normalized = itemIds.map((itemId) => itemId.trim()).filter((itemId) => itemId.length > 0);
    if (normalized.length !== new Set(normalized).size) {
      return fail(c, "DUPLICATE_ITEM_IDS", "itemIds must be unique", 422);
    }

    if (normalized.length > 0) {
      const placeholders = normalized.map(() => "?").join(",");
      const owned = await c.env.DB
        .prepare(`SELECT id FROM items WHERE creator_user_id = ? AND id IN (${placeholders})`)
        .bind(auth.userId, ...normalized)
        .all<{ id: string }>();

      if (owned.results.length !== normalized.length) {
        return fail(c, "INVALID_ITEMS", "All items must exist and belong to the current user", 422);
      }
    }

    await c.env.DB.prepare("DELETE FROM pack_items WHERE pack_id = ?").bind(packId).run();

    for (let index = 0; index < normalized.length; index++) {
      await c.env.DB
        .prepare("INSERT INTO pack_items (pack_id, item_id, sort_order) VALUES (?, ?, ?)")
        .bind(packId, normalized[index], index)
        .run();
    }

    return ok(c, { packId, itemIds: normalized });
  });

  app.post("/:packId/submit", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const packId = c.req.param("packId");
    const pack = await c.env.DB
      .prepare("SELECT id, creator_user_id FROM packs WHERE id = ?")
      .bind(packId)
      .first<{ id: string; creator_user_id: string }>();

    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }

    await c.env.DB
      .prepare("UPDATE packs SET moderation_status = 'pending_review', updated_at = CURRENT_TIMESTAMP WHERE id = ?")
      .bind(packId)
      .run();

    return ok(c, { packId, moderationStatus: "pending_review" });
  });

  app.post("/:packId/publish", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const packId = c.req.param("packId");
    const pack = await c.env.DB
      .prepare("SELECT id, creator_user_id FROM packs WHERE id = ?")
      .bind(packId)
      .first<{ id: string; creator_user_id: string }>();

    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }

    const packItems = await c.env.DB
      .prepare("SELECT COUNT(*) AS total FROM pack_items WHERE pack_id = ?")
      .bind(packId)
      .first<{ total: number }>();

    if (!packItems || Number(packItems.total) <= 0) {
      return fail(c, "EMPTY_PACK", "Cannot publish an empty pack", 422);
    }

    await c.env.DB
      .prepare(
        "UPDATE packs SET moderation_status = 'approved', published_at = COALESCE(published_at, CURRENT_TIMESTAMP), updated_at = CURRENT_TIMESTAMP WHERE id = ?"
      )
      .bind(packId)
      .run();

    return ok(c, { packId, moderationStatus: "approved" });
  });

  app.get("/:packId", optionalAuth, async (c) => {
    const packId = c.req.param("packId");
    const auth = c.get("auth");

    const pack = await c.env.DB
      .prepare(
      `SELECT id, creator_user_id, title, moderation_status,
        config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
      )
      .bind(packId)
      .first<PackRow>();

    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }

    const isOwner = auth?.userId === pack.creator_user_id;
    const isPublic = pack.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }

    const packConfig = parsePackConfig(pack.config_json);

    const packItems = await c.env.DB
      .prepare(
        `SELECT pi.item_id, pi.sort_order, i.title, i.type
         FROM pack_items pi
         JOIN items i ON i.id = pi.item_id
         WHERE pi.pack_id = ?
         ORDER BY pi.sort_order ASC`
      )
      .bind(packId)
      .all<{ item_id: string; sort_order: number; title: string; type: string }>();

    return ok(c, {
      pack: {
        ...toPackResponse(pack),
        thumbnailUrl: packConfig.thumbnailAssetId ? `/v1/packs/${packId}/thumbnail` : null
      },
      items: packItems.results.map((entry) => ({
        itemId: entry.item_id,
        sortOrder: entry.sort_order,
        title: entry.title,
        type: entry.type
      }))
    });
  });

  app.get("/:packId/thumbnail", optionalAuth, async (c) => {
    const packId = c.req.param("packId");
    const auth = c.get("auth");

    const pack = await c.env.DB
      .prepare("SELECT id, creator_user_id, moderation_status, config_json FROM packs WHERE id = ?")
      .bind(packId)
      .first<{
        id: string;
        creator_user_id: string;
        moderation_status: string;
        config_json: string | null;
      }>();

    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }

    const isOwner = auth?.userId === pack.creator_user_id;
    const isPublic = pack.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const config = parsePackConfig(pack.config_json);
    if (!config.thumbnailAssetId) {
      return fail(c, "MISSING_THUMBNAIL", "Pack thumbnail is not available", 404);
    }

    const asset = await c.env.DB
      .prepare("SELECT r2_key, mime_type FROM assets WHERE id = ?")
      .bind(config.thumbnailAssetId)
      .first<{ r2_key: string; mime_type: string }>();

    if (!asset) {
      return fail(c, "ASSET_NOT_FOUND", "Pack thumbnail asset not found", 404);
    }

    const object = await c.env.ASSETS.get(asset.r2_key);
    if (!object || !object.body) {
      return fail(c, "OBJECT_NOT_FOUND", "Pack thumbnail object not found", 404);
    }

    const contentType = object.httpMetadata?.contentType ?? asset.mime_type ?? "application/octet-stream";
    return new Response(object.body, {
      status: 200,
      headers: {
        "content-type": contentType
      }
    });
  });

  app.get("/:packId/download", optionalAuth, async (c) => {
    const packId = c.req.param("packId");
    const auth = c.get("auth");

    const pack = await c.env.DB
      .prepare("SELECT id, creator_user_id, title, moderation_status, config_json FROM packs WHERE id = ?")
      .bind(packId)
      .first<{
        id: string;
        creator_user_id: string;
        title: string;
        moderation_status: string;
        config_json: string | null;
      }>();

    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }

    const isOwner = auth?.userId === pack.creator_user_id;
    const isPublic = pack.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const config = parsePackConfig(pack.config_json);
    if (!config.zipAssetId) {
      return fail(c, "MISSING_ARCHIVE", "Pack archive is not available", 409);
    }

    const asset = await c.env.DB
      .prepare("SELECT r2_key, mime_type FROM assets WHERE id = ?")
      .bind(config.zipAssetId)
      .first<{ r2_key: string; mime_type: string }>();

    if (!asset) {
      return fail(c, "ASSET_NOT_FOUND", "Pack archive asset not found", 404);
    }

    const object = await c.env.ASSETS.get(asset.r2_key);
    if (!object || !object.body) {
      return fail(c, "OBJECT_NOT_FOUND", "Pack archive object not found", 404);
    }

    await c.env.DB
      .prepare("INSERT INTO downloads (id, user_id, item_id, pack_id, created_at) VALUES (?, ?, NULL, ?, CURRENT_TIMESTAMP)")
      .bind(randomId("dwl"), auth?.userId ?? null, packId)
      .run();

    const contentType = object.httpMetadata?.contentType ?? asset.mime_type ?? "application/zip";
    const fileName = downloadFileName(pack.title, "zip");

    return new Response(object.body, {
      status: 200,
      headers: {
        "content-type": contentType,
        "content-disposition": `attachment; filename=\"${fileName}\"`
      }
    });
  });

  app.delete("/:packId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const packId = c.req.param("packId");
    const pack = await c.env.DB
      .prepare("SELECT id, creator_user_id FROM packs WHERE id = ?")
      .bind(packId)
      .first<{ id: string; creator_user_id: string }>();

    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }

    await c.env.DB.prepare("DELETE FROM pack_items WHERE pack_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM featured_row_items WHERE pack_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM downloads WHERE pack_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM reports WHERE pack_id = ?").bind(packId).run();
    await c.env.DB
      .prepare("DELETE FROM moderation_actions WHERE target_type = 'pack' AND target_id = ?")
      .bind(packId)
      .run();
    await c.env.DB.prepare("DELETE FROM packs WHERE id = ?").bind(packId).run();

    return ok(c, { packId, deleted: true });
  });

  return app;
}
