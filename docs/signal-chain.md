# Signal Chain

## Key Files
- `core/src/dsp/SignalGraphExecutor.h` — Graph execution, buffer routing, topological sort
- `core/src/dsp/SignalGraphExecutor.cpp` — Executor implementation
- `core/src/presets/PresetTypes.h` — `SignalGraph`, `GraphNode`, `GraphEdge` structures
- `core/src/dsp/effects/NAMAmpEffect.h` — Neural amp model loading and processing
- `core/src/dsp/MultiPresetMixer.h` — Multi-preset mixing, global chain orchestration

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
    {"id": "gate", "type": "dynamics_gate", "params": {"threshold": -50.0}},
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

**UI note**: Create this topology by inserting a **Splitter** effect from the Utility category. The mixer node is inserted automatically to rejoin the branches. A split has 2 to 4 branches, each on its own mixer input (`toPort`, which the mixer's `level_N`, `pan_N`, `delay_N` and `mute_N` params belong to); the splitter's Branches control changes the count. A splitter's `fromPort` only orders its branches: every output carries the same signal.

Mixers sum incoming edges with per-edge `gain` for blend control.

### Wet/Dry Mix
```
            ┌→ delay →┐
input → split          → mixer → output
            └─────────┘ (dry)
```

## Execution Model

### Topological Sort
Nodes are sorted into levels by Kahn's algorithm: a level is every node whose sources have all
run, so its nodes are independent of each other and may run in parallel, and the levels in order
are the execution order. A cycle, or an edge naming a node the graph does not have, marks the
graph invalid, and an invalid graph outputs silence.

### Buffer Management
- Per-node stereo buffers allocated at `Prepare()` time, sized for `maxBlockSize`
- Each node has a buffer pair and a scratch pair: it gathers its input into the buffers,
  processes from them into the scratch pair, and the two are swapped. No effect is asked to
  process in place, and nothing is copied back.
- No allocations during audio processing

### Processing Loop
1. Measure raw input diagnostics.
2. Apply mono routing and the active user input calibration gain.
3. Measure processed input diagnostics.
4. Process global pre-chain (for example noise gate and transpose).
5. For each preset: process the preset graph, then mix outputs with pan and mix gain.
6. Process global post-chain (for example EQ and doubler).
7. Apply master gain, then final output protection if enabled.

### Bypass Semantics
Disabled nodes skip processing; their buffer becomes a pass-through of gathered inputs. The signal path remains connected.

### Stereo Preservation
When a node's input carries no stereo signal, a node that supports mono processing runs
`ProcessMono` on the left channel and copies the result to the right, which halves a NAM
model's cost on a mono guitar. The output node copies left over right on the same condition. So
an effect that makes a mono input stereo has to say so, or a later node discards its right
channel:

- **`EffectProcessor::ProducesStereoOutput()`**, checked each block in
  `SignalGraphExecutorPlan.cpp`, is the mechanism to use. Return `true` only while the channels
  can actually differ: Chorus and Flanger while Depth and Mix are both above zero, the Doubler
  while Mix is above zero, the delays and Simple Cab while Spread is on, the IR cab while a
  slot is panned or L/R split is on, Ring Mod until its right carrier has relocked, 3D Spatial
  always, and a composite whenever its inner graph's output was stereo.
- **`NodeMayProduceStereo()`** also treats every node whose category is `delay` or `reverb` as
  stereo. It reads the category stored on the graph node, not the registry's: the default
  global post chain stores its Doubler as `modulation`, so that one relies on its own claim.
  It deliberately leaves out `modulation`: phaser, tremolo and the wahs move both channels
  together, and counting them as stereo would put a following NAM node on its stereo path at
  about twice the cost.

`ModulationStereoTests` holds each modulation effect and the Doubler to what their channels
actually do, and checks that the image survives a following amp, the output, the default
global post chain, and being wrapped in a composite.

### Note Routing
Besides audio, the graph carries notes from nodes that make them to nodes that play them
(`EffectProcessor::GetNoteOutput()` and `AcceptsNoteInput()`; `dsp/NoteEvents.h`). Today that is
Guitar to MIDI into the Plugin Host. When the plan is built, each player is given every note
source upstream of it: reachable backwards along its incoming edges, however many nodes lie
between. A source on a parallel branch that does not pass through the player is not one of
them, and routing stops at a composite's edge. A source always sits in an earlier level than
its players, so it has finished its block before they start theirs, parallel levels included.

Each block a player is handed the sources that ran that block (`SetNoteInput()`, just before
`Process()`). A bypassed source, or one with no input, is not among them, and the player takes
that to mean the source holds nothing and lets its notes go. A source that did not run the
block before is `Reset()` before it runs again, so it starts afresh rather than replaying a
note it was holding when it was bypassed.

### Implicit I/O Nodes
If edges reference `__input__` or `__output__` but those nodes are missing, the executor inserts implicit input/output nodes during `SetGraph()`.

## Processor Lifecycle

| Method | When Called | Purpose |
|--------|-------------|---------|
| `SetGraph(graph)` | Preset load | Build execution order, create processors; leaves the executor unprepared, so `Prepare()` follows |
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

Global chains may include additional parameters, but core trims remain, and global effects are now integrated into chains. User input calibration is separate from the preset schema and is applied once from app settings before the pre-chain.

## Global Signal Chains

Global pre-chain and post-chain are SignalGraphs that wrap around all presets, referencing GlobalSignalChainConfig, preChainGraph, postChainGraph. Default contents include pre: input → gate; post: EQ → doubler. Configuration via UI enables shared FX across presets.

## Multi-Preset Mixing

Support for running multiple presets in parallel with mix/mute/solo controls:
- Each preset runs its own signal graph
- Outputs summed with per-preset gain
- Solo/mute for A/B comparison

## Validation Rules

Current executor validates:
- **Acyclic**: Topological sort must cover all nodes
- **Edges name nodes**: an edge to or from a node the graph does not have is invalid

Future validation (not yet enforced):
- Exactly one `input` and one `output` node
- All nodes reachable from input
- All nodes connect to output (no orphans)

## See Also
- [FX Library](fx-library.md) — Effect types, registry, parameters
- [Data Models](data-models.md) — `SignalGraph` JSON schema
- [Architecture Overview](architecture-overview.md) — System layers
