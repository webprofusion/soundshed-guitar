import { Hono } from "hono";
import { ok, fail, safeJson } from "../lib/http";
import { Env } from "../types/env";

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function parseConfigJson(raw: string | null | undefined): Record<string, unknown> {
  if (!raw) {
    return {};
  }
  try {
    const parsed = JSON.parse(raw) as Record<string, unknown>;
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function toAbsoluteUrl(baseUrl: string, path: string): string {
  const base = new URL(baseUrl);
  return new URL(path, `${base.protocol}//${base.host}`).toString();
}

function renderSharePage(params: {
  title: string;
  description: string;
  canonicalUrl: string;
  protocolUrl: string;
  webUrl: string;
  summaryLabel: string;
  imageUrl?: string;
}): Response {
  const escapedTitle = escapeHtml(params.title);
  const escapedDescription = escapeHtml(params.description);
  const escapedCanonical = escapeHtml(params.canonicalUrl);
  const escapedProtocolUrl = escapeHtml(params.protocolUrl);
  const escapedWebUrl = escapeHtml(params.webUrl);
  const escapedLabel = escapeHtml(params.summaryLabel);
  const escapedImage = params.imageUrl ? escapeHtml(params.imageUrl) : "";

  const html = `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>${escapedTitle} | Soundshed Guitar</title>
    <meta name="description" content="${escapedDescription}" />
    <link rel="canonical" href="${escapedCanonical}" />
    <meta property="og:site_name" content="Soundshed Guitar" />
    <meta property="og:type" content="website" />
    <meta property="og:title" content="${escapedTitle}" />
    <meta property="og:description" content="${escapedDescription}" />
    <meta property="og:url" content="${escapedCanonical}" />
    ${escapedImage ? `<meta property="og:image" content="${escapedImage}" />` : ""}
    <meta name="twitter:card" content="summary_large_image" />
    <meta name="twitter:title" content="${escapedTitle}" />
    <meta name="twitter:description" content="${escapedDescription}" />
    ${escapedImage ? `<meta name="twitter:image" content="${escapedImage}" />` : ""}
    <style>
      body { font-family: Arial, Helvetica, sans-serif; background: #0f1116; color: #f2f4f8; margin: 0; }
      .page { max-width: 720px; margin: 4rem auto; padding: 0 1.25rem; }
      .card { background: #171b24; border: 1px solid #2d3646; border-radius: 14px; padding: 1.25rem; }
      .label { color: #8ea5c6; font-size: 0.82rem; text-transform: uppercase; letter-spacing: 0.08em; }
      h1 { margin: 0.5rem 0 0.75rem; font-size: 1.45rem; line-height: 1.3; }
      p { margin: 0 0 1rem; color: #d2d9e5; }
      a.btn { display: inline-block; padding: 0.7rem 1rem; border-radius: 10px; background: #4f8cff; color: #fff; text-decoration: none; font-weight: 600; }
      a.secondary { margin-left: 0.6rem; color: #9ec2ff; }
      .muted { margin-top: 0.75rem; font-size: 0.9rem; color: #9eb0ca; }
    </style>
  </head>
  <body>
    <main class="page">
      <article class="card">
        <div class="label">Tone Sharing ${escapedLabel}</div>
        <h1>${escapedTitle}</h1>
        <p>${escapedDescription}</p>
        <a id="open-app" class="btn" href="${escapedProtocolUrl}">Open in Soundshed Guitar</a>
        <a class="secondary" href="${escapedWebUrl}">Open Web App</a>
        <div class="muted">If the app does not open automatically, the web app will open after a short delay.</div>
      </article>
    </main>
    <script>
      (function () {
        const protocolUrl = ${JSON.stringify(params.protocolUrl)};
        const fallbackUrl = ${JSON.stringify(params.webUrl)};
        const openButton = document.getElementById("open-app");

        let handoffAttempted = false;
        const attemptHandoff = function () {
          if (handoffAttempted) return;
          handoffAttempted = true;

          const startedAt = Date.now();
          window.location.href = protocolUrl;

          window.setTimeout(function () {
            const elapsed = Date.now() - startedAt;
            if (elapsed < 2500) {
              window.location.href = fallbackUrl;
            }
          }, 1400);
        };

        if (openButton) {
          openButton.addEventListener("click", function (event) {
            event.preventDefault();
            attemptHandoff();
          });
        }
      })();
    </script>
  </body>
</html>`;

  return new Response(html, {
    status: 200,
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "public, max-age=300",
    },
  });
}

function renderMissingSharePage(appUrl: string): Response {
  const escapedAppUrl = escapeHtml(appUrl);
  const html = `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Tone Sharing link unavailable | Soundshed Guitar</title>
    <meta name="description" content="This Tone Sharing link is unavailable." />
    <meta name="robots" content="noindex, nofollow" />
  </head>
  <body style="font-family: Arial, Helvetica, sans-serif; background:#0f1116; color:#f2f4f8; margin:0;">
    <main style="max-width:720px; margin:4rem auto; padding:0 1.25rem;">
      <h1>This shared tone is unavailable</h1>
      <p>The preset or pack may have been removed or is not publicly accessible.</p>
      <p><a href="${escapedAppUrl}" style="color:#79a9ff;">Open Soundshed Guitar</a></p>
    </main>
  </body>
</html>`;

  return new Response(html, {
    status: 404,
    headers: { "content-type": "text/html; charset=utf-8" },
  });
}

export function appRoutes() {
  const app = new Hono<{ Bindings: Env }>();

  app.get("/share/item/:itemId", async (c) => {
    const itemId = c.req.param("itemId").trim();
    const canonicalUrl = toAbsoluteUrl(c.req.url, `/share/item/${encodeURIComponent(itemId)}`);
    const protocolUrl = `soundshed://tone-sharing?itemId=${encodeURIComponent(itemId)}`;
    const webUrl = `https://guitar.soundshed.com/?itemId=${encodeURIComponent(itemId)}`;

    const row = await c.env.DB.prepare(
      `SELECT i.id, i.title, i.type, i.config_json, u.display_name
       FROM items i
       LEFT JOIN users u ON u.id = i.creator_user_id
       WHERE i.id = ? AND i.moderation_status = 'approved'`
    ).bind(itemId).first<{
      id: string;
      title: string;
      type: string;
      config_json: string | null;
      display_name: string | null;
    }>();

    if (!row) {
      return renderMissingSharePage("https://guitar.soundshed.com");
    }

    const config = parseConfigJson(row.config_json);
    const description = typeof config.description === "string" && config.description.trim().length > 0
      ? config.description.trim()
      : `${row.type} preset shared on Tone Sharing`;
    const creator = typeof row.display_name === "string" && row.display_name.trim().length > 0
      ? ` by ${row.display_name.trim()}`
      : "";
    const ogTitle = `${row.title}${creator}`;

    return renderSharePage({
      title: ogTitle,
      description,
      canonicalUrl,
      protocolUrl,
      webUrl,
      summaryLabel: "Preset",
    });
  });

  app.get("/share/pack/:packId", async (c) => {
    const packId = c.req.param("packId").trim();
    const canonicalUrl = toAbsoluteUrl(c.req.url, `/share/pack/${encodeURIComponent(packId)}`);
    const protocolUrl = `soundshed://tone-sharing?packId=${encodeURIComponent(packId)}`;
    const webUrl = `https://guitar.soundshed.com/?packId=${encodeURIComponent(packId)}`;

    const row = await c.env.DB.prepare(
      `SELECT p.id, p.title, p.config_json, u.display_name,
              (SELECT COUNT(*) FROM pack_items pi WHERE pi.pack_id = p.id) AS item_count
       FROM packs p
       LEFT JOIN users u ON u.id = p.creator_user_id
       WHERE p.id = ? AND p.moderation_status = 'approved'`
    ).bind(packId).first<{
      id: string;
      title: string;
      config_json: string | null;
      display_name: string | null;
      item_count: number | null;
    }>();

    if (!row) {
      return renderMissingSharePage("https://guitar.soundshed.com");
    }

    const config = parseConfigJson(row.config_json);
    const count = Number(row.item_count ?? 0);
    const fallbackDescription = `Pack with ${count} preset${count === 1 ? "" : "s"} shared on Tone Sharing`;
    const description = typeof config.description === "string" && config.description.trim().length > 0
      ? config.description.trim()
      : fallbackDescription;
    const creator = typeof row.display_name === "string" && row.display_name.trim().length > 0
      ? ` by ${row.display_name.trim()}`
      : "";

    const thumbnailAssetId = typeof config.thumbnailAssetId === "string" ? config.thumbnailAssetId.trim() : "";
    const imageUrl = thumbnailAssetId.length > 0
      ? toAbsoluteUrl(c.req.url, `/v1/packs/${encodeURIComponent(packId)}/thumbnail`)
      : undefined;

    return renderSharePage({
      title: `${row.title}${creator}`,
      description,
      canonicalUrl,
      protocolUrl,
      webUrl,
      summaryLabel: "Pack",
      imageUrl,
    });
  });

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
