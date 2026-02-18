# Cloudflare Presets Platform MVP Plan

## Assumptions

- We are optimizing for the fastest MVP with basic but safe auth.
- API and website backend run on Cloudflare Workers.
- The app and website both consume the same HTTP API.
- Passwordless email sign-in is acceptable for v1.
- Preset payload binaries are stored in R2; metadata and discovery model live in D1.

## Cloudflare Service Layout

### Runtime and data

- **Worker API**: Auth, discovery, upload orchestration, moderation, admin curation.
- **D1**: Relational metadata, auth/session state, moderation state, curated rows.
- **R2**: Preset/packs/media objects.
- **KV**: Cached discovery payloads (home rails, top rows) with short TTL.
- **Queues**: Async processing (file validation, manifest extraction, moderation pre-check).
- **Cron Triggers**: Recompute trending/popularity and refresh discovery cache.

### Suggested modules in Worker

- `auth/*`: start verify logout me session middleware.
- `discovery/*`: rows, home feed, filters, search.
- `items/*`: item CRUD, submit for review, detail.
- `packs/*`: pack CRUD and listing/detail.
- `uploads/*`: signed upload URLs and completion webhook.
- `admin/*`: moderation and curation endpoints.
- `jobs/*`: queue consumers and scheduled jobs.

## Basic Auth (Passwordless Email)

### Flow

1. `POST /auth/start` with email.
2. Worker generates one-time login token, stores **hash** in `auth_tokens`.
3. Worker sends link/code via email provider.
4. `POST /auth/verify` validates token and expiry, marks token used.
5. Worker creates session row and sets secure `HttpOnly` cookie.

### Cookie/session defaults

- `HttpOnly`, `Secure`, `SameSite=Lax`, path `/`.
- Session TTL: 30 days, rotate session ID every 24h.
- Track `created_at`, `last_seen_at`, `ip_hash`, `user_agent_hash`.

## API Contract (MVP)

### Auth

- `POST /v1/auth/start`
- `POST /v1/auth/verify`
- `POST /v1/auth/logout`
- `GET /v1/auth/me`

### Discovery

- `GET /v1/home`
- `GET /v1/rows/:slug`
- `GET /v1/items/:itemId`
- `GET /v1/packs/:packId`
- `GET /v1/search?q=&type=&taxonomy=&cursor=`

### Creator uploads

- `POST /v1/uploads/init`
- `POST /v1/uploads/complete`
- `POST /v1/items`
- `PATCH /v1/items/:itemId`
- `POST /v1/items/:itemId/submit`

### Social and download

- `POST /v1/items/:itemId/rate`
- `POST /v1/items/:itemId/favorite`
- `POST /v1/items/:itemId/report`
- `POST /v1/items/:itemId/download-url`
- `POST /v1/packs/:packId/download-url`

### Admin and curation

- `GET /v1/admin/review?status=pending`
- `POST /v1/admin/review/:itemId/approve`
- `POST /v1/admin/review/:itemId/reject`
- `PUT /v1/admin/rows/:rowId`

## Discovery UX Data Model (Netflix-style rails)

- `featured_rows` controls row order and eligibility rules.
- `featured_row_items` controls deterministic row item order.
- `taxonomies` + `item_taxonomies` power genre/artist/category/tag pivots.
- Home endpoint returns rail payload with light cards and pagination cursor.

## R2 Key Strategy

- `items/{itemId}/v{version}/payload.bin`
- `items/{itemId}/v{version}/manifest.json`
- `packs/{packId}/v{version}/pack.zip`
- `media/items/{itemId}/thumb.jpg`
- `media/items/{itemId}/preview.mp3`

## Caching and jobs

- Cache `GET /v1/home` and row feeds in KV (TTL 60-300 seconds).
- Invalidate row cache keys on moderation approval/rejection and row edits.
- Nightly cron recomputes trending score from downloads/ratings/recency.

## Security baseline

- Signed R2 URLs with short expiry (2-5 minutes).
- Per-IP and per-user rate limiting in Worker.
- Token hashing at rest; no plaintext one-time tokens in D1.
- Strict item compatibility validation before publish/feature.

## Delivery sequence (8-week MVP)

1. **Week 1-2**: auth/session + users + item metadata CRUD.
2. **Week 3-4**: R2 upload/download + manifests + item detail API.
3. **Week 5-6**: rows/home discovery + taxonomy filters + packs.
4. **Week 7**: moderation queue, admin review, featured row tooling.
5. **Week 8**: rate limits, cache strategy, trending cron, hardening.

## Out of scope for MVP

- Personalized recommendations.
- Full-text search engine beyond simple D1 LIKE/filter queries.
- Creator payouts/revenue sharing.
- Multi-tenant content rights workflows.
