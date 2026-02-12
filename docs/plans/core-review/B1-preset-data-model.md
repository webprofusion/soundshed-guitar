# B1 — Preset Data Model (Versioned)

## Intent
Represent a user’s tone configuration as a **portable, versioned data model** that can be:
- Saved/loaded locally
- Exchanged between systems (import/export)
- Used to reconstruct audio behavior (graph + parameters + resources)

## Actors
- End user (creates/loads/saves presets)
- UI client (edits preset metadata and graph)
- Engine (applies preset to DSP)

## Preset Model (Abstract)
A preset must include:

### Identity & metadata
- Stable preset identifier
- Human-readable name
- Optional: author, description, category, tags
- Timestamps (created/modified) as strings

### Versioning
- A required schema version number.
- The loader must support migrating older versions to the current semantic model.

### Global settings
- Input trim (dB)
- Output trim (dB)
- Output volume (linear 0..1)
- Auto-level input/output toggles
- Transpose (semitones)

### Main signal graph
- A2 graph definition (nodes + edges)

### Optional: embedded resources
- A list of embedded resources used for portability when presets reference user-provided files.

### Optional: global signal chain
- A representation of global pre/post processing applied around the preset mix.

## Behavioral Requirements
### R1 — Deterministic application
- Applying a preset must produce deterministic DSP behavior given the same resources and inputs.

### R2 — Validation
- Presets must be validated on load:
  - Required fields present
  - Supported version or migratable version
  - Graph references valid (or deterministically repaired per defined rules)

### R3 — Forward compatibility
- Unknown fields must not cause load failure.
- Unknown node parameter keys must be tolerated.

### R4 — Resource references
- Nodes may reference resources via a `ResourceRef` concept supporting:
  - Library reference (type + stable id)
  - Direct file path reference
  - Embedded reference
- A resolution priority must be defined (e.g., library → embedded → path) and applied consistently.

### R5 — Portability
- Presets must support an export mode that embeds resources so another system can load the preset without external files.

## Error Handling
- If required resources are missing:
  - The system must load the preset in a safe degraded mode (e.g., bypass the dependent node) and notify the user.

## Compatibility & Migration
- Version migrations must be deterministic and testable.
- Migrations must preserve intent even if exact sound cannot be perfectly preserved.

## Test Strategy
- Round-trip serialization tests
- Migration tests from prior versions
- Validation tests for missing/invalid fields

## Non-Goals
- Defining the on-disk directory layout is B2.

## Traceability (Non-Normative)
- docs/data-models.md
- core/src/presets/PresetTypes.h
