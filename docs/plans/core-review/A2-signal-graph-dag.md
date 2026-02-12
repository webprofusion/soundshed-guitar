# A2 — Signal Processing Graph (DAG)

## Intent
Provide a **flexible audio routing model** that can represent:
- Linear effect chains
- Parallel paths (split/mix)
- Dynamic reconfiguration (editing while running, within safe constraints)

The graph is the **primary representation** of a tone/preset’s signal path.

## Actors
- End user (editing the chain)
- UI client (issuing graph-edit commands)
- Engine (validates, compiles, executes)

## Inputs
- A graph definition (nodes + edges)
- Node parameter updates, enable/bypass updates
- Resource assignments to nodes (where applicable)

## Outputs
- Audio output produced by executing the graph
- Validation status (valid/invalid) and error reporting
- Optional diagnostics: per-node levels and performance stats

## Core Concepts (Abstract Data Model)
### Node
A node represents a processing unit.
- `id`: unique identifier within the graph
- `type`: effect type identifier (string)
- `enabled`: boolean (true = processing active; false = bypass)
- `params`: key/value numeric parameters
- `config`: key/value string configuration
- `resources`: zero or more external resource references

### Edge
An edge represents a signal connection from a source node output to a destination node input.
- `from`, `to`: node identifiers
- `fromPort`, `toPort`: optional port indices (for split/mix topologies)
- `gain`: optional multiplicative factor applied to the edge contribution

### Reserved node types
The system must support these conceptual node roles:
- `input`: graph entry
- `output`: graph exit
- `splitter`: 1→N fan-out
- `mixer`: N→1 summing junction

Implementations may represent these as real nodes or as implicit routing behavior.

## Behavioral Requirements
### R1 — Acyclic execution
- The processing graph **must be acyclic**.
- If a cycle is present, the graph is **invalid** and must not be executed as normal.

### R2 — Deterministic execution order
- For a valid graph, nodes must execute in a **deterministic order** consistent with dependencies.
- Given the same graph and the same input samples, output must be **bitwise-deterministic** unless explicitly documented otherwise.

### R3 — Mixing semantics
- If a node has multiple incoming edges, the inputs must be **summed** (after applying per-edge gain).
- If a node has no incoming signal, it must process **silence** (or a defined default) and must not produce NaN/Inf.

### R4 — Bypass semantics
- If a node is disabled/bypassed, the node must behave as a **pass-through** for its input mix (i.e., graph connectivity remains intact).
- Bypass must not change the graph topology.

### R5 — Split/mix semantics
- A splitter must deliver **copies** of its input to multiple downstream edges.
- A mixer must **sum** its inputs into a single output.
- The graph representation must support modeling parallel cab paths and wet/dry mixes.

### R6 — Graph compilation & preparation
- The engine must be able to “compile” a graph into an executable plan:
  - Validate references and topology
  - Build execution order
  - Create node processors
  - Pre-allocate required per-node buffers
- Preparation must be repeatable on sample-rate or block-size changes.

### R7 — Graph editing
- The system must support these operations while preserving validity:
  - Add/remove/replace a node
  - Reorder nodes in linear representations (which translates to topology changes)
  - Update node params/config
  - Enable/disable node
  - Assign or change node resources
- If an edit results in invalid topology, the engine must fail safely (see Error Handling).

## Error Handling
- Invalid graph must be reported as a structured error to the UI client.
- In audio processing:
  - The engine must not crash.
  - It must fail-soft by outputting silence or bypass (exact policy defined by A1).

## Performance & Real-Time Constraints
- Graph processing must be **real-time safe**:
  - No unbounded allocations per audio block
  - No blocking on locks in the audio callback
- Buffer allocation must occur during a prepare/compile phase, not in the processing loop.

## Threading / Concurrency
- Graph editing commands come from non-audio threads.
- The system must provide a defined synchronization mechanism so edits do not corrupt audio execution.

## Compatibility
- Graph data model must be versioned indirectly via the preset schema.
- Unknown node parameters/config keys must be tolerated (ignored or stored) to support forward compatibility.

## Test Strategy
- DAG validation tests: cycle detection, missing references, orphan nodes.
- Golden routing tests: linear chain, parallel split/mix, wet/dry, multi-input nodes.
- Bypass invariants: enabling/disabling nodes must preserve overall routing and not change latency unexpectedly.

## Non-Goals
- Supporting arbitrary feedback loops is not required.

## Traceability (Non-Normative)
- docs/signal-chain.md
- docs/data-models.md
- core/src/dsp/SignalGraphExecutor.h
- core/src/presets/PresetTypes.h
