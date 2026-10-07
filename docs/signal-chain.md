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
2. Apply the input mode (Mono takes input 1, input 2 or both summed onto both channels) and the
   active user input calibration gain.
3. Measure processed input diagnostics.
4. Process global pre-chain (for example noise gate and transpose).
5. For each preset: process the preset graph, then mix outputs with pan and mix gain.
6. Process global post-chain (for example EQ and doubler).
7. Apply master gain, then final output protection if enabled.
8. On a mono output, fold the stereo mix into its one channel.

### Bypass Semantics
Disabled nodes skip processing; their buffer becomes a pass-through of gathered inputs. The signal path remains connected.

### Channel Layout
Whether each connection carries one signal on both channels (mono) or two (stereo) is decided
when a graph is built, from the graph's input and the effect types along the path. It is never
measured from the audio, and it never moves because a knob did.

- **The input.** In a DAW the track's bus decides: a mono track is mono, a stereo track stereo,
  and a mono-in/stereo-out insert is mono in. In the standalone app the input mode decides:
  Mono takes input 1, input 2 or both summed onto both channels; Stereo and Dual mono keep the
  two inputs apart. A device with one active input is mono whatever was asked
  (`MultiPresetMixer::ApplyInputLayout`).
- **Each effect type declares whether it can widen** (`EffectProcessor::CanWiden`): whether any
  settings can make its two outputs differ when its two inputs are the same. A pan, a width or
  spread control, an LFO offset between the sides, or a stereo file all count, even at zero.
  The default is true, because the mistakes are not alike: a type wrongly left at true costs CPU
  after it, while one wrongly declared false lets a following mono path drop its right side.
  `ChannelLayoutTests` drives every type that says false with random settings and holds it to
  identical outputs. The types that cannot widen are the amps and drives, the EQs, compressors,
  gate, limiter, gain and input analyzer, the phaser, vibe and wahs, the pitch effects, the
  synth and Guitar to MIDI.
- **Resolution** (`SignalGraphExecutor::ResolveChannelLayout`), in execution order: a node's
  input is stereo if any connection into it is; its output is stereo if its input is or its type
  can widen. A node runs `ProcessMono` on one channel, copied to the other, only when its input is
  mono, it has a mono path and its type cannot widen. Everything else runs `Process` on both
  channels. A mono connection always holds the same samples on both channels: after a node runs
  in stereo on one, its left is copied over its right.
- **Between graphs**, the pre-chain takes the input's layout, each rig the pre-chain's output,
  and the post-chain is stereo, since the Multi-Rig mix bus is.
- **Changes.** A running graph re-resolves in place when its input layout changes (an input mode
  or a bus change); anything structural rebuilds the chain anyway. A NAM node on a mono
  connection loads only its left model; a connection that turns stereo while running builds the
  right one off the DSP lock (`TakeDeferredRebuild`) while the left covers both sides.

Since everything after a node that can widen already runs stereo, turning a pan or a Spread up,
switching a tremolo to Pan or loading a stereo IR is heard in the block it lands in, with nothing
to switch. The cost is CPU: an amp placed after a widening effect runs both sides even while that
effect is not widening. A node's **channel mode** (`GraphNode.channelMode`) is the control for
that: Mono folds a stereo input (summed, or one side alone), runs the node mono and puts out mono;
a widening type in Mono has its output summed too. The chain after it is mono again until the next
type that can widen. Changing it rebuilds the chain.

A mono output (a one-channel bus or device) gets the final mix folded, ½(L+R), rather than its
right side dropped.

### Dual Mono
Dual mono keeps a stereo input as two separate chains through the same preset: nothing crosses
between the sides anywhere, not through a pan and not through a reverb tank. Each side hears what
a mono input of its own would make (`SignalGraphExecutor::SetDualMono`).

- A type that keeps its channels apart (`EffectProcessor::KeepsChannelsSeparate`) runs as usual.
  A few of those only keep them apart once `SetDualMono(true)` switches something off: the
  compressors' and the gate's Stereo Link, the IR cab's slot pan (a balance instead of a fold to
  mono), the digital delay's ping-pong (each side then repeats on its own), and a true-stereo IR
  reverb's cross paths.
- Any other node gets a second instance. The primary runs on the left input, on both channels,
  and keeps its left output; the second runs on the right and keeps its right. Every operation on
  a node reaches both: params, bypass, config, resources, deferred rebuilds, automation, tempo,
  notes. Reverbs, the rotary, 3D Spatial, Ring Mod, the harmonizer, the pitch shifters, Auto Arp,
  the synth, Guitar to MIDI, the wahs and WASM effects are doubled this way.
- Hosted plugins cannot be kept in step with their own editor, so they run shared and the input
  status says so (`dualMonoShared` in `inputModeChanged`).
- On a running graph the second instances are built off the DSP lock first
  (`StageDualMonoTwins`), and switching dual mono on only installs them. A node's mono channel
  mode is off in dual mono, since a fold would cross the sides.
- It needs two inputs and two outputs; on a mono output it falls back to Mono with both inputs
  summed. In a DAW it is a per-instance choice for a stereo track, saved with the project.

`DualMonoTests` drives every registered type at its defaults and under random settings and holds
each side's output to its own input.

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
