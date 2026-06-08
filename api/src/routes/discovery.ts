import { Hono } from "hono";
import { parseItemConfig, parsePackConfig } from "../lib/content-config";
import { ok } from "../lib/http";
import { Env } from "../types/env";

type RowItem = {
  id: string;
  kind: "item" | "pack";
  title: string;
  type: string | null;
  description?: string | null;
  tags?: string[] | null;
  thumbnailUrl?: string | null;
  thumbnailAssetId?: string | null;
};

export function discoveryRoutes() {
  const app = new Hono<{ Bindings: Env }>();

  app.get("/home", async (c) => {
    const rows = await c.env.DB.prepare(
      "SELECT id, slug, title, sort_order FROM featured_rows WHERE active = 1 ORDER BY sort_order ASC LIMIT 12"
    ).all<{ id: string; slug: string; title: string; sort_order: number }>();

    const resultRows: Array<{ id: string; slug: string; title: string; items: RowItem[] }> = [];
    for (const row of rows.results) {
      const rowItems = await c.env.DB.prepare(
        `SELECT fri.item_id, fri.pack_id, i.title AS item_title, i.type AS item_type, i.config_json AS item_config_json, p.title AS pack_title, p.config_json AS pack_config_json
         FROM featured_row_items fri
         LEFT JOIN items i ON i.id = fri.item_id
         LEFT JOIN packs p ON p.id = fri.pack_id
         WHERE fri.row_id = ?
         ORDER BY fri.sort_order ASC
         LIMIT 40`
      )
        .bind(row.id)
        .all<{
          item_id: string | null;
          pack_id: string | null;
          item_title: string | null;
          item_type: string | null;
          item_config_json: string | null;
          pack_title: string | null;
          pack_config_json: string | null;
        }>();

      const mappedItems: RowItem[] = rowItems.results.map((entry) => {
        if (entry.item_id) {
          const config = parseItemConfig(entry.item_config_json);
          return {
            id: entry.item_id,
            kind: "item",
            title: entry.item_title ?? "Untitled",
            type: entry.item_type,
            description: config.description,
            tags: config.tags
          };
        }
        const config = parsePackConfig(entry.pack_config_json);
        const packId = entry.pack_id ?? "";
        return {
          id: packId,
          kind: "pack",
          title: entry.pack_title ?? "Untitled Pack",
          type: null,
          thumbnailUrl: packId && config.thumbnailAssetId ? `/v1/packs/${packId}/thumbnail` : null,
          thumbnailAssetId: config.thumbnailAssetId ? "legacy_available" : null,
        };
      });

      resultRows.push({
        id: row.id,
        slug: row.slug,
        title: row.title,
        items: mappedItems
      });
    }

    if (resultRows.length === 0) {
      const latestItems = await c.env.DB
        .prepare(
          `SELECT id, title, type, config_json
           FROM items
           WHERE moderation_status = 'approved'
           ORDER BY published_at DESC, updated_at DESC
           LIMIT 40`
        )
        .all<{ id: string; title: string; type: string; config_json: string | null }>();

      const latestPacks = await c.env.DB
        .prepare(
          `SELECT id, title, config_json
           FROM packs
           WHERE moderation_status = 'approved'
           ORDER BY published_at DESC, updated_at DESC
           LIMIT 20`
        )
        .all<{ id: string; title: string; config_json: string | null }>();

      if (latestItems.results.length > 0) {
        resultRows.push({
          id: "fallback_latest_items",
          slug: "latest-items",
          title: "Latest Presets",
          items: latestItems.results.map((item) => {
            const config = parseItemConfig(item.config_json);
            return {
              id: item.id,
              kind: "item" as const,
              title: item.title,
              type: item.type,
              description: config.description,
              tags: config.tags
            };
          })
        });
      }

      if (latestPacks.results.length > 0) {
        resultRows.push({
          id: "fallback_latest_packs",
          slug: "latest-packs",
          title: "Latest Packs",
          items: latestPacks.results.map((pack) => {
            const config = parsePackConfig(pack.config_json);
            return {
              id: pack.id,
              kind: "pack" as const,
              title: pack.title,
              type: null,
              thumbnailUrl: config.thumbnailAssetId ? `/v1/packs/${pack.id}/thumbnail` : null,
              thumbnailAssetId: config.thumbnailAssetId ? "legacy_available" : null,
            };
          })
        });
      }
    }

    const payload = {
      generatedAt: new Date().toISOString(),
      rows: resultRows
    };

    return ok(c, payload);
  });

  app.get("/search", async (c) => {
    const query = (c.req.query("q") ?? "").trim();
    const type = (c.req.query("type") ?? "").trim();
    const taxonomy = (c.req.query("taxonomy") ?? "").trim();

    const params: unknown[] = [];
    let sql = `
      SELECT DISTINCT i.id, i.title, i.type, i.moderation_status, i.published_at
      FROM items i
      LEFT JOIN item_taxonomies it ON it.item_id = i.id
      LEFT JOIN taxonomies t ON t.id = it.taxonomy_id
      WHERE i.moderation_status = 'approved'
    `;

    if (query.length > 0) {
      sql += " AND i.title LIKE ?";
      params.push(`%${query}%`);
    }
    if (type.length > 0) {
      sql += " AND i.type = ?";
      params.push(type);
    }
    if (taxonomy.length > 0) {
      sql += " AND t.slug = ?";
      params.push(taxonomy);
    }

    sql += " ORDER BY i.published_at DESC LIMIT 100";

    const statement = c.env.DB.prepare(sql).bind(...params);
    const items = await statement.all<{
      id: string;
      title: string;
      type: string;
      moderation_status: string;
      published_at: string | null;
    }>();

    return ok(c, { items: items.results });
  });

  app.get("/rows/:slug", async (c) => {
    const slug = c.req.param("slug");

    const row = await c.env.DB
      .prepare("SELECT id, slug, title FROM featured_rows WHERE slug = ? AND active = 1")
      .bind(slug)
      .first<{ id: string; slug: string; title: string }>();

    if (!row) {
      return ok(c, { row: null, items: [] });
    }

    const rowItems = await c.env.DB
      .prepare(
        `SELECT fri.item_id, fri.pack_id, fri.sort_order, i.title AS item_title, i.type AS item_type, p.title AS pack_title
         FROM featured_row_items fri
         LEFT JOIN items i ON i.id = fri.item_id
         LEFT JOIN packs p ON p.id = fri.pack_id
         WHERE fri.row_id = ?
         ORDER BY fri.sort_order ASC
         LIMIT 200`
      )
      .bind(row.id)
      .all<{
        item_id: string | null;
        pack_id: string | null;
        sort_order: number;
        item_title: string | null;
        item_type: string | null;
        pack_title: string | null;
      }>();

    return ok(c, {
      row: {
        id: row.id,
        slug: row.slug,
        title: row.title
      },
      items: rowItems.results.map((entry) => ({
        id: entry.item_id ?? entry.pack_id ?? "",
        kind: entry.item_id ? "item" : "pack",
        sortOrder: entry.sort_order,
        title: entry.item_id ? (entry.item_title ?? "Untitled") : (entry.pack_title ?? "Untitled Pack"),
        type: entry.item_type
      }))
    });
  });

  return app;
}
