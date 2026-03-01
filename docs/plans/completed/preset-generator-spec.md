# Preset Generator Spec (Node CLI, MVP)

> **Status: IMPLEMENTED.** The CLI tool is in `tools/preset-generator/src/` with the following modules: `index.ts` (CLI entry/commands), `generator.ts`, `pairing.ts`, `tone3000.ts`, `validate.ts`, `cache.ts`, `fs-utils.ts`, `types.ts`. Matches the architecture described in this spec.

## Purpose
Create a repeatable offline-friendly process to generate large collections of usable presets with associated NAM/IR resources, grouped into content packs for local app import or remote publishing.

## Primary Goals
- Browse Tone3000 data and discover popular NAM and IR resources.
- Pair NAM models with credible IR matches.
- Generate valid Soundshed `PresetV2` JSON signal chains.
- Export outputs as pack-ready artifacts.
- Minimize Tone3000 API calls and resource downloads via aggressive local caching.

## Assumptions
- Preset output must follow docs/data-models.md and docs/signal-chain.md.
- Resource references must resolve using `resourceType + resourceId` library semantics.
- Tone3000 access uses API key auth and exposes list endpoints for models and IRs.
- AI enrichment is optional; deterministic generation remains authoritative.

## Non-Goals (MVP)
- Audio perceptual scoring/render quality analysis.
- Full moderation/publishing workflow automation.
- Runtime integration into plugin process.

## Architecture
- Tool path: `tools/preset-generator/`
- Runtime: Node.js + TypeScript CLI.
- Commands:
  - `generate`: ingest -> pair -> synthesize -> validate -> write artifacts.
  - `validate`: re-validate generated presets/resources.
  - `pack`: create zip archive with manifest and payload files.

## Data Contracts

### Candidate Resource
- `id`: local canonical ID (`nam:<slug>` / `ir:<slug>`)
- `kind`: `nam` | `ir`
- `name`: display name
- `category`: style/category string
- `tags`: string[]
- `popularity`: numeric score (0..1)
- `downloadUrl`: optional source URL
- `externalId`: source system identifier
- `sha256`: optional content hash if downloaded

### Pairing Candidate
- `namId`, `irId`, `score`, `reasons[]`

### Generated Preset
- `PresetV2` with `version=2`
- valid DAG with at least:
  - input -> gate -> amp_nam -> cab_ir -> output
- optional post-effects per template

### Run Manifest
- `runId`, `createdAt`, `generatorVersion`
- `configDigest`
- generated preset list with hashes
- selected resources with hashes
- cache stats

## Caching Strategy (Required)

### API Response Cache
- Directory: `output/cache/api/`
- Key: SHA-256 of request method + URL + query + auth scope
- Value: JSON payload + fetchedAt + ttlSeconds
- Behavior:
  - cache hit inside TTL: reuse
  - stale entry: conditional refresh when possible, else refetch
  - allow `--refresh` to bypass cache

### Resource Blob Cache
- Directory: `output/cache/resources/`
- Files:
  - `blobs/<sha256>.<ext>`
  - `index.json` mapping source URL/externalId to blob hash
- Behavior:
  - download only if URL not in index or hash missing
  - dedupe by content hash across runs
  - reuse existing blob for repeated candidates

### Cache Policy
- Configurable TTLs:
  - discovery endpoints default 24h
  - popular lists default 6h
- Max cache size optional (future LRU cleanup).

## Generation Flow
1. Load config and seed lists.
2. Ingest candidates from Tone3000 and/or local seed files.
3. Apply popularity/category/license filters.
4. Resolve local resource cache for selected candidates.
5. Pair NAM with IR via deterministic scoring.
6. Build presets from templates and bounded parameter sets.
7. Validate schema, references, and parameter ranges.
8. Emit artifacts:
  - `presets/*.json`
  - `resources/indexes/resources-index.json`
  - `manifest.run.json`
9. Optional `pack` command emits zip with pack manifest + payloads.

## Pairing Heuristics (MVP)
- Score components:
  - tag overlap (weight 0.35)
  - category compatibility (weight 0.35)
  - popularity mean (weight 0.20)
  - novelty/diversity bonus (weight 0.10)
- Hard constraints:
  - same `kind` cannot pair
  - both sides must pass quality floor

## Naming Rules (MVP)
- Deterministic title: `<Genre> <AmpName> <CabHint> <UseCase>`
- Tags include category, gain profile, and source hints.
- Optional AI pass may rewrite title/description under policy checks.

## Content Pack Grouping
- Group by one strategy per run:
  - genre
  - gain tier
  - amp family
- Output includes `packs/<packId>/pack-manifest.json` and preset IDs.

## Validation Rules
- Every preset has `id`, `name`, `version=2`, `graph`.
- Graph edges reference existing node IDs.
- `amp_nam` nodes include NAM resource refs.
- `cab_ir` nodes include IR resource refs.
- Parameter ranges constrained to known safe defaults from docs/fx-library.md.

## CLI Config Example
`tools/preset-generator/config/default.json`
- Tone3000 auth and endpoints
- cache TTLs
- generation counts
- filter thresholds
- pack grouping mode

## Observability
- `run-summary.json` with:
  - input counts
  - cache hit/miss stats
  - accepted/rejected counts
  - top rejection reasons

## Safety and Legal Notes
- Preserve source attribution in metadata when available.
- Avoid exact trademarked song/artist names in public packs unless policy permits.
- Treat missing licenses as non-publishable by default.

## Future Extensions
- Audio render smoke tests.
- Embedding resources into portable preset exports.
- AI-based style transfer and descriptive copy generation.
- Incremental regeneration based on changed candidates only.
