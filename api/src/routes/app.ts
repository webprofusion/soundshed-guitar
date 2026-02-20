import { Hono } from "hono";
import { ok, fail, safeJson } from "../lib/http";
import { Env } from "../types/env";

export function appRoutes() {
  const app = new Hono<{ Bindings: Env }>();

  app.post("/v1/app/updatecheck", async (c) => {
    try {
      const body = await safeJson<{
        current_version: string;
        os: string;
        cpu: string;
        is_standalone: boolean;
        instance_id: string;
      }>(c.req.raw);

      if (!body) {
        return fail(c, "invalid_json", "Invalid JSON body");
      }

      const { current_version, os, cpu, is_standalone, instance_id } = body;

      if (!current_version || !os || !cpu || is_standalone === undefined || !instance_id) {
        return fail(c, "missing_fields", "Missing required fields: current_version, os, cpu, is_standalone, instance_id");
      }

      const db = c.env.DB;
      const now = new Date().toISOString();

      // Upsert instance info
      await db.prepare(`
        INSERT INTO app_instances (id, os, cpu, current_version, last_seen_at, created_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
          os = excluded.os,
          cpu = excluded.cpu,
          current_version = excluded.current_version,
          last_seen_at = excluded.last_seen_at
      `).bind(instance_id, os, cpu, current_version, now, now).run();

      // Log update check
      const checkId = crypto.randomUUID();
      await db.prepare(`
        INSERT INTO app_update_checks (id, instance_id, version_checked, is_standalone, created_at)
        VALUES (?, ?, ?, ?, ?)
      `).bind(checkId, instance_id, current_version, is_standalone ? 1 : 0, now).run();

      // Get latest active release
      const latestRelease = await db.prepare(`
        SELECT version, download_url, release_notes
        FROM app_releases
        WHERE is_active = 1
        ORDER BY created_at DESC
        LIMIT 1
      `).first<{ version: string; download_url: string; release_notes: string }>();

      if (!latestRelease) {
        return ok(c, {
          is_update_available: false,
          latest_version: current_version,
          download_url: null,
          release_notes: null
        });
      }

      const isUpdateAvailable = latestRelease.version !== current_version;

      return ok(c, {
        is_update_available: isUpdateAvailable,
        latest_version: latestRelease.version,
        download_url: latestRelease.download_url,
        release_notes: latestRelease.release_notes
      });
    } catch (error) {
      console.error("Update check error:", error);
      return fail(c, "server_error", "Failed to process update check", 500);
    }
  });

  return app;
}
