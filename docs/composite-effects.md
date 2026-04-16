# Composite Effects

## Key Files
- `core/src/presets/PresetTypes.h` — `CompositeEffectDefinition`, `ExposedParameter` structures
- `core/src/dsp/effects/CompositeEffectProcessor.h` — Processor that runs a nested signal graph
- `core/src/dsp/effects/CompositeEffectProcessor.cpp` — Implementation
- `core/ui/ts/compositeTypes.ts` — TypeScript types for composite effects
- `core/ui/ts/compositeEffects.ts` — UI composite effect management

## Overview

A **composite effect** bundles multiple individual effects into a single reusable unit with a simplified control surface. It acts as one node in the parent signal graph while internally running a mini signal graph. A design layout selects which inner parameters are exposed to the user.

### Use Case Example

A "Vintage Marshall Channel" composite might contain:

```
input → pre_eq → noise_gate → amp_nam → cab_ir → reverb_room → output
```

But to the user it appears as a single effect with only 4 exposed knobs:
- **Drive** → `amp_nam.inputGain`
- **Tone** → `pre_eq.highGain`
- **Reverb** → `reverb_room.mix`
- **Volume** → `amp_nam.outputGain`

## Data Model

### CompositeEffectDefinition

Defines a reusable composite effect template.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique identifier (e.g., `"composite-vintage-marshall"`) |
| `name` | string | Yes | Display name (e.g., `"Vintage Marshall Channel"`) |
| `category` | string | Yes | Effect category for UI grouping (e.g., `"amp"`, `"channel"`) |
| `description` | string | No | User-facing description |
| `author` | string | No | Creator name |
| `tags` | string[] | No | Searchable tags |
| `version` | int | No | Definition schema version (default: 1) |
| `innerGraph` | SignalGraph | Yes | The mini signal graph of inner effects |
| `exposedParams` | ExposedParameter[] | Yes | Parameters surfaced to the user |
| `layout` | EffectLayout | No | Custom control layout design |
| `resourceRefs` | ResourceRef[] | No | Resources needed by inner nodes |
| `createdAt` | string | No | ISO 8601 timestamp |
| `modifiedAt` | string | No | Last modification timestamp |

### ExposedParameter

Maps a user-facing parameter to an inner node parameter.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `paramId` | string | Yes | User-facing parameter ID (e.g., `"drive"`) |
| `displayName` | string | Yes | Label shown in UI (e.g., `"Drive"`) |
| `nodeId` | string | Yes | Target node ID within the inner graph |
| `nodeParamKey` | string | Yes | Parameter key on the target node |
| `minValue` | float | No | Override min range (defaults to inner param def) |
| `maxValue` | float | No | Override max range (defaults to inner param def) |
| `defaultValue` | float | No | Override default value |
| `unit` | string | No | Display unit override |
| `curve` | string | No | Mapping curve: `"linear"` (default), `"log"`, `"exp"` |

### JSON Example

```json
{
  "id": "composite-vintage-marshall",
  "name": "Vintage Marshall Channel",
  "category": "channel",
  "description": "Classic Marshall crunch with pre-EQ and room verb",
  "tags": ["marshall", "crunch", "channel-strip"],
  "innerGraph": {
    "nodes": [
      {"id": "in", "type": "input"},
      {"id": "pre_eq", "type": "eq_parametric", "params": {
        "lowGain": 0.0, "lowFreq": 120, "highGain": 2.0, "highFreq": 6000
      }},
      {"id": "gate", "type": "dynamics_gate", "params": {"thresholdDb": -55.0}},
      {"id": "amp", "type": "amp_nam", "resources": [
        {"resourceType": "nam", "resourceId": "plexi-bright"}
      ], "params": {"inputGain": 3.0}},
      {"id": "cab", "type": "cab_ir", "resources": [
        {"resourceType": "ir", "resourceId": "4x12-sm57"}
      ]},
      {"id": "verb", "type": "reverb_room", "params": {"decay": 0.3, "mix": 0.15}},
      {"id": "out", "type": "output"}
    ],
    "edges": [
      {"from": "in", "to": "pre_eq"},
      {"from": "pre_eq", "to": "gate"},
      {"from": "gate", "to": "amp"},
      {"from": "amp", "to": "cab"},
      {"from": "cab", "to": "verb"},
      {"from": "verb", "to": "out"}
    ]
  },
  "exposedParams": [
    {
      "paramId": "drive",
      "displayName": "Drive",
      "nodeId": "amp",
      "nodeParamKey": "inputGain",
      "minValue": -6.0,
      "maxValue": 24.0,
      "defaultValue": 3.0,
      "unit": "dB"
    },
    {
      "paramId": "tone",
      "displayName": "Tone",
      "nodeId": "pre_eq",
      "nodeParamKey": "highGain",
      "minValue": -12.0,
      "maxValue": 12.0,
      "defaultValue": 2.0,
      "unit": "dB"
    },
    {
      "paramId": "reverb",
      "displayName": "Reverb",
      "nodeId": "verb",
      "nodeParamKey": "mix",
      "minValue": 0.0,
      "maxValue": 1.0,
      "defaultValue": 0.15
    },
    {
      "paramId": "volume",
      "displayName": "Volume",
      "nodeId": "amp",
      "nodeParamKey": "outputGain",
      "minValue": -24.0,
      "maxValue": 12.0,
      "defaultValue": 0.0,
      "unit": "dB"
    }
  ],
  "layout": {
    "effectType": "composite",
    "version": 1,
    "dimensions": {"width": 400, "height": 200},
    "backgrounds": [
      {"layerIndex": 0, "type": "color", "value": "#2a1a0e"}
    ],
    "controls": [
      {"paramKey": "drive", "type": "knob", "position": {"x": 40, "y": 60}, "style": {"knobStyle": "amp"}},
      {"paramKey": "tone", "type": "knob", "position": {"x": 140, "y": 60}, "style": {"knobStyle": "amp"}},
      {"paramKey": "reverb", "type": "knob", "position": {"x": 240, "y": 60}, "style": {"knobStyle": "amp"}},
      {"paramKey": "volume", "type": "knob", "position": {"x": 340, "y": 60}, "style": {"knobStyle": "amp"}}
    ],
    "textLabels": [
      {"id": "title", "text": "VINTAGE MARSHALL", "position": {"x": 200, "y": 16}, "fontSize": 16, "fontWeight": "bold", "color": "#d4a84b", "textAlign": "center"}
    ]
  }
}
```

## Architecture

### Processing Model

A composite effect is a single `EffectProcessor` node in the parent graph. Internally it contains a `SignalGraphExecutor` that processes its own sub-graph.

```
Parent Graph:
  input → [CompositeEffect] → delay → output
                  │
                  ├── Inner Graph:
                  │   input → eq → gate → amp → cab → reverb → output
                  │
                  └── Exposed: drive, tone, reverb, volume
```

### CompositeEffectProcessor

Extends `EffectProcessor`. On construction:
1. Receives a `CompositeEffectDefinition`
2. Creates a nested `SignalGraphExecutor` from the inner graph
3. Builds the exposed parameter map

Processing:
1. Parent executor calls `Process()` on the composite
2. Composite feeds audio into its inner executor's input
3. Inner graph processes through all sub-effects
4. Output is returned to the parent graph

Parameter routing:
- `SetParam("drive", 5.0)` → routes to inner `amp.inputGain = 5.0`
- `GetParam("drive")` → reads from inner `amp.inputGain`

### Resource Handling

Inner nodes that need resources (NAM models, IRs) are resolved through the same `ResourceLibrary`. The composite definition stores `ResourceRef` entries that its inner nodes reference.

### Registration

Composite effects register dynamically with the `EffectRegistry`:
- Type ID: `"composite:{definitionId}"` (e.g., `"composite:vintage-marshall"`)
- Category: from the definition
- Parameters: from the exposed parameter list
- Factory: creates `CompositeEffectProcessor` with the definition

### Lifecycle

1. **Startup**: `CompositeEffectLibrary` loads definitions from `~/.guitarfx/composites/`
2. **Registration**: Each definition registers with `EffectRegistry`
3. **Instantiation**: When a preset graph references a composite type, the factory creates a `CompositeEffectProcessor`
4. **Processing**: Inner graph executes within the parent processing loop
5. **UI**: Exposed params render via the layout (or auto-generated controls)

## Storage Layout

```
~/.guitarfx/
└── composites/
    ├── factory/                           # Built-in composite definitions
    │   ├── vintage-marshall-channel.json
    │   └── clean-jazz-channel.json
    └── user/                              # User-created composites
        └── my-custom-channel.json
```

## UI Integration

### Composite in Signal Path

A composite appears as a single expandable node in the signal path view. The node uses the composite's layout (if defined) or auto-generates controls from exposed parameters.

### Message Protocol

#### Engine → UI Messages

| Type | Payload | Description |
|------|---------|-------------|
| `compositeLibrary` | `{definitions: CompositeDefinition[]}` | Full composite library sync |
| `compositeDefinitionAdded` | `{definition: CompositeDefinition}` | New definition available |
| `compositeDefinitionRemoved` | `{id: string}` | Definition removed |

#### UI → Engine Messages

| Type | Payload | Description |
|------|---------|-------------|
| `saveCompositeDefinition` | `{definition: CompositeDefinition}` | Save/update definition |
| `deleteCompositeDefinition` | `{id: string}` | Delete definition |
| `addCompositeToGraph` | `{definitionId, afterNodeId}` | Add composite to signal path |

### Parameter Updates

Composite node parameters use the standard `setParameter` message with the exposed param ID:
```typescript
postMessage("setParameter", { nodeId: "composite1", key: "drive", value: 5.0 });
```

The engine routes through the `ExposedParameter` mapping to the inner node.

## Creating a Composite Effect (User Flow)

1. User builds a signal path with multiple effects (their "channel")
2. User selects the nodes they want to bundle
3. System creates a `CompositeEffectDefinition` from those nodes + edges
4. User selects which parameters to expose
5. Optionally, user opens the Layout Designer to customize the control surface
6. Definition is saved to the composite library
7. The composite appears in the FX browser under its category

## See Also
- [Signal Chain](signal-chain.md) — Parent graph execution
- [FX Library](fx-library.md) — Effect registration and parameters
- [Data Models](data-models.md) — Graph and resource structures
- [User Interface](user-interface.md) — Layout designer and messaging
