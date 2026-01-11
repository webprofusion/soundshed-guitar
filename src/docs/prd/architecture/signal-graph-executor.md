# Signal Graph Executor

## Purpose

Executes the preset `SignalGraph` by routing audio through nodes in topological order, applying effect processing, split/mix paths, and global trims.

## Execution Model

- Topological sort: Computes execution order via Kahn's algorithm. Graph validity is currently defined as “no cycles” only.
- Implicit I/O: If edges reference `__input__` or `__output__`, the executor inserts implicit `input`/`output` nodes when missing.
- Per-node buffers: Allocates stereo buffers per node (`bufferLeft`, `bufferRight`) and temporary mix buffers. Downstream nodes read from upstream node buffers.
- Splitter: Implemented as a pass-through node. Split behavior emerges because multiple downstream nodes read the splitter’s output buffer.
- Mixer: Sums all incoming edges into the mixer’s node buffer, applying per-edge `gain`.
- Non-mixer multi-input: If multiple edges feed a non-mixer node, the “last edge wins” semantics apply (later edge overwrites buffer).
- Bypass: Disabled processors skip processing; their node buffer remains a pass-through of gathered inputs.
- Global trims: `inputTrim` applied before graph, `outputTrim` applied to the final output.

## Lifecycle

- `SetGraph(graph)`: Copies graph, resets state, inserts implicit I/O as needed, builds execution order, creates processors, and re-prepares if prior `Prepare()` was called.
- `Prepare(sampleRate, maxBlockSize)`: Allocates buffers and calls `Prepare()` on each processor.
- `Reset()`: Calls `Reset()` on each processor.
- `Process(inputs, outputs, numSamples)`: Clears buffers, copies input into the input node, processes nodes in execution order, and writes output from the output node.

## Processor Creation & Configuration

- Effect instantiation: Uses `EffectRegistry::Create(type)` for normal nodes. Reserved types (`input`, `output`, `splitter`, `mixer`) use `PassthroughProcessor`.
- Parameters & config: Applies `node.params` and `node.config` to the processor.
- Resources: If `node.resource` is valid, resolves via `ResourceLibrary` and calls `LoadResource(path)` on the processor.

## Edge Semantics

- Edge fields: `from`, `to`, `fromPort`, `toPort`, `gain`.
- Ports: `fromPort` and `toPort` are available in the model, but the current executor does not branch per-port; it treats edges uniformly. Mixer semantics are implemented regardless of `toPort` numbering.

## Constraints & Guarantees

- Acyclic-only validation: Executor sets `IsValid` to true iff topological sort covers all nodes. Other validations (single I/O, connectivity) are not enforced here.
- Buffering: Fixed-size per-node buffers sized to `maxBlockSize`. Temporary buffers used for processor output.
- Stereo: All processing is stereo in/out (`float**` with two channels).

## Correctness Notes

- Implicit I/O nodes: Documented here; ensure preset authoring tools accommodate this behavior.
- Mixer summation: Correct per-edge `gain` application; non-mixers do not sum.
- Multi-input non-mixer: “Last edge wins” may be surprising; consider validation or explicit policy in the model docs.
- Ports: `fromPort`/`toPort` exist in the model but are not functionally used by the executor beyond standard routing.

## Integration Points

- `EffectProcessor`: V-table for `Prepare`, `Process`, `Reset`, parameter/config setters, and optional `LoadResource`.
- `EffectRegistry`: Type info and factories; reserved types mapped to passthrough.
- `ResourceLibrary`: Resolves `ResourceRef` to file paths for effects requiring external assets.

## Future Enhancements

- Validation expansion: Enforce single input/output, connectivity, and unreachable/orphan detection.
- Port-aware routing: Implement explicit multi-output splitters and multi-input mixers using `fromPort`/`toPort`.
- Multi-channel: Extend beyond stereo (e.g., M/S, surround).
- Feedback support: Controlled loops with delay/limiters.
