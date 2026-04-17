# Network API

## Key Files
- `api/src/index.ts` — Worker entry point and route mounting
- `api/src/routes/` — Route groups for auth, discovery, items, packs, uploads, and sharing
- `api/src/lib/http.ts` — Standard success/error envelopes
- `api/README.md` — Quickstart and implemented endpoint inventory

## Overview

This document covers the current cloud API project in this repository.

The API is a Cloudflare Worker built with Hono. It backs authentication, discovery, item and pack publishing, upload flows, and sharing consent.

## Runtime Shape

- Runtime: Cloudflare Worker
- Router: Hono
- CORS: enabled for desktop/WebView clients
- Mount points:
  - `/health`
  - `/v1/auth/*`
  - `/v1/*` discovery routes
  - `/v1/items/*`
  - `/v1/packs/*`
  - `/v1/share-consent/*`
  - `/v1/uploads/*`
  - additional `/v1/*` routes for tone advisor and proxy helpers

## Response Envelopes

Successful responses use:

```json
{
  "ok": true,
  "data": {}
}
```

Error responses use:

```json
{
  "ok": false,
  "error": {
    "code": "NOT_FOUND",
    "message": "Route not found"
  }
}
```

## Implemented Route Groups

### Health

- `GET /health`

### Authentication

- `POST /v1/auth/start`
- `POST /v1/auth/verify`
- `GET /v1/auth/me`
- `POST /v1/auth/logout`

### Discovery

- `GET /v1/home`
- `GET /v1/rows/:slug`
- `GET /v1/search`

### Items

- `GET /v1/items`
- `POST /v1/items`
- `GET /v1/items/me/list`
- `GET /v1/items/:itemId`
- `PATCH /v1/items/:itemId`
- `DELETE /v1/items/:itemId`
- `POST /v1/items/:itemId/submit`
- `POST /v1/items/:itemId/publish`
- `GET /v1/items/:itemId/download`

### Packs

- `GET /v1/packs`
- `POST /v1/packs`
- `GET /v1/packs/me/list`
- `GET /v1/packs/:packId`
- `PATCH /v1/packs/:packId`
- `DELETE /v1/packs/:packId`
- `POST /v1/packs/:packId/items`
- `POST /v1/packs/:packId/submit`
- `POST /v1/packs/:packId/publish`
- `GET /v1/packs/:packId/download`

### Share Consent

- `GET /v1/share-consent/status`
- `POST /v1/share-consent/accept`

### Uploads

- `POST /v1/uploads/init`
- `PUT /v1/uploads/:uploadId`
- `POST /v1/uploads/complete`

## Operational Notes

- CORS allows desktop/WebView access and exposes `content-disposition`, `content-length`, and `content-type`.
- Auth startup uses SendGrid when configured.
- In development, auth can fall back to logging the one-time code when the SendGrid secret is absent.
- Uploads currently stream through the Worker rather than using direct pre-signed R2 uploads.
- Shared preset publishing depends on an accepted `tone_sharing_publish` consent record.

## Local Development

From `api/`:

```bash
npm install
npm run d1:migrate
npm run dev
```

Set `SENDGRID_API_KEY` as a Worker secret when email delivery is required.

## See Also

- [api/README.md](../api/README.md) — quickstart and full implemented endpoint list
- [Architecture Overview](architecture-overview.md) — repo-wide system layers
- [Data Models](data-models.md) — preset and resource schema
- [AI Module Generation API Architecture](plans/2026-04-17-ai-module-generation-api-architecture.md) — proposed session, job, artifact, and publish flow for AI-generated WASM modules
