# WASM Effect Host MVP Plan

## Scope

Introduce a generic WASM-backed effect that runs inside the existing signal graph as a normal registry-backed effect. The first slice is intentionally narrow:

- stereo in / stereo out processing
- fixed host-owned controls: `mix`, `inputGainDb`, `outputGainDb`
- fixed guest-readable generic params: `param1` through `param8`
- guest-readable transport param: `bpm`
- one required WASM module resource plus a small fixed set of optional binary resource slots
- real runtime integration in the core DSP layer
- direct-WASM sample modules in tests so the feature is verifiable without an external WASM toolchain

This MVP does not attempt a dynamic UI generated from guest metadata and does not introduce a block-memory ABI yet.

## Assumptions

- The WASM host should behave like any other effect node, not as a parallel execution system.
- The current preset model is already sufficient for multiple resources because `GraphNode.resources` is an array.
- The current UI/catalog metadata is not fully generic for multi-resource effects, so the MVP uses explicit surfaced resource-slot metadata.
- The local build environment does not provide clang, emscripten, wabt, or equivalent tooling, so tests must embed/generate raw WASM binaries directly.

## Runtime Choice

Use Wasmtime's prebuilt C API bundle as the embedded runtime.

Reasons:

- predictable C API for embedding and export lookup
- direct loading from module bytes
- straightforward host import linkage for params/resources
- prebuilt release bundles avoid requiring a local Rust toolchain

## MVP ABI

The guest module exports:

- `audiofx_prepare(sampleRate: f32, maxBlockSize: i32, resourceSlotCount: i32) -> i32`
- `audiofx_reset() -> void`
- `audiofx_process(inLeft: f32, inRight: f32) -> (f32, f32)`
- `audiofx_get_latency_samples() -> i32` (optional)

The host exposes imports in module `host`:

- `read_param(index: i32) -> f32`
- `read_resource_size(slot: i32) -> i32`
- `read_resource_byte(slot: i32, offset: i32) -> i32`

Parameter index mapping:

- `0..7` => `param1..param8`
- `8` => current BPM

Resource slot mapping:

- slot `0` => the WASM module itself
- slots `1..N` => optional arbitrary binary resource blobs

The first implementation processes sample-by-sample. That keeps the ABI easy to exercise with direct binary fixtures and still validates real graph integration, params, and resource access.

## Core Changes

1. Add a new `WasmEffect` processor.
2. Register it through `EffectRegistry` like any other built-in effect.
3. Fetch the Wasmtime C API bundle from CMake and link it into the core target.
4. Surface fixed resource slots to the UI via `EffectTypeInfo.exposedResources`.
5. Preserve original node resource-slot indices when resolving resources in `SignalGraphExecutor`.
6. Allow generic file browsing for unknown resource types in `PluginController`.

## UI Changes

For now, the UI exposes a fixed set of slots:

- `Module` (`wasm`)
- `Data A` (`blob`)
- `Data B` (`blob`)
- `Data C` (`blob`)

The signal-path UI should render generic dropdown/browse controls for surfaced resource slots, not only the NAM / IR-specialized picker buttons.

## Tests

Add focused host tests that build raw WASM binaries in C++ and write them to temp files at runtime. Include a few representative modules:

- gain module using `read_param`
- stereo mix module proving graph execution integration
- resource-driven scaler using `read_resource_byte`
- stateful module proving `reset()` behavior

## Follow-up Work

- dynamic guest metadata for params and resource slots
- shared-memory block ABI for better performance
- explicit resource validation / MIME-style typing beyond `blob`
- preset authoring flows for reusable WASM effect packages
- optional sandboxing / policy limits if untrusted modules become a product requirement