import { Hono } from "hono";
import { sendToneSharingModerationNotification } from "../lib/email";
import { fail, ok, safeJson } from "../lib/http";
import { hasToneSharingPublishConsent } from "../lib/shareConsent";
import { optionalAuth, requireAuth } from "../middleware/session";
import { Env } from "../types/env";
import { randomId } from "../lib/utils";
import { allowedItemVisibilities, parseItemConfig, stringifyItemConfig, type ItemConfig, type ItemVisibility } from "../lib/content-config";

type ItemType = "preset" | "blend" | "layout" | "composite" | "combo";

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

type AuthContext = { userId: string; email: string; role: string; sessionId: string };

type CreatorRow = {
  id: string;
  email: string;
  display_name: string | null;
};

type ItemStatsRow = {
  favoriteCount: number;
  ratingCount: number;
  averageRating: number | null;
};

type ItemUserStateRow = {
  isFavorite: number | null;
  rating: number | null;
};

type ModerateItemBody = {
  action?: "approve" | "reject";
  notes?: string;
};

async function loadCreator(db: D1Database, creatorUserId: string): Promise<CreatorRow | null> {
  return db.prepare(
    `SELECT u.id, u.email, u.display_name
     FROM users u
     WHERE u.id = ?`
  ).bind(creatorUserId).first<CreatorRow>();
}

async function loadItemStats(db: D1Database, itemId: string): Promise<ItemStatsRow> {
  const favorites = await db.prepare(
    `SELECT COUNT(*) AS favoriteCount FROM favorites WHERE item_id = ?`
  ).bind(itemId).first<{ favoriteCount: number | null }>();
  const ratings = await db.prepare(
    `SELECT COUNT(*) AS ratingCount, AVG(score) AS averageRating FROM ratings WHERE item_id = ?`
  ).bind(itemId).first<{ ratingCount: number | null; averageRating: number | null }>();

  return {
    favoriteCount: Number(favorites?.favoriteCount ?? 0),
    ratingCount: Number(ratings?.ratingCount ?? 0),
    averageRating: ratings?.averageRating == null ? null : Number(ratings.averageRating),
  };
}

async function loadItemUserState(db: D1Database, itemId: string, userId: string): Promise<ItemUserStateRow> {
  const row = await db.prepare(
    `SELECT
       EXISTS(SELECT 1 FROM favorites WHERE user_id = ? AND item_id = ?) AS isFavorite,
       (SELECT score FROM ratings WHERE user_id = ? AND item_id = ?) AS rating`
  ).bind(userId, itemId, userId, itemId).first<ItemUserStateRow>();

  return {
    isFavorite: row?.isFavorite ?? 0,
    rating: row?.rating ?? null,
  };
}

async function toItemResponse(
  db: D1Database,
  item: ItemRow,
  auth?: AuthContext,
  options: { includeCreatorEmail?: boolean } = {}
) {
  const config = parseItemConfig(item.config_json);
  const [creator, stats, userState] = await Promise.all([
    loadCreator(db, item.creator_user_id),
    loadItemStats(db, item.id),
    auth ? loadItemUserState(db, item.id, auth.userId) : Promise.resolve<ItemUserStateRow | null>(null),
  ]);
  const isOwnerOrAdmin = Boolean(auth && (auth.userId === item.creator_user_id || auth.role === "admin"));

  const includeCreatorEmail = options.includeCreatorEmail === true;

  return {
    id: item.id,
    creatorUserId: item.creator_user_id,
    creatorDisplayName: creator?.display_name ?? null,
    type: item.type,
    title: item.title,
    description: config.description,
    tags: config.tags,
    visibility: config.visibility,
    moderationStatus: item.moderation_status,
    appMinVersion: config.appMinVersion,
    appMaxVersion: config.appMaxVersion,
    favoriteCount: stats.favoriteCount,
    ratingCount: stats.ratingCount,
    averageRating: stats.averageRating,
    currentUserFavorite: Boolean(userState?.isFavorite),
    currentUserRating: userState?.rating == null ? null : Number(userState.rating),
    publishedAt: item.published_at,
    createdAt: item.created_at,
    updatedAt: item.updated_at,
    // Compatibility fields retained for existing clients; sensitive values are owner/admin-only.
    ...(includeCreatorEmail && isOwnerOrAdmin ? { creatorEmail: creator?.email ?? null } : {}),
    payloadAssetId: isOwnerOrAdmin ? config.payloadAssetId : null,
    privatePayloadAssetId: isOwnerOrAdmin ? config.privatePayloadAssetId : null,
    manifestAssetId: isOwnerOrAdmin ? config.manifestAssetId : null,
    thumbnailAssetId: isOwnerOrAdmin ? config.thumbnailAssetId : null,
    previewAssetId: isOwnerOrAdmin ? config.previewAssetId : null,
  };
}

function requireAdmin(auth: AuthContext | undefined) {
  return Boolean(auth && auth.role === "admin");
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

  app.get("/", optionalAuth, async (c) => {
    const auth = c.get("auth");
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
      items: await Promise.all(rows.results.map((item) => toItemResponse(c.env.DB, item, auth, { includeCreatorEmail: false })))
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
    return ok(c, { items: await Promise.all(rows.results.map((item) => toItemResponse(c.env.DB, item, auth, { includeCreatorEmail: false }))) });
  });

  app.get("/pending/list", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!requireAdmin(auth)) {
      return fail(c, "FORBIDDEN", "Admin access required", 403);
    }

    const rows = await c.env.DB.prepare(
      `SELECT id, creator_user_id, type, title, moderation_status, config_json,
              published_at, created_at, updated_at
       FROM items
       WHERE moderation_status = 'pending_review'
       ORDER BY updated_at ASC
       LIMIT 200`
    ).all<ItemRow>();

    return ok(c, { items: await Promise.all(rows.results.map((item) => toItemResponse(c.env.DB, item, auth, { includeCreatorEmail: false }))) });
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
    if (!allowedItemVisibilities.has(visibility)) {
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

    return ok(c, { item: created ? await toItemResponse(c.env.DB, created, auth) : null }, 201);
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
    if (!allowedItemVisibilities.has(visibility)) {
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

    return ok(c, { item: updated ? await toItemResponse(c.env.DB, updated, auth) : null });
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
      .prepare("SELECT id, creator_user_id, type, title, config_json FROM items WHERE id = ?")
      .bind(itemId)
      .first<{ id: string; creator_user_id: string; type: ItemType; title: string; config_json: string | null }>();

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
        "UPDATE items SET moderation_status = 'pending_review', updated_at = CURRENT_TIMESTAMP WHERE id = ?"
      )
      .bind(itemId)
      .run();

    try {
      await sendToneSharingModerationNotification(c.env, {
        targetType: "item",
        targetId: itemId,
        title: item.title,
        creatorEmail: auth.email,
        creatorUserId: auth.userId,
      });
    } catch (error) {
      console.error("Failed to send tone sharing moderation notification", error);
    }

    return ok(c, { itemId, moderationStatus: "pending_review" });
  });

  app.post("/:itemId/moderate", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!requireAdmin(auth)) {
      return fail(c, "FORBIDDEN", "Admin access required", 403);
    }

    const itemId = c.req.param("itemId");
    const body = await safeJson<ModerateItemBody>(c.req.raw);
    const action = body?.action;
    const notes = body?.notes?.trim() ?? null;
    if (action !== "approve" && action !== "reject") {
      return fail(c, "INVALID_ACTION", "Action must be approve or reject", 422);
    }

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

    const nextStatus = action === "approve" ? "approved" : "rejected";
    await c.env.DB
      .prepare(
        `UPDATE items SET
           moderation_status = ?,
           published_at = CASE WHEN ? = 'approved' THEN COALESCE(published_at, CURRENT_TIMESTAMP) ELSE published_at END,
           updated_at = CURRENT_TIMESTAMP
         WHERE id = ?`
      )
      .bind(nextStatus, nextStatus, itemId)
      .run();

    await c.env.DB.prepare(
      `INSERT INTO moderation_actions (id, target_type, target_id, action, actor_user_id, notes)
       VALUES (?, 'item', ?, ?, ?, ?)`
    ).bind(randomId("mod"), itemId, action, auth!.userId, notes).run();

    const updated = await c.env.DB
      .prepare(
        `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
      )
      .bind(itemId)
      .first<ItemRow>();

    return ok(c, { item: updated ? await toItemResponse(c.env.DB, updated, auth) : null });
  });

  app.put("/:itemId/favorite", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    const item = await c.env.DB.prepare("SELECT id FROM items WHERE id = ? AND moderation_status = 'approved'").bind(itemId).first<{ id: string }>();
    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }

    await c.env.DB.prepare(
      `INSERT INTO favorites (user_id, item_id, created_at)
       VALUES (?, ?, CURRENT_TIMESTAMP)
       ON CONFLICT(user_id, item_id) DO NOTHING`
    ).bind(auth.userId, itemId).run();

    return ok(c, { itemId, favorite: true });
  });

  app.delete("/:itemId/favorite", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    await c.env.DB.prepare("DELETE FROM favorites WHERE user_id = ? AND item_id = ?").bind(auth.userId, itemId).run();
    return ok(c, { itemId, favorite: false });
  });

  app.put("/:itemId/rating", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    const body = await safeJson<{ rating?: number }>(c.req.raw);
    const rating = typeof body?.rating === "number" ? Math.round(body.rating) : NaN;
    if (!Number.isFinite(rating) || rating < 1 || rating > 5) {
      return fail(c, "INVALID_RATING", "Rating must be an integer between 1 and 5", 422);
    }

    const item = await c.env.DB.prepare("SELECT id FROM items WHERE id = ? AND moderation_status = 'approved'").bind(itemId).first<{ id: string }>();
    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }

    await c.env.DB.prepare(
      `INSERT INTO ratings (user_id, item_id, score, created_at, updated_at)
       VALUES (?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
       ON CONFLICT(user_id, item_id) DO UPDATE SET
         score = excluded.score,
         updated_at = CURRENT_TIMESTAMP`
    ).bind(auth.userId, itemId, rating).run();

    return ok(c, { itemId, rating });
  });

  app.delete("/:itemId/rating", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const itemId = c.req.param("itemId");
    await c.env.DB.prepare("DELETE FROM ratings WHERE user_id = ? AND item_id = ?").bind(auth.userId, itemId).run();
    return ok(c, { itemId, rating: null });
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

    return ok(c, { item: await toItemResponse(c.env.DB, item, auth) });
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
