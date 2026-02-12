# B3 — Resource Library (Catalog + Resolution)

## Intent
Provide a persistent catalog of external assets (e.g., amp models, cabinet impulses) so presets can reference resources by stable identifiers rather than raw file paths.

## Actors
- End user (imports/selects resources)
- UI client (browses/selects resources)
- Engine (resolves resource refs to concrete data)

## Concepts
### Library Resource (catalog entry)
A resource entry must include:
- `type`: resource type identifier (e.g., `nam`, `ir`)
- `id`: stable identifier unique within type
- `name`: display name
- Optional metadata: category, description, tags, arbitrary key/value metadata
- `filePath`: a location where the resource can be loaded from
- Optional integrity data: content hash, size

### ResourceRef (reference used by presets)
See B1 for the high-level ResourceRef concept. This spec covers the **library resolution** aspect.

## Behavioral Requirements
### R1 — CRUD
The library must support:
- Add resource
- Update resource
- Remove resource
- Clear all resources

### R2 — Queries
The library must support query operations:
- Lookup by (`type`, `id`)
- List by `type`
- List by (`type`, `category`)
- List all resources

### R3 — Resolution
Given a ResourceRef, the library must be able to resolve it to a concrete loadable location.

Minimum required behavior:
- If the reference is a library ref (`resourceType` + `resourceId`) and the catalog has an entry, resolution returns that entry’s location.
- If the reference is not resolvable as a library ref, resolution may fall back to a direct file reference (if present and valid).
- Embedded resources are resolved by the preset import/load pipeline (B2), not by the library resolver itself.

### R4 — Relative path handling
- When loading the library catalog from a file, relative paths must be interpreted relative to the catalog location (or a defined library root).

### R5 — Robustness
- Missing catalog entries must not crash the system.
- Broken paths must yield a structured “resource missing” outcome rather than undefined behavior.

## Persistence Requirements
### P1 — Load/save catalog
- The library must be loadable from a directory or a file.
- The library must be savable to a file.
- The on-disk representation must be stable enough for forward/backward compatibility (additive fields preferred).

## Performance Constraints
- Library queries must not block the audio thread.
- Loading/saving the catalog must occur off the audio thread.

## Test Strategy
- Lookup/resolve correctness tests
- Relative path resolution tests
- Missing entry / missing file tests

## Traceability (Non-Normative)
- core/src/resources/ResourceLibrary.h
- core/src/resources/ResourceLibrary.cpp
- core/src/presets/PresetTypes.h
