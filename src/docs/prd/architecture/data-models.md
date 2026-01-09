# Data Models Specification

## Overview

This document defines the core data structures used throughout NeuronGuitar. Data models are technology-agnostic, specifying structure and semantics without implementation details.

## Naming Conventions

- **Identifiers**: lowercase with hyphens (e.g., `preset-123`, `nam-amp`)
- **Parameters**: lowercase with underscores (e.g., `input_trim`, `low_freq`)
- **Types**: PascalCase (e.g., `PresetV2`, `GraphNode`)

## Preset Model

### PresetV2

The top-level preset structure (version 2).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| id | string | Yes | Unique identifier (UUID) |
| name | string | Yes | Display name |
| version | int | Yes | Schema version (2) |
| author | string | No | Creator name |
| category | string | No | Classification |
| description | string | No | User-facing description |
| tags | string[] | No | Searchable tags |
| createdAt | datetime | No | Creation timestamp |
| modifiedAt | datetime | No | Last modification |
| global | GlobalSettings | Yes | Global parameters |
| graph | SignalGraph | Yes | Effect signal graph |
| embeddedResources | EmbeddedResource[] | No | Portable resources |

### GlobalSettings

Parameters independent of signal path.

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| inputTrim | float | 0.0 | -40..+20 | Input gain (dB) |
| outputTrim | float | 0.0 | -40..+20 | Output gain (dB) |
| transpose | int | 0 | -24..+24 | Pitch shift (semitones) |

## Signal Graph Model

### SignalGraph

Container for the effect processing graph.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| nodes | GraphNode[] | Yes | Processing nodes |
| edges | GraphEdge[] | Yes | Node connections |

### GraphNode

A single processing unit in the signal graph.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| id | string | Yes | Unique node identifier |
| type | string | Yes | Effect type identifier |
| category | string | No | UI grouping |
| label | string | No | Display name override |
| enabled | bool | No | Bypass toggle (default: true) |
| params | map[string, float] | No | Numeric parameters |
| config | map[string, string] | No | String configuration |
| resource | ResourceRef | No | External resource reference |

### GraphEdge

Connection between nodes.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| from | string | Yes | Source node ID |
| to | string | Yes | Destination node ID |
| fromPort | int | No | Output port index (default: 0) |
| toPort | int | No | Input port index (default: 0) |
| gain | float | No | Edge gain multiplier (default: 1.0) |

### Special Node Types

| Type | Inputs | Outputs | Description |
|------|--------|---------|-------------|
| input | 0 | 1 | Graph entry point |
| output | 1 | 0 | Graph exit point |
| splitter | 1 | N | Signal copy to multiple outputs |
| mixer | N | 1 | Sum multiple inputs |

## Resource Model

### ResourceRef

Reference to an external resource.

| Field | Type | Description |
|-------|------|-------------|
| resourceType | string | Library resource type ("nam", "ir") |
| resourceId | string | Library resource identifier |
| filePath | string | Direct file path |
| embeddedId | string | Embedded resource reference |

Resolution priority: library → embedded → filePath

### EmbeddedResource

Resource embedded in a preset for portability.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| id | string | Yes | Reference ID within preset |
| type | string | Yes | Resource type |
| name | string | Yes | Display name |
| hash | string | Yes | SHA-256 hash |
| data | string | No | Base64-encoded content |
| originalPath | string | No | Original file location |

### LibraryResource

Resource entry in the library catalog.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| type | string | Yes | Resource type |
| id | string | Yes | Unique identifier |
| name | string | Yes | Display name |
| category | string | No | Grouping category |
| description | string | No | User description |
| tags | string[] | No | Searchable tags |
| filePath | string | Yes | File location |
| hash | string | Yes | Content hash |
| size | int | Yes | File size (bytes) |
| addedAt | datetime | No | Addition timestamp |

## Effect Types

### EffectTypeInfo

Metadata describing an available effect type.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| typeId | string | Yes | Unique type identifier |
| version | int | Yes | Type version |
| displayName | string | Yes | Human-readable name |
| description | string | No | User description |
| category | string | Yes | Effect category |
| tags | string[] | No | Searchable tags |
| requiresResource | bool | No | Needs external file |
| resourceTypes | string[] | No | Accepted resource types |
| parameters | ParameterDef[] | Yes | Parameter definitions |

### ParameterDef

Definition of an effect parameter.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| id | string | Yes | Unique parameter ID |
| displayName | string | Yes | UI label |
| description | string | No | Tooltip text |
| type | enum | Yes | Float, Int, Bool, Enum |
| defaultValue | any | Yes | Default value |
| minValue | float | Conditional | Minimum (numeric types) |
| maxValue | float | Conditional | Maximum (numeric types) |
| step | float | No | Increment step |
| unit | string | No | Display unit |
| curve | enum | No | Linear, Log, Exp |
| enumOptions | EnumOption[] | Conditional | Options (Enum type) |

### EnumOption

Option for enum-type parameters.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| value | string | Yes | Internal value |
| label | string | Yes | Display label |

## UI State Model

### UIState

Complete UI state for synchronization.

| Field | Type | Description |
|-------|------|-------------|
| parameters | map[string, float] | Current parameter values |
| currentPreset | PresetInfo | Active preset information |
| presets | PresetSummary[] | Local preset list |
| library | LibrarySummary | Available resources |
| signalGraph | SignalGraph | Current effect graph |

### PresetInfo

Information about the current preset.

| Field | Type | Description |
|-------|------|-------------|
| id | string | Preset identifier |
| name | string | Display name |
| modified | bool | Has unsaved changes |

### PresetSummary

Summary for preset lists.

| Field | Type | Description |
|-------|------|-------------|
| id | string | Preset identifier |
| name | string | Display name |
| category | string | Classification |
| author | string | Creator name |
| tags | string[] | Tags |

### LibrarySummary

Summary of available library resources.

| Field | Type | Description |
|-------|------|-------------|
| nam | ResourceSummary[] | NAM models |
| ir | ResourceSummary[] | Impulse responses |

### ResourceSummary

Summary for resource lists.

| Field | Type | Description |
|-------|------|-------------|
| id | string | Resource identifier |
| name | string | Display name |
| category | string | Grouping |

## Serialization

### JSON Format

Primary serialization format for presets and API communication.

```json
{
  "id": "preset-123",
  "name": "My Preset",
  "version": 2,
  "global": {
    "inputTrim": 0.0,
    "outputTrim": -3.0
  },
  "graph": {
    "nodes": [...],
    "edges": [...]
  }
}
```

### Field Naming

- JSON uses camelCase
- Internal models use language conventions
- Mapping handled by serialization layer

### Null Handling

- Optional fields omitted when null/empty
- Required fields must be present
- Empty arrays serialized as `[]`

### Date/Time Format

ISO 8601 format: `2026-01-09T12:00:00Z`

## Validation Rules

### Preset Validation

```
Required:
- id: non-empty string
- name: non-empty string, max 100 chars
- version: must equal 2
- global: valid GlobalSettings
- graph: valid SignalGraph

Optional fields:
- If present, must match type constraints
```

### Signal Graph Validation

```
Required:
- Exactly one node with type "input"
- Exactly one node with type "output"
- All nodes reachable from input
- All nodes connect to output
- No cycles in graph
- All edge references valid
```

### Parameter Validation

```
For each parameter:
- Value within min/max range
- Correct type for parameter definition
- Enum values from defined options
```

## Migration

### Version Detection

```
1. Read "version" field
2. If missing or 1: apply v1 → v2 migration
3. Validate against target schema
```

### V1 to V2 Migration

```
V1 structure (linear chain):
{
  "effects": [effect1, effect2, ...]
}

V2 structure (signal graph):
{
  "graph": {
    "nodes": [input, effect1, effect2, ..., output],
    "edges": [linear connections]
  }
}
```

## Related Documents

- [Preset Model v2 Design](../preset-model-v2-design.md)
- [Signal Graph Model](./signal-graph-model.md)
- [Resource Model](./resource-model.md)
- [Effect Registry](./effect-registry.md)
