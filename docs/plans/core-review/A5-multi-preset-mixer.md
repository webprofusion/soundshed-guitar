# A5 — Multi-Preset Parallel Mixer

## Intent
Allow multiple presets (each with its own signal graph) to run in parallel and be mixed into a single output for:
- A/B comparisons
- Layered tones
- Setlist-style workflows

## Actors
- End user (adds/removes presets, sets mix)
- UI client (controls mixer)
- Engine (executes preset graphs and mixes)

## Inputs
- Set of active presets (each a B1 preset / A2 graph)
- Per-preset mix settings (gain/pan/mute/solo)
- Global chain configuration (optional)

## Outputs
- Mixed stereo audio output
- Mixer state for UI (active ids + per-preset config)

## Core Concepts
### Preset instance
An “active preset” is an instance identified by a stable `presetInstanceId` (string) with:
- Display name
- Mix level (linear 0..1)
- Pan (-1..1, equal-power)
- Mute
- Solo

## Behavioral Requirements
### R1 — Parallel execution
- Each active preset must be processed independently.
- Outputs are summed into a master output after applying per-instance pan and mix gain.

### R2 — Mute/solo semantics
- If any instance is soloed, only soloed instances contribute to the output.
- Muted instances contribute nothing.

### R3 — Per-instance pan
- Pan must be equal-power or otherwise explicitly defined.

### R4 — Master controls
The mixer must support:
- Master gain
- Optional limiter

### R5 — Global input mode
The system must support global input behavior controls:
- Mono mode (with selectable input channel)
- Stereo mode

### R6 — Auto-leveling (optional feature)
- Auto-level input/output may be enabled to normalize levels based on model metadata and/or calibration settings.

### R7 — Global pre/post chains (optional feature)
- A global pre-chain may apply processing before preset graphs.
- A global post-chain may apply processing after the preset mix.

### R8 — Node-level editing support
- The mixer must expose a way to update a specific node in a specific preset instance:
  - Enable/disable node
  - Update node params/config
  - Update node resources

### R9 — Diagnostics hooks (optional)
- The mixer may provide signal level diagnostics and performance stats without impacting real-time safety.

## Error Handling
- If an individual preset graph becomes invalid or cannot process, the mixer must continue running other instances.
- Failures must be surfaced to UI/logging.

## Performance & Real-Time Constraints
- No blocking locks on the audio callback.
- Per-block work must be bounded.

## Test Strategy
- 1/2/N preset mixing correctness
- Solo/mute interactions
- Pan law correctness
- Global chain applied exactly once around preset mix

## Traceability (Non-Normative)
- core/src/dsp/MultiPresetMixer.h
- docs/signal-chain.md (multi-preset mixing overview)
- docs/user-interface.md (mixer-related message types)
