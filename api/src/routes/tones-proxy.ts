import { Hono } from "hono";
import { fail, ok } from "../lib/http";
import { Env } from "../types/env";

const DEFAULT_TONE3000_API_BASE = "https://www.tone3000.com/api/v1";

function resolveUpstreamBase(env: Env): string {
  const raw = (env.TONE3000_API_BASE ?? "").trim();
  const base = raw || DEFAULT_TONE3000_API_BASE;
  return base.endsWith("/") ? base.slice(0, -1) : base;
}

function copyResponseHeaders(source: Headers): Record<string, string> {
  const headers: Record<string, string> = {};
  const keys = [
    "content-type",
    "cache-control",
    "etag",
    "last-modified",
    "content-disposition",
    "content-length",
  ];
  for (const key of keys) {
    const value = source.get(key);
    if (value) {
      headers[key] = value;
    }
  }
  headers["x-soundshed-tones-proxy"] = "1";
  return headers;
}

function resolveProxyPath(requestUrl: URL, wildcardPath: string): string {
  const normalizedWildcard = wildcardPath.replace(/^\/+/, "").trim();
  if (normalizedWildcard) {
    return normalizedWildcard;
  }

  // Fallback for environments where mounted wildcard params can be empty.
  const markers = ["/v1/resourcesearch/", "/resourcesearch/"];
  for (const marker of markers) {
    const index = requestUrl.pathname.indexOf(marker);
    if (index >= 0) {
      const candidate = requestUrl.pathname.slice(index + marker.length).replace(/^\/+/, "").trim();
      if (candidate) {
        return candidate;
      }
    }
  }

  return "";
}

export function tonesProxyRoutes() {
  const app = new Hono<{ Bindings: Env }>();

  app.get("/health", async (c) => {
    const bearerSecret = (c.env.TONE3000_API_BEARER_SECRET ?? "").trim();
    if (!bearerSecret) {
      return fail(c, "CONFIG_ERROR", "Tone3000 proxy secret is not configured", 500);
    }

    const upstream = new URL(`${resolveUpstreamBase(c.env)}/tones/search`);
    upstream.searchParams.set("page", "1");
    upstream.searchParams.set("page_size", "1");

    let upstreamResponse: Response;
    try {
      upstreamResponse = await fetch(upstream.toString(), {
        method: "GET",
        headers: {
          authorization: `Bearer ${bearerSecret}`,
          accept: "application/json"
        },
      });
    } catch {
      return fail(c, "UPSTREAM_ERROR", "Tone3000 upstream health probe failed", 502);
    }

    if (!upstreamResponse.ok) {
      const detail = await upstreamResponse.text().catch(() => "");
      return fail(
        c,
        "UPSTREAM_UNHEALTHY",
        `Tone3000 upstream responded with ${upstreamResponse.status}${detail ? ` - ${detail.slice(0, 240)}` : ""}`,
        502,
      );
    }

    return ok(c, {
      healthy: true,
      upstreamStatus: upstreamResponse.status,
      upstreamBase: resolveUpstreamBase(c.env),
      checkedAt: new Date().toISOString(),
    });
  });

  app.all("/*", async (c) => {
    const bearerSecret = (c.env.TONE3000_API_BEARER_SECRET ?? "").trim();
    if (!bearerSecret) {
      return fail(c, "CONFIG_ERROR", "Tone3000 proxy secret is not configured", 500);
    }

    const wildcardPath = c.req.param("*") ?? "";
    const incomingUrl = new URL(c.req.url);
    const normalizedPath = resolveProxyPath(incomingUrl, wildcardPath);
    if (!normalizedPath) {
      return fail(c, "INVALID_PATH", "Missing Tone3000 API path", 400);
    }

    const upstream = new URL(`${resolveUpstreamBase(c.env)}/${normalizedPath}`);
    upstream.search = incomingUrl.search;

    const outboundHeaders = new Headers();
    const contentType = c.req.header("content-type");
    if (contentType) {
      outboundHeaders.set("content-type", contentType);
    }
    outboundHeaders.set("authorization", `Bearer ${bearerSecret}`);
    outboundHeaders.set("accept", c.req.header("accept") ?? "application/json");
    outboundHeaders.set("user-agent", "SoundshedGuitar-TonesProxy/1.0");

    const method = c.req.method.toUpperCase();
    const hasBody = method !== "GET" && method !== "HEAD";

    let upstreamResponse: Response;
    try {
      upstreamResponse = await fetch(upstream.toString(), {
        method,
        headers: outboundHeaders,
        body: hasBody ? await c.req.arrayBuffer() : undefined,
      });
    } catch {
      return fail(c, "UPSTREAM_ERROR", "Tone3000 upstream request failed", 502);
    }

    const responseBody = method === "HEAD" ? null : await upstreamResponse.arrayBuffer();
    return c.newResponse(responseBody, upstreamResponse.status as any, copyResponseHeaders(upstreamResponse.headers));
  });

  return app;
}
