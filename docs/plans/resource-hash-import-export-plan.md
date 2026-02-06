# Resource Hash Export/Import Plan

Date: 2026-01-23

## Goal
Add SHA-256 hashes to all exported resources and de-duplicate imports by resource type + SHA-256 hash so that identical content reuses an existing library entry instead of creating duplicates.

## Requirements
- Export workflows must compute a fresh SHA-256 hash from the resource bytes and include it in export metadata.
- Import workflows must treat the hash as authoritative and only create new resources when no existing resource of the same type matches the hash.
- Imports are assumed to include a SHA-256 hash.
- Hash matching is case-insensitive.

## Scope
- Preset archive export/import (.soundshed.zip)
- Blend archive export/import (.namz)
- Library export (.soundshed-library.zip)
- Resource library metadata persistence

## Data Model
- Add/ensure `hash` is stored alongside each resource entry.
- Archive resource metadata includes `hash`.

## Export Flow
1. Retrieve base64 resource data.
2. Compute SHA-256 from decoded bytes.
3. Store `hash` in archive metadata for each resource.

## Import Flow
1. Read `hash` for each resource entry.
2. If library already has a resource of the same type with a matching hash, map to existing `id` and skip import.
3. Otherwise, import as a new resource and store the hash.

## Notes
- Exported archives store resources in `resources/nam` and `resources/ir` for library exports.
- Preset/blend imports must remap preset/blend references to the existing resource ID when de-dup occurs.
