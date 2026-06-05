# Soundshed Presets API (Cloudflare Worker)

## Quickstart

1. Install dependencies

```bash
npm install
```

2. Update `wrangler.toml` bindings and IDs.

Set `ENVIRONMENT` to `development` for local and `production` in deployed environments.

3. Set SendGrid API key as a Worker secret

```bash
wrangler secret put SENDGRID_API_KEY
```

Also set the Tone3000 proxy bearer secret for `/v1/resourcesearch/*` endpoints:

```bash
wrangler secret put TONE3000_API_BEARER_SECRET
```

4. Apply schema migration to local D1

```bash
npm run d1:migrate
```

Note: the current schema file is treated as a fresh bootstrap (no in-place migration scripts). If your local D1 already has older tables, recreate/reset it before running `npm run d1:migrate`.

5. Start local dev server

```bash
npm run dev
```

## Implemented endpoints

- `GET /health`
- `POST /v1/auth/start`
- `POST /v1/auth/verify`
- `GET /v1/auth/me`
- `POST /v1/auth/logout`
- `GET /v1/home`
- `GET /v1/rows/:slug`
- `GET /v1/search`
- `GET /v1/items`
- `POST /v1/items`
- `GET /v1/items/me/list`
- `PATCH /v1/items/:itemId`
- `DELETE /v1/items/:itemId`
- `POST /v1/items/:itemId/submit`
- `POST /v1/items/:itemId/publish`
- `GET /v1/items/:itemId`
- `GET /v1/items/:itemId/download`
- `GET /v1/packs`
- `POST /v1/packs`
- `GET /v1/packs/me/list`
- `PATCH /v1/packs/:packId`
- `DELETE /v1/packs/:packId`
- `POST /v1/packs/:packId/items`
- `POST /v1/packs/:packId/submit`
- `POST /v1/packs/:packId/publish`
- `GET /v1/packs/:packId`
- `GET /v1/packs/:packId/download`
- `GET /v1/share-consent/status`
- `POST /v1/share-consent/accept`
- `POST /v1/uploads/init`
- `PUT /v1/uploads/:uploadId`
- `POST /v1/uploads/complete`
- `GET /v1/resourcesearch/health`

## Notes

- `POST /v1/auth/start` sends the one-time code through SendGrid.
- CORS is configured to allow all origins and common methods for desktop/WebView access.
- Download response headers (`content-disposition`, `content-length`, `content-type`) are exposed for cross-origin clients.
- Ensure your sender domain/email is verified in SendGrid before use.
- If `SENDGRID_API_KEY` is not set in `development`, auth falls back to logging the one-time code in Worker logs.
- If `SENDGRID_API_KEY` is not set in `production`, `/v1/auth/start` fails with an email configuration error.
- Upload path currently stores binary through Worker endpoint, not direct pre-signed R2 URL.
- Uploads are validated by `kind` during `PUT /v1/uploads/:uploadId` (e.g., preset payload archives must be ZIPs containing only `.json`, `.wav`, and `.nam` files, with at least one `.json` and one `.wav`/`.nam`).
- Tone-sharing preset publishes now expect the public `payloadAssetId` to be the sanitized shareable archive. If the client also uploads a full private backup archive, store it in `privatePayloadAssetId` so it is retained by the API but never served by `/v1/items/:itemId/download`.
- Publishing a preset now requires an accepted `tone_sharing_publish` consent record. The desktop app records it locally and also persists it through `/v1/share-consent/accept`.
