# NeuralAmpModelerCore Integration Summary

This document summarizes how `NeuralAmpModelerCore` is currently defined and used in Soundshed Guitar.

## Assumptions

- `SoundshedGuitarCore` is the single source of truth for NAM dependency wiring.
- The active consumer project (JUCE) includes `core/` via `add_subdirectory(...)`.
- `GUITARFX_CORE_FETCH_DEPS` decides whether NAM is auto-fetched or must be supplied by the consumer.

## Build-Time Integration

### 1. Dependency declaration and fetch

In `core/CMakeLists.txt`:

- NAM integration is gated by:
  - `GUITARFX_CORE_FETCH_DEPS`
  - `NOT TARGET NeuralAmpModelerCore`
  - `NOT TARGET nam`
- Fetch source:
  - Repository: `https://github.com/sdatkinson/NeuralAmpModelerCore.git`
  - Tag: `v0.4.0.rc3`
- Upstream NAM tools/tests are disabled:
  - `NAM_BUILD_TOOLS=OFF`
  - `NAM_BUILD_TESTS=OFF`

### 2. Compatibility handling

`core/CMakeLists.txt` applies a few integration safeguards:

- MSVC workaround: removes an invalid inherited `-Wno-error` compile flag from NAM tool directory source scope.
- MSVC optimization of build graph: excludes NAM tool executables (`benchmodel`, `loadmodel`, `render`, `run_tests`, `tools`) from default build.
- Eigen bootstrap: ensures NAM's expected `Dependencies/eigen` tree exists by copying fetched Eigen headers when missing.

### 3. Target creation and linking model

`SoundshedGuitarCore` supports two NAM target patterns:

- Preferred: `NeuralAmpModelerCore`
- Fallback: `nam`

If upstream does not define `NeuralAmpModelerCore`, `core/CMakeLists.txt` creates a local static `NeuralAmpModelerCore` target from NAM `*.cpp` sources and sets include/link requirements.

`SoundshedGuitarCore` then links:

- `NeuralAmpModelerCore` when present
- otherwise `nam` when present

When `NeuralAmpModelerCore_SOURCE_DIR` is known, NAM include paths are also exported to consumers.

### 4. Consumer projects

- JUCE project forces core dependency fetch:
  - `juce/CMakeLists.txt` sets `GUITARFX_CORE_FETCH_DEPS ON` before adding `core/`.
- JUCE is the active and supported integration path.

## Runtime Usage in DSP

### 1. Effect registration

NAM-backed effects are registered in `core/src/dsp/effects/BuiltinEffects.h`:

- `amp_nam`
- `amp_nam_optimized`
- `amp_nam_blend`

### 2. Model loading APIs

NAM models are loaded from `.nam` resources using `nam::get_dsp(...)` (fallback/standard path) and optimized wrappers where available:

- `NAMAmpEffect`: uses `nam::get_dsp(...)`
- `OptimizedNAMAmpEffect`: attempts optimized wrapper first, falls back to `nam::get_dsp(...)`
- `MultiModelNAMAmpEffect`: same optimized-then-fallback strategy per model instance

### 3. Factory registration guard

`core/src/dsp/NAMFactoryForce.cpp` forces reference to NAM factory entry points (`wavenet`, `lstm`, `convnet`) so static registration is not dead-stripped by the linker.

Each NAM effect registration path calls `nam::factory::ForceFactoryRegistration()` during startup registration.

### 4. Processing behavior

Current NAM effect flow includes:

- Stereo model instances (left/right)
- Resampling when model expected sample rate differs from host sample rate
- Optional metadata-driven auto-level behavior (input/output/loudness/normalization gain)
- Wet/dry mix and output gain staging

## Tests

Core tests in `core/tests/CMakeLists.txt` link NAM targets the same way:

- Link `NeuralAmpModelerCore` if present
- otherwise link `nam` if present

This keeps test binaries aligned with runtime linkage behavior.

## Current Integration Contract (Practical)

- Primary contract: consumer links `SoundshedGuitarCore`.
- Dependency ownership: currently centralized in `core/CMakeLists.txt` with optional fetch mode.
- Target compatibility: both `NeuralAmpModelerCore` and `nam` are accepted.
- Runtime dependency use: all NAM inference paths live in core effect processors.
