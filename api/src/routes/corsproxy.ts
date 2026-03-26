import { Hono } from "hono";
import { fail } from "../lib/http";
import { Env } from "../types/env";

/**
 * Allowed hostnames for the CORS proxy.
 * Restricting to known external API hosts prevents SSRF against internal services.
 */
const ALLOWED_HOSTS = new Set(["www.googleapis.com","www.youtube.com","www.youtube-nocookie.com"]);

export function corsProxyRoutes() {
  const app = new Hono<{ Bindings: Env }>();

  app.get("/corsproxy", async (c) => {
    const rawUrl = c.req.query("url");

    if (!rawUrl) {
      return fail(c, "MISSING_PARAM", "Missing required query parameter: url", 400);
    }

    let target: URL;
    try {
      target = new URL(rawUrl);
    } catch {
      return fail(c, "INVALID_URL", "The url parameter is not a valid URL", 400);
    }

    if (target.protocol !== "https:" && target.protocol !== "http:") {
      return fail(c, "INVALID_URL", "Only http and https URLs are allowed", 400);
    }

    if (!ALLOWED_HOSTS.has(target.hostname)) {
      return fail(
        c,
        "DISALLOWED_HOST",
        `Host '${target.hostname}' is not permitted. Allowed hosts: ${[...ALLOWED_HOSTS].join(", ")}`,
        403
      );
    }

    let upstream: Response;
    try {
      upstream = await fetch(target.toString(), {
        headers: {
          "User-Agent": "SoundshedGuitar-API/1.0"
        }
      });
    } catch (err) {
      return fail(c, "FETCH_ERROR", "Failed to fetch the upstream URL", 502);
    }

    const contentType = upstream.headers.get("content-type") ?? "application/octet-stream";
    const body = await upstream.arrayBuffer();

    return c.newResponse(body, upstream.status as any, {
      "content-type": contentType,
      "x-proxied-status": String(upstream.status)
    });
  });

  return app;
}
