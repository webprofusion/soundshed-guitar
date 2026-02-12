# A3 — Effect Model & Catalog

## Intent
Provide an extensible library of audio effects that can be:
- Discovered at runtime (catalog)
- Instantiated by type identifier
- Controlled by a standardized parameter model
- Integrated into the signal graph (A2)

## Actors
- Engine (instantiates and runs effects)
- UI client (browses catalog; edits node params)
- Effect author (adds new effect types)

## Inputs
- Effect type identifier
- Parameter updates (numeric)
- Configuration updates (string)
- Resource assignments (external files/assets)

## Outputs
- Audio output per processing block
- Effect catalog metadata for UI

## Effect Type Definition (Abstract)
Each effect type must define:
- `typeId`: stable identifier (string)
- `displayName`: user-friendly name
- `category`: grouping label for UI browsing
- `description`: optional
- `parameters`: a list of parameter definitions, each with:
  - `paramId` (string)
  - range (min/max)
  - default value
  - unit (optional)
  - mapping curve (optional)
- `configKeys`: optional supported string configs
- `resourceRequirements`: optional description of required resources (0, 1, or N)

## Behavioral Requirements
### R1 — Standard lifecycle
Each effect instance must support:
- Prepare(sampleRate, maxBlockSize)
- Reset()
- Process(inputs, outputs, numSamples)
- SetParameter(paramId, value)
- SetConfig(key, value)
- LoadResource(resourceRef)

### R2 — Real-time safety
- `Process` must be real-time safe (no blocking, no unbounded allocation).
- Expensive work (file loading, model initialization) must not occur in the audio callback.

### R3 — Parameter behavior
- Parameters must clamp or otherwise validate values to their supported ranges.
- Unknown parameter IDs must be handled gracefully (ignored + optionally logged).

### R4 — Bypass compatibility
- Effects must support being bypassed by the graph executor without violating stability.

### R5 — Latency reporting
- If an effect introduces algorithmic latency, it must declare/report it so the host can compensate where supported.

### R6 — Discoverability
- The system must provide a queryable “effect catalog” for UI:
  - List all effect types
  - Filter by category
  - Fetch metadata for a specific type

### R7 — Extensibility
- Adding a new effect type must not require modifying unrelated effects.
- New effect types must be registerable via a single well-defined integration point.

## Error Handling
- If an effect cannot load required resources:
  - It must enter a safe state (silent output or bypass-equivalent) rather than emitting invalid samples.
  - The error must be surfaced to UI/logging via a structured error event.

## Performance & Real-Time Constraints
- Preparation time must be bounded and should support caching/reuse strategies for heavy resources.

## Compatibility
- Effect type IDs must be stable over time.
- Renames require migration rules at the preset level.

## Test Strategy
- Parameter range tests (min/max/default)
- Resource loading failure tests
- No-NaN/Inf tests under silence and extreme values
- Regression audio tests for key effects (amp/cab, gate, EQ)

## Non-Goals
- Defining the exact UI control type (knob/slider) is not required; only parameter semantics.

## Traceability (Non-Normative)
- docs/fx-library.md
- core/src/dsp/EffectProcessor.h
- core/src/dsp/EffectRegistry.h
