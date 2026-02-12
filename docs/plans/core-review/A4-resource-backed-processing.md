# A4 — Resource-Backed Processing

## Intent
Support effect nodes whose behavior depends on external resources (e.g., neural amp models, impulse responses), while keeping audio processing real-time safe.

## Actors
- End user (selects models/IRs)
- UI client (browses and assigns resources)
- Engine (resolves resources and loads them into processors)

## Inputs
- Resource references attached to graph nodes (library ref, embedded ref, direct file ref)
- Resource library catalog (B3)
- Optional embedded resource payloads provided during import/load (B2)

## Outputs
- Updated DSP behavior for nodes that consume resources
- UI notifications for successful/failed load

## Behavioral Requirements
### R1 — Multiple resources per node
- A node may declare **0..N** resource slots.
- The system must support assigning a resource at a specific slot index.

### R2 — Resolution priority
A resource reference must be resolved deterministically.
- Preferred: library reference (stable id) when available.
- Portable presets: embedded resources must be supported.
- Fallback: direct file path where permitted.

Exact priority must be defined and stable (recommended: library → embedded → direct path).

### R3 — Load lifecycle
- Resource loading must not happen on the audio callback thread.
- Effects must be able to transition between resources safely:
  - No crashes
  - No invalid sample output (NaN/Inf)
  - Clear user-visible errors on failure

### R4 — Degraded operation
If a required resource cannot be resolved or loaded:
- The node must enter a safe degraded mode (bypass-equivalent or silence, depending on effect semantics).
- The error must be surfaced to the UI/logging.

### R5 — Consistency with presets
- When a preset is applied, all resource-backed nodes must be brought to the preset’s referenced resources (or safe degraded mode).

### R6 — Integrity & safety checks (recommended)
- If integrity data (hash/size) is available, implementations should validate it.
- File type validation should be performed before attempting to parse/execute resource content.

## Performance & Real-Time Constraints
- Resource resolution and any heavy parsing/inference initialization must occur off the audio thread.
- Swapping resources should minimize audible artifacts (e.g., avoid clicks where feasible).

## Compatibility
- Resource references must remain stable across library reorganizations via stable ids.
- If an effect type changes its resource requirements, migration rules must be defined.

## Test Strategy
- Missing resource tests (library id not found, file missing)
- Hot-swap tests (change model/IR while running)
- Multi-resource slot tests

## Traceability (Non-Normative)
- docs/fx-library.md (ResourceRef)
- core/src/presets/PresetTypes.h (ResourceRef + node resources)
- core/src/resources/ResourceLibrary.h
- core/src/dsp/SignalGraphExecutor.cpp (resource load during graph build)
- core/src/PluginController.cpp (node resource update flows)
