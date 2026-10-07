# Data Models

## Key Files
- `core/src/presets/PresetTypes.h` — Core data structures (`Preset`, `SignalGraph`, `GraphNode`, etc.)
- `core/src/presets/PresetStorage.h` — Preset file I/O and storage layout

## Overview

This document defines the core data structures for presets, signal graphs, and resources. All models serialize to JSON for storage and API communication.

## Naming Conventions

- **Identifiers**: lowercase with hyphens (`preset-123`, `amp-nam`)
- **Parameters**: lowercase with underscores (`input_trim`, `low_freq`)
- **Types**: PascalCase (`PresetV2`, `GraphNode`)
- **JSON fields**: camelCase

## PresetV2

Top-level preset structure (schema version 2).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique identifier (UUID) |
| `name` | string | Yes | Display name |
| `version` | int | Yes | Schema version (must be 2) |
| `author` | string | No | Creator name |
| `category` | string | No | Classification |
| `description` | string | No | User-facing description |
| `tags` | string[] | No | Searchable tags |
| `createdAt` | datetime | No | ISO 8601 timestamp |
| `modifiedAt` | datetime | No | Last modification |
| `designedPeakInputDbfs` | float | No | Stored reference peak captured from raw input during preset design |
| `global` | GlobalSettings | Yes | Global parameters |
| `globalSignalChain` | GlobalSignalChainConfig | No | Shared pre/post chain configuration and input/output routing |
| `graph` | SignalGraph | Yes | Effect signal graph |
| `scenes` | PresetScene[] | No | Multiple named signal-chain variants within one preset |
| `embeddedResources` | EmbeddedResource[] | No | Portable resources |

### PresetScene

When present, `scenes` is the source of truth for per-scene signal chains. The top-level `graph`
field remains for backward compatibility and mirrors the currently active scene graph.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Stable scene identifier within the preset |
| `title` | string | Yes | User-facing scene title |
| `graph` | SignalGraph | Yes | Signal chain for this scene |

### GlobalSettings

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `inputTrim` | float | 0.0 | -40..+20 | Input gain (dB) |
| `outputTrim` | float | 0.0 | -40..+20 | Output gain (dB) |
| `outputVolume` | float | 1.0 | 0.0..1.0 | Output volume (linear) |
| `transpose` | int | 0 | -24..+12 | Pitch shift (semitones) |

Older presets and saved global chains may still carry `autoLevelInput`/`autoLevelOutput` from the retired mixer-wide auto-level. They are read and ignored; the engine no longer has that stage.

### GlobalSignalChainConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `preChainGraph` | SignalGraph | default pre-chain | Shared graph before all presets |
| `postChainGraph` | SignalGraph | default post-chain | Shared graph after all presets |
| `inputGain` | float | 0.0 | Shared input gain in dB |
| `monoMode` | bool | false | Mono input: one input (or both summed) on both channels. Standalone only; in a DAW the bus decides |
| `inputChannel` | int | 0 | Which input mono takes: 0 input 1, 1 input 2, 2 both summed |
| `dualMono` | bool | false | With two inputs, run them as two mono chains with nothing crossing between the sides. Saved per instance in a DAW |
| `outputGain` | float | 0.0 | Shared output gain in dB |
| `limiterEnabled` | bool | false | Final mixer output protection toggle |

## SignalGraph

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `nodes` | GraphNode[] | Yes | Processing nodes |
| `edges` | GraphEdge[] | Yes | Connections |

## GraphNode

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique within graph |
| `type` | string | Yes | Effect type identifier |
| `category` | string | No | UI grouping |
| `label` | string | No | Display name override |
| `title` | string | No | The name the user gave the node |
| `enabled` | bool | No | Bypass toggle (default: true) |
| `channelMode` | string | No | How the node takes a stereo input: absent follows it; `mono`, `monoLeft` or `monoRight` fold it (summed, or one side), run the node mono and put out mono (docs/signal-chain.md, Channel Layout) |
| `params` | map[string, float] | No | Numeric parameters |
| `config` | map[string, string] | No | String configuration |
| `resource` | ResourceRef | No | External resource |

## GraphEdge

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Source node ID |
| `to` | string | Yes | Destination node ID |
| `fromPort` | int | No | Output port (default: 0) |
| `toPort` | int | No | Input port (default: 0) |
| `gain` | float | No | Edge gain (default: 1.0) |

## ResourceRef

| Field | Type | Description |
|-------|------|-------------|
| `resourceType` | string | `"nam"` or `"ir"` |
| `resourceId` | string | Library resource ID |
| `filePath` | string | Direct file path |
| `embeddedId` | string | Embedded resource reference |
| `parameterId` | string | Blend models only: the primary parameter the model was captured at |
| `parameterValue` | float | Blend models only: its position on the Blend sweep |
| `parameters` | map[string, float] | Blend models only: every setting it was captured at, normalised 0..1 |

**Resolution priority**: library → embedded → filePath

**Presets from outside** — a preset archive, a Tone Sharing pack or a generated pack — are
cleaned on the way in, by the engine (`SanitizeImportedPreset`, run by
`ParseFactoryPresetArchive`) and by the web UI (`sanitizeImportedPreset` in
`core/ui/ts/presets/sanitize.ts`): every `filePath` (and `originalPath`, `customModelPath`,
`customIrPath`) is dropped, since it names a file on the sharer's machine or anywhere else the
archive chooses, and a node id that is not 1-64 of `[A-Za-z0-9_-]` gets a fresh id, the same in
the graph and every scene, with the edges following. Node ids end up in the UI's markup.

A blend node's resources are not stored with the preset: the controller builds them
from the node's blend definition whenever it builds the chain.

## BlendDefinition

The blend library holds these (document store, `storage::ItemType::kBlend`). An
`amp_nam_blend` node names one with `config.blendId`.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique id |
| `name` | string | Yes | Display name |
| `category` | string | No | `pedal`, `preamp`, `amp`, `full-rig` or `cab` |
| `models` | string[] | Yes | NAM resource ids |
| `modelMappings` | BlendModelMapping[] | No | Per model, what it was captured at; wins over `models` |
| `blendMode` | string | No | `interpolate` (default) or `snap` |
| `parameters` | string[] | No | The parameters the editor shows, in order |
| `toneGroupId` / `toneGroupTitle` | string | No | The tone group it was made from |
| `factory` | bool | No | Set on blends registered from a factory archive; never stored |

**BlendModelMapping**

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | NAM resource id |
| `parameterId` | string | The primary parameter (the editor writes the first shown) |
| `parameterValue` | float | The primary parameter's captured value, normalised 0..1 |
| `parameters` | map[string, float] | Every captured value, normalised 0..1 |

A model with no captured value keeps its list position on the Blend sweep and is given
no `parameterId`. Node config for a blend node: `blendId`, `blendMode` (rewritten from
the definition on every build) and `blendModeOverride` (the node's own mode; empty
follows the definition).

## EmbeddedResource

For portable preset sharing.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Reference ID within preset |
| `type` | string | Yes | Resource type |
| `name` | string | Yes | Display name |
| `hash` | string | Yes | SHA-256 hash |
| `data` | string | No | Base64-encoded content |
| `originalPath` | string | No | Original file location |

## Example Preset

```json
{
  "id": "preset-abc123",
  "name": "Vintage Crunch",
  "version": 2,
  "author": "ToneHunter",
  "category": "Rock",
  "tags": ["marshall", "classic", "crunch"],
  "global": {
    "inputTrim": -3.0,
    "outputTrim": 0.0
  },
  "graph": {
    "nodes": [
      {"id": "in", "type": "input"},
      {"id": "gate", "type": "dynamics_gate", "params": {"thresholdDb": -55.0}},
      {"id": "amp", "type": "amp_nam", "resource": {"resourceType": "nam", "resourceId": "plexi-bright"}, "params": {"inputGain": 3.0}},
      {"id": "cab", "type": "cab_ir", "resource": {"resourceType": "ir", "resourceId": "4x12-sm57"}},
      {"id": "out", "type": "output"}
    ],
    "edges": [
      {"from": "in", "to": "gate"},
      {"from": "gate", "to": "amp"},
      {"from": "amp", "to": "cab"},
      {"from": "cab", "to": "out"}
    ]
  },
  "scenes": [
    {
      "id": "scene-clean",
      "title": "Clean",
      "graph": {
        "nodes": [
          {"id": "in", "type": "input"},
          {"id": "amp", "type": "amp_nam"},
          {"id": "out", "type": "output"}
        ],
        "edges": [
          {"from": "in", "to": "amp"},
          {"from": "amp", "to": "out"}
        ]
      }
    }
  ]
}
```

## Storage Layout

```
~/.guitarfx/
├── presets/
│   ├── factory/           # Built-in presets
│   │   └── clean-start.json
│   └── user/              # User-created presets
│       └── my-crunch.json
├── library/
│   ├── index.json         # Resource catalog
│   ├── nam/
│   │   └── models/
│   └── ir/
│       └── impulses/
└── cache/
    └── resources/         # Extracted embedded resources
        └── sha256/
```

## Settings Ownership (standalone vs plugin instance)

The standalone app and every hosted plugin instance share **one** settings store — the
path comes from the OS user-data directory with no variation by wrapper type — and a 2s
`PollSharedSyncState()` poll fans changes out between them. Each setting therefore has to
declare who owns it. There are three classes.

| Class | Persisted in | Examples |
|-------|--------------|----------|
| **Shared** | the store, by whoever changes it | interface + user input calibration, DSP level targets, API keys, feature toggles, theme |
| **Instance-owned** | host state (the DAW project); the store only *seeds* a brand-new instance | NAM quality tier, UI layout (`uiSettings`/`uiZoom`/`uiBounds`) |
| **Standalone-only** | the store, and never written by a plugin instance | `lastPresetId`, metronome, input mode, global FX chain |

Rules that keep this honest:

- `IsInstanceOwnedSettingKey()` is the single definition of instance ownership, and
  `SaveAppSettings()` filters on it in **both** directions. Suppressing a save at the call
  site is not enough — the value is already in `mAppSettings`, so the next save of any
  unrelated key would diff it as changed and publish it anyway.
- Restoring host state never writes to the store. `DeserializeState()` runs under a scope
  that blocks saves and rebases `mAppSettingsBaseline` on the way out; without it,
  reopening an old project republishes its whole settings snapshot over settings the user
  has changed since.
- Merging is not applying. Anything merged from host state must also be pushed to the DSP,
  or the instance runs on what `Initialize()` read from the store while the UI reports the
  project's values back.
- A shared-sync reload must re-assert instance-owned values, or another instance's change
  drags this one's tier and editor layout with it.
- A shared-sync reload takes only the shared settings the store has changed since this
  instance last read or wrote them (`MergeAppSettingsChangedInStore()`, against
  `mStoreAppSettingsSnapshot`). The rest keep this instance's values, so a project's restored
  level target or calibration survives an unrelated change elsewhere. A key another
  instance did change wins, project value or not: it is the newer edit.
- Only an open editor polls, so a plugin instance with its editor closed hears nothing.
  It catches up when an editor's UI loads (`uiReady`): if the shared-sync version has
  moved since it last loaded the shared sources (`mSharedSyncVersionLoaded`), it reloads
  them before the UI is sent its state. Its own writes move that version on only when
  nothing was written in between.
- The editor **window size** rides in host state too, but it is not a setting: the editor
  reports its bounds to `SetEditorWindowSize()` and they are written as
  `state["editorWindow"]`, so a DAW reopens each instance at the size the user left it.
  `uiSettings["bounds"]` is *not* that value — the web UI captures it from
  `window.outerWidth`/`screenX`, which are the WebView's CSS pixels and stop agreeing with
  the wrapper's logical units under DPI scaling (and a WebView cannot resize the native
  window it sits in). Window *position* is the host's: no plugin wrapper API lets us ask
  for a placement. Standalone ignores all of this and keeps its own `window-state.json`.
- Global FX (gate, EQ, doubler, transpose, trims) are **not** settings — they live in
  `GlobalSignalChainConfig` on the mixer, per instance, and are never read from a preset.
- Hosted plugin state is the one part of host state standalone *does* restore. Everything
  else in a standalone snapshot is ignored (the store is authoritative), but a plugin's
  opaque chunk exists nowhere else once the app closes, so
  `RestoreStandaloneHostedPluginState()` grafts just those chunks onto the preset startup
  loaded from the store — matched by scene id, node id and plugin identity, and only when
  the snapshot's `presetId` still matches the resolved `lastPresetId`. Unsaved graph edits
  stay discarded. See `docs/fx-library.md` for the full set of recall rules.

## Preset Manager Operations

| Operation | Description |
|-----------|-------------|
| `LoadPreset(id)` | Load preset by ID, resolve resources |
| `SavePreset(preset)` | Save to user presets folder |
| `DeletePreset(id)` | Remove preset file |
| `ListPresets()` | Enumerate available presets |
| `ImportPreset(file)` | Import from file, extract embedded resources |
| `ExportPreset(id, options)` | Export with optional resource embedding |

## Versioning & Migration

### Version Detection
1. Read `version` field
2. If missing or 1: apply v1 → v2 migration
3. Validate against current schema

### V1 to V2 Migration
V1 used a linear effect array; V2 uses a signal graph:
```
V1: {"effects": [effect1, effect2]}
V2: {"graph": {"nodes": [input, effect1, effect2, output], "edges": [...]}}
```

## Validation Rules

### Preset
- `id`: non-empty string
- `name`: non-empty, max 100 chars
- `version`: must equal 2
- `global`: valid GlobalSettings
- `graph`: valid SignalGraph

### SignalGraph
- At least one `input` and one `output` node (recommended)
- All edge references resolve to existing nodes
- No cycles

### Parameters
- Values within defined min/max range
- Correct type for parameter definition

## See Also
- [Signal Chain](signal-chain.md) — Graph execution semantics
- [FX Library](fx-library.md) — Effect types and parameters
- [Network API](network-api.md) — Remote preset operations
- [Composite Effects](composite-effects.md) — Composite effect definitions and data model
