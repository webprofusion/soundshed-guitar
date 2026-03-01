# Custom NAM Blend Library Plan

> **Status: IMPLEMENTED.** Blend CRUD and UI in `core/ui/ts/blendEditor.ts` and `core/ui/ts/blendManager.ts`. The `.namz` export/import (with resource bundling) is in `core/ui/ts/archiveUtils.ts` and `core/ui/ts/blendEditor.ts`. Blend library stored at `settings/resources/blend-fx-library.json`. Backend load/save handled via `PluginController`. Auto-map from model name tokens implemented in `core/ui/ts/blendUtils.ts` (`inferParamValueFromName`).

## Goals
- Allow users to create custom blended NAM effects that are stored once and referenced by ID in presets and signal chain nodes.
- Use GUIDs generated in the UI for blend IDs and imported resource IDs.
- No migration required for existing presets (they do not use blended effects).
- Categories are constrained to Tone3000 categories:
  - `pedal` (Pedals / FX)
  - `preamp` (Preamps)
  - `amp` (Amps)
  - `full-rig` (Full Rigs)
  - `cab` (Cab IRs)

## Non-Goals (for now)
- Full preset packaging/sharing beyond the blend archive.
- Advanced effect-parameter mapping beyond the supported common set.

## Data Model

### Blend Definition (stored once)
- `id` (GUID) — generated in UI via `crypto.randomUUID()`.
- `name` — default to model group name; user-editable.
- `category` — one of the Tone3000 categories above.
- `models` — ordered list of model references (Tone3000 resource IDs or library IDs).
- `parameters` — enabled parameter IDs (e.g. `gain`, `drive`, `treble`, `middle`, `bass`, `contour`, `presence`).
- `modelMappings` — per-model mapping payload:
  - `id` — model resource ID.
  - `parameters` — map of `paramId -> normalized value` (0..1; negatives allowed for signed ranges).
  - `parameterId` / `parameterValue` — primary parameter (legacy/compat) derived from the first enabled parameter.
- `blendMode` — `snap` or `interpolate`.

### Storage
- User library file: settings/resources/blend-fx-library.json
- One entry per blend definition; no duplication in presets or signal chain.

## Reference Model (no duplication)
- Presets and signal chain nodes reference `blendId` only.
- Blend definition is resolved by `blendId` at load time.

## UI Behavior
- Blend editor is a modal dialog with model list and per-model parameter mapping.
- Users can add/remove mapped parameters and edit values per model.
- Auto-map can infer parameters from model name tokens (e.g. `T6` → treble 0.6, `B-5` → bass -0.5).
- FX chooser lists blends by name and category.
- Dragging a blend into the chain creates a node that references `blendId`.
- Export/Import:
  - Export creates a `.namz` zip containing `blend.json` and `resources/` with model files.
  - Import loads the `.namz`, re-imports models into the library, then creates a new blend with remapped resource IDs.

## Backend Behavior
- Load the blend library file at startup; broadcast to UI in state.
- Save/update definitions through UI messages.
- Apply blend definitions at preset load time by expanding `modelMappings` into `node.resources` with `parameterId` and `parameterValue`.
- Provide resource data for export via `requestResourceData`.
- Save `.namz` files via native Save dialog (`saveBlendArchive`).

## Open Questions
- Should we surface advanced mapping (non-amp params) for future effect types?
- Do we want per-blend overrides for visualization type beyond category?
