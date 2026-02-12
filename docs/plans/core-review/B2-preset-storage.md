# B2 — Preset Storage & Import/Export

## Intent
Provide a storage subsystem that can:
- Persist presets to disk
- Enumerate available presets
- Import/export presets (including portable packages)

## Actors
- End user (save/load/import/export)
- UI client (requests lists and operations)
- Host environment (provides filesystem roots and dialogs)

## Inputs
- Preset objects (B1)
- Files and directories chosen by the user
- Optional: archives containing presets + resources

## Outputs
- Stored preset files
- Lists of presets and metadata
- Export files/packages

## Behavioral Requirements
### R1 — CRUD operations
The system must support:
- Save preset to a chosen location or to a default user preset location
- Load preset from a file
- Delete preset
- List presets from configured directories

### R2 — Atomicity and corruption resistance
- Saving must be robust to partial writes (e.g., write-temp + replace).
- A corrupt preset file must not prevent listing/loading other presets.

### R3 — Import
- Importing a preset must:
  - Validate and migrate the preset (B1)
  - Resolve/extract embedded resources where present
  - Register imported resources in the local resource library as needed

### R4 — Export
- Exporting a preset must support at least two modes:
  - Reference-only: keeps library references; does not embed large binaries
  - Portable: embeds required external resources

### R5 — Multiple preset roots
- The system must support multiple preset folders (e.g., factory + user + custom folders).
- Factory content must be read-only.

### R6 — Stable identifiers
- Preset IDs must remain stable across saves/exports unless explicitly “Save As” semantics are requested.

## Error Handling
- If a save/import/export fails, the UI must receive a clear structured error with:
  - User-facing message
  - Optional detail for logs/support

## Performance Constraints
- Listing operations must scale to large libraries without blocking the audio thread.
- Import/export of large resources must be done off the audio thread.

## Test Strategy
- Save/load round-trip
- Corruption tolerance tests
- Import/export portability tests

## Traceability (Non-Normative)
- docs/data-models.md
- core/src/presets/PresetStorage.h
- core/src/IPluginHost.h (dialogs, paths)
