# Signal Chain

## Key Files
- `src/src/dsp/SignalGraphExecutor.h` — Graph execution, buffer routing, topological sort
- `src/src/dsp/SignalGraphExecutor.cpp` — Executor implementation
- `src/src/presets/PresetTypes.h` — `SignalGraph`, `GraphNode`, `GraphEdge` structures
- `src/src/dsp/effects/NAMAmpEffect.h` — Neural amp model loading and processing

## Overview

The signal chain system processes audio through a directed acyclic graph (DAG) of effect nodes. Unlike fixed-order effect chains, the signal graph supports arbitrary node placement, parallel paths with splitters/mixers, and dynamic reconfiguration.

## Graph Model

### SignalGraph
Container for the processing graph.

| Field | Type | Description |
|-------|------|-------------|
| `nodes` | `GraphNode[]` | Processing units |
| `edges` | `GraphEdge[]` | Connections between nodes |

### GraphNode
A single processing unit.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique identifier within graph |
| `type` | string | Yes | Effect type (e.g., `amp_nam`, `ir_cab`, `eq_parametric`) |
| `category` | string | No | UI grouping |
| `label` | string | No | Display name override |
| `enabled` | bool | No | Bypass toggle (default: true) |
| `params` | map | No | Numeric parameter values |
| `config` | map | No | String configuration values |
| `resource` | ResourceRef | No | External resource (NAM model, IR file) |

### GraphEdge
Connection between nodes.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Source node ID |
| `to` | string | Yes | Destination node ID |
| `fromPort` | int | No | Output port index (default: 0) |
| `toPort` | int | No | Input port index (default: 0) |
| `gain` | float | No | Edge gain multiplier (default: 1.0) |

### Special Node Types

| Type | Inputs | Outputs | Description |
|------|--------|---------|-------------|
| `input` | 0 | 1 | Graph entry point |
| `output` | 1 | 0 | Graph exit point |
| `splitter` | 1 | N | Copies signal to multiple outputs |
| `mixer` | N | 1 | Sums multiple inputs with per-edge gain |

## Graph Structures

### Linear Chain
```
input → gate → amp → cab → output
```

```json
{
  "nodes": [
    {"id": "in", "type": "input"},
    {"id": "gate", "type": "dynamics_gate", "params": {"thresholdDb": -50.0}},
    {"id": "amp", "type": "amp_nam", "resource": {"resourceType": "nam", "resourceId": "plexi-bright"}},
    {"id": "cab", "type": "cab_ir", "resource": {"resourceType": "ir", "resourceId": "4x12-sm57"}},
    {"id": "out", "type": "output"}
  ],
  "edges": [
    {"from": "in", "to": "gate"},
    {"from": "gate", "to": "amp"},
    {"from": "amp", "to": "cab"},
    {"from": "cab", "to": "out"}
  ]
}
```

### Parallel Paths (Dual Cab)
```
            ┌→ cab1 →┐
input → amp → split   → mixer → output
            └→ cab2 →┘
```

Mixers sum incoming edges with per-edge `gain` for blend control.

### Wet/Dry Mix
```
            ┌→ delay →┐
input → split          → mixer → output
            └─────────┘ (dry)
```

## Execution Model

### Topological Sort
Nodes execute in dependency order via Kahn's algorithm. The executor validates acyclicity—cycles cause the graph to be marked invalid.

### Buffer Management
- Per-node stereo buffers allocated at `Prepare()` time
- Sized for `maxBlockSize`
- No allocations during audio processing

### Processing Loop
```
1. Apply inputTrim to host input buffer
2. Copy input to the input node's buffer
3. For each node in topological order:
   - If disabled: pass-through (buffer unchanged)
   - If mixer: sum all incoming edges with gains
   - If splitter: downstream nodes read from splitter's buffer
   - Otherwise: processor.Process(input, output, samples)
4. Copy output node's buffer to host output
5. Apply outputTrim
```

### Bypass Semantics
Disabled nodes skip processing; their buffer becomes a pass-through of gathered inputs. The signal path remains connected.

### Implicit I/O Nodes
If edges reference `__input__` or `__output__` but those nodes are missing, the executor inserts implicit input/output nodes during `SetGraph()`.

## Processor Lifecycle

| Method | When Called | Purpose |
|--------|-------------|---------|
| `SetGraph(graph)` | Preset load | Build execution order, create processors |
| `Prepare(rate, blockSize)` | Sample rate/buffer change | Allocate buffers, prepare processors |
| `Reset()` | Playback start | Clear processor state |
| `Process(in, out, samples)` | Audio callback | Execute graph |

### Processor Creation
- Effect instantiation via `EffectRegistry::Create(type)`
- Reserved types (`input`, `output`, `splitter`, `mixer`) use `PassthroughProcessor`
- Parameters applied via `SetParameter()` / `SetConfig()`
- Resources resolved via `ResourceLibrary` and loaded via `LoadResource(path)`

## Global Parameters

Applied outside the graph:

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `inputTrim` | -40..+20 dB | 0.0 | Gain before graph |
| `outputTrim` | -40..+20 dB | 0.0 | Gain after graph |

## Multi-Preset Mixing (Future)

Support for running multiple presets in parallel with mix/mute/solo controls:
- Each preset runs its own signal graph
- Outputs summed with per-preset gain
- Solo/mute for A/B comparison

## Validation Rules

Current executor validates:
- **Acyclic**: Topological sort must cover all nodes

Future validation (not yet enforced):
- Exactly one `input` and one `output` node
- All nodes reachable from input
- All nodes connect to output (no orphans)

## See Also
- [FX Library](fx-library.md) — Effect types, registry, parameters
- [Data Models](data-models.md) — `SignalGraph` JSON schema
- [Architecture Overview](architecture-overview.md) — System layers
