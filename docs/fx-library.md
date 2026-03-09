# FX Library

## Key Files
- `src/src/dsp/EffectProcessor.h` — Base interface for all effect processors
- `src/src/dsp/EffectRegistry.h` — Type registration and factory
- `src/src/dsp/effects/` — Individual effect implementations
- `src/src/presets/PresetTypes.h` — `ResourceRef` structure

## Overview

The FX library defines available effect types, their parameters, and resource configuration. Effects register with the `EffectRegistry` for dynamic discovery and instantiation. External resources (NAM models, IRs) are referenced via `ResourceRef` with resolution through the `ResourceLibrary`.

## Effect IDs

### UUID-based canonical IDs

All registered effects use a **UUID v4** as their canonical type ID, defined as `constexpr const char*` constants in `core/src/dsp/EffectGuids.h`. UUIDs are permanent — they never change, regardless of renames or refactoring.

```cpp
// EffectGuids.h
namespace guitarfx::EffectGuids {
  constexpr const char* kAmpNam     = "2eb53b40-6139-4696-8820-387ac56ffa91";
  constexpr const char* kDynamicsGate = "e8388de1-d262-4123-a123-8dbc56f657bc";
  // ...
}
```

Always reference effect types using the `EffectGuids::k*` constants in C++ code — never embed the UUID string literal directly.

### Human-readable aliases

Each effect retains its legacy string ID (e.g. `"amp_nam"`, `"dynamics_gate"`) as an **alias** in `EffectTypeInfo.aliases`. Aliases are resolved to the canonical UUID automatically by `EffectRegistry::Resolve()`, which is called during preset deserialization. This means:

- Old presets with string type IDs load transparently and are normalized to UUIDs in memory.
- New presets are always written with UUID type IDs.
- No preset migration tool is needed.

```cpp
info.type    = EffectGuids::kFuzz;
info.aliases = {"fuzz"};          // old presets using "fuzz" still load
EffectRegistry::Instance().Register(info.type, info, factory);
```

### Routing nodes

The special nodes `input` and `output` are infrastructure-only and are **not** registered effects. They keep their plain string IDs and are never resolved through the registry.

### Registered effect UUID table

All UUID constants are defined in `core/src/dsp/EffectGuids.h`. The table below documents the mapping.

| Constant | UUID | Legacy alias |
|---|---|---|
| `kAmpBuiltin` | `1460a632-6690-4fef-ac6d-6432e3b983f8` | `amp_builtin` |
| `kAmpNam` | `2eb53b40-6139-4696-8820-387ac56ffa91` | `amp_nam` |
| `kAmpNamOptimized` | `49ea214c-91e6-41f9-bd27-ad6eec0ae90a` | `amp_nam_optimized` |
| `kAmpNamBlend` | `8a22c0f8-413b-42c1-b9ba-d543cf011d9e` | `amp_nam_blend` |
| `kFxNam` | `c3263344-65e4-4b7e-b102-ea625700e12f` | `fx_nam` |
| `kCabIr` | `94fa2577-e904-43b8-968b-9c569c511160` | `cab_ir` |
| `kCabSimple` | `27e0eaa3-b023-4b5a-b783-cce65254c0d3` | `cab_simple` |
| `kDynamicsGate` | `e8388de1-d262-4123-a123-8dbc56f657bc` | `dynamics_gate` |
| `kCompressorVca` | `72af3541-2408-4a5c-a2dc-ba164f17eac9` | `compressor_vca` |
| `kCompressorOpto` | `9651c79e-6530-4c23-9150-aa4c0ff2f1d8` | `compressor_opto` |
| `kLimiterBrickwall` | `f4094126-b5de-4c5d-8d05-d56bd8c312d1` | `limiter_brickwall` |
| `kOverdrive` | `fa9e05a8-168a-4293-aa91-6b770de3da1d` | `overdrive` |
| `kDistortion` | `686773c9-30ac-4f33-b0f8-9222146d45b1` | `distortion` |
| `kFuzz` | `3a38b19c-1d97-4989-b5bb-12bcc59d1e6b` | `fuzz` |
| `kEqParametric` | `4b4025ca-64cd-4180-be79-81873b618dba` | `eq_parametric` |
| `kDelayDigital` | `673d3e7a-e9ef-4c5d-a4c4-619dff3355ed` | `delay_digital` |
| `kDelayDoubler` | `778aaef4-40e3-4efa-8782-6a8bfa1d1661` | `delay_doubler` |
| `kReverbRoom` | `7467cbf1-6c7f-4f07-b5dd-a303d25b475c` | `reverb_room` |
| `kReverbHall` | `a07ab1a5-37e5-4279-bd08-5ad640886709` | `reverb_hall` |
| `kReverbPlate` | `9e023b65-5431-48eb-95ff-4f13e7f864a2` | `reverb_plate` |
| `kReverbChamber` | `4ef25e86-9763-40bc-aca6-636b542df60b` | `reverb_chamber` |
| `kReverbSpring` | `0df83b32-23d0-4530-a50e-e0824a5ccf01` | `reverb_spring` |
| `kReverbShimmer` | `7dcbb06d-8925-4f84-b412-232b7c02de26` | `reverb_shimmer` |
| `kReverbAmbient` | `d663f5d8-0f6e-4721-960d-81621fe41801` | `reverb_ambient` |
| `kReverbAdvanced` | `92558944-f0da-4d97-ab75-bed8b63abc31` | `reverb_advanced` |
| `kReverbIr` | `497d3c9d-ed6b-4c71-8e6d-0f9d61564dbc` | `reverb_ir` |
| `kChorus` | `decdd132-029a-46a5-a362-edcde007a450` | `chorus` |
| `kFlanger` | `1a3f3793-7e80-4e3d-ab7b-3ce3ce032fe7` | `flanger` |
| `kPhaser` | `3aa9dc81-31c2-40d5-9b1b-b0b9d1295e9b` | `phaser` |
| `kTremolo` | `c9debb02-d7e7-43e3-8330-b387be46dcf4` | `tremolo` |
| `kAutoWah` | `b06c6d84-01b3-4d0a-ad98-40eecb64438e` | `auto_wah` |
| `kPitchShift` | `0c15f065-8335-4932-9d2f-366d436ec30a` | `pitch_shift` |
| `kTranspose` | `9b89cc46-e05b-4f06-981e-1d74d1f628cf` | `transpose` |
| `kTransposeStft` | `66b3a43a-72eb-4c7a-9c47-50e9ab24b718` | `transpose_stft` |
| `kOctave` | `2e4d5380-5a79-412f-bfc0-bf84ef74d561` | `octave` |
| `kGain` | `0bcd895e-5d36-4247-a351-6bed1fcb37a8` | `gain` |
| `kSynthSaw` | `608e846e-0e60-4064-9c83-37c0df573c38` | `synth_saw` |
| `kSplitter` | `f5f2541b-fcea-4cfd-9e62-eeddf583ef4e` | `splitter` |
| `kMixer` | `d7d1e40f-9c79-4582-9a82-d5fa5bbbfb97` | `mixer` |

### Backward Compatibility via Aliases

`EffectTypeInfo` has an `aliases` field — a list of legacy type IDs that resolve to the canonical UUID:

```cpp
info.type    = EffectGuids::kFuzz;
info.aliases = {"fuzz"};  // old presets using "fuzz" load correctly
EffectRegistry::Instance().Register(info.type, info, factory);
```

`EffectRegistry::Resolve(typeId)` is called during preset deserialization so alias resolution is transparent to all callers.



## Effect Registry

### Registration
Effects register at startup via `REGISTER_EFFECT` macro, providing:
- Type ID (string identifier)
- Display name and description
- Category for UI grouping
- Parameter definitions
- Factory function

### Factory
```cpp
// Create effect instance by type ID
EffectProcessor* processor = EffectRegistry::Create("amp_nam");
```

### Queries
- `GetAllTypes()` — List all registered effect types
- `GetTypesByCategory(category)` — Filter by category
- `GetTypeInfo(typeId)` — Get metadata for specific type

## Effect Categories

| Category | Description | Examples |
|----------|-------------|----------|
| `amp` | Amplifier simulation | NAM amp models, Neural FX |
| `cab` | Cabinet simulation | IR convolution, simple cab |
| `drive` | Gain/clipping/saturation | Overdrive, distortion, fuzz |
| `dynamics` | Dynamics processing | Noise gate, compressor, limiter |
| `eq` | Equalization | Parametric EQ |
| `modulation` | Modulation effects | Chorus, flanger, phaser, tremolo, auto-wah |
| `pitch` | Pitch manipulation | Pitch shift, transpose, octave |
| `delay` | Time-based delay | Digital delay, doubler |
| `reverb` | Reverberation | Room, hall, plate, spring, IR |
| `utility` | Utility processing | Gain, splitter, mixer |
| `synth` | Synthesized tones | Synth saw |

## Effect Processor Interface

```cpp
class EffectProcessor {
    virtual void Prepare(double sampleRate, int maxBlockSize);
    virtual void Process(float** inputs, float** outputs, int numSamples);
    virtual void Reset();
    virtual void SetParameter(const std::string& id, float value);
    virtual float GetParameter(const std::string& id);
    virtual void SetConfig(const std::string& key, const std::string& value);
    virtual bool LoadResource(const std::string& path);
    virtual int GetLatencySamples();
};
```

## Built-in Effect Types

### NAM Amp (`amp_nam`)
Neural amp model processing.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `inputGain` | -24..+24 | 0.0 | dB |
| `outputGain` | -24..+24 | 0.0 | dB |

**Resource**: NAM model file (`.nam`)

### IR Cabinet (`cab_ir`)
Impulse response convolution for cabinet simulation.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mix` | 0.0–1.0 | 1.0 | — |
| `outputGain` | -24..+24 | 0.0 | dB |
| `quality` | 0–3 | 1 | — |

Quality levels: 0=Economy, 1=Standard, 2=High, 3=Full

**Resource**: Audio file (`.wav`)

### Noise Gate (`dynamics_gate`)
Input noise reduction.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `thresholdDb` | -80..0 | -60 | dB |
| `attackMs` | 0.1–50 | 1.0 | ms |
| `releaseMs` | 1–500 | 50 | ms |

### Parametric EQ (`eq_parametric`)
4-band parametric equalizer (low/high shelves + 2 parametric mids).

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `lowGain` | -12..+12 | 0.0 | dB |
| `lowFreq` | 20–500 | 100 | Hz |
| `lowMidGain` | -12..+12 | 0.0 | dB |
| `lowMidFreq` | 100–2000 | 400 | Hz |
| `lowMidQ` | 0.1–10 | 1.0 | — |
| `highMidGain` | -12..+12 | 0.0 | dB |
| `highMidFreq` | 500–8000 | 2000 | Hz |
| `highMidQ` | 0.1–10 | 1.0 | — |
| `highGain` | -12..+12 | 0.0 | dB |
| `highFreq` | 2000–16000 | 8000 | Hz |

### Digital Delay (`delay_digital`)
Clean digital delay.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `timeMs` | 1–2000 | 300 | ms |
| `feedback` | 0.0–0.95 | 0.3 | — |
| `mix` | 0.0–1.0 | 0.3 | — |

### Algorithmic Reverbs
The reverb family uses a shared multi-mode algorithmic engine with dedicated type IDs:

- `reverb_room`
- `reverb_hall`
- `reverb_plate`
- `reverb_chamber`
- `reverb_spring`
- `reverb_shimmer`
- `reverb_ambient`
- `reverb_advanced`

Each mode has tuned internals, but only exposes the controls typically needed for that style.

#### Room Reverb (`reverb_room`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.46 | — |
| `size` | 0.0–1.0 | 0.42 | — |
| `damping` | 0.0–1.0 | 0.56 | — |
| `preDelay` | 0–220 | 8.0 | ms |
| `mix` | 0.0–1.0 | 0.22 | — |

#### Hall Reverb (`reverb_hall`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.76 | — |
| `size` | 0.0–1.0 | 0.70 | — |
| `preDelay` | 0–220 | 24.0 | ms |
| `damping` | 0.0–1.0 | 0.46 | — |
| `width` | 0.0–1.2 | 1.05 | — |
| `mix` | 0.0–1.0 | 0.27 | — |

#### Plate Reverb (`reverb_plate`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.58 | — |
| `preDelay` | 0–220 | 12.0 | ms |
| `tone` | 0.0–1.0 | 0.76 | — |
| `damping` | 0.0–1.0 | 0.35 | — |
| `mix` | 0.0–1.0 | 0.24 | — |

#### Chamber Reverb (`reverb_chamber`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.60 | — |
| `size` | 0.0–1.0 | 0.56 | — |
| `tone` | 0.0–1.0 | 0.52 | — |
| `preDelay` | 0–220 | 15.0 | ms |
| `mix` | 0.0–1.0 | 0.24 | — |

#### Spring Reverb (`reverb_spring`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.50 | — |
| `tone` | 0.0–1.0 | 0.68 | — |
| `drive` | 0.0–1.0 | 0.22 | — |
| `mix` | 0.0–1.0 | 0.23 | — |

#### Shimmer Reverb (`reverb_shimmer`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.84 | — |
| `size` | 0.0–1.0 | 0.80 | — |
| `shimmer` | 0.0–1.0 | 0.45 | — |
| `preDelay` | 0–220 | 28.0 | ms |
| `mix` | 0.0–1.0 | 0.30 | — |

#### Ambient Reverb (`reverb_ambient`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.86 | — |
| `size` | 0.0–1.0 | 0.90 | — |
| `diffusion` | 0.0–1.0 | 0.90 | — |
| `width` | 0.0–1.2 | 1.10 | — |
| `mix` | 0.0–1.0 | 0.32 | — |

#### Advanced Reverb (`reverb_advanced`)

Common controls:

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.64 | — |
| `size` | 0.0–1.0 | 0.55 | — |
| `mix` | 0.0–1.0 | 0.24 | — |
| `damping` | 0.0–1.0 | 0.46 | — |
| `preDelay` | 0–220 | 16.0 | ms |
| `tone` | 0.0–1.0 | 0.62 | — |
| `width` | 0.0–1.2 | 1.00 | — |

Advanced controls:

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `diffusion` | 0.0–1.0 | 0.70 | — |
| `lowCut` | 20–1200 | 140 | Hz |
| `highCut` | 1000–20000 | 12000 | Hz |
| `modRate` | 0.02–8.0 | 0.45 | Hz |
| `modDepth` | 0.0–1.0 | 0.18 | — |
| `ducking` | 0.0–1.0 | 0.08 | — |
| `drive` | 0.0–1.0 | 0.00 | — |
| `shimmer` | 0.0–1.0 | 0.00 | — |

### Simple Cabinet (`cab_simple`)
Filter-based cabinet simulation (no IR required).

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `bass` | 0.0–1.0 | 0.5 | — |
| `presence` | 0.0–1.0 | 0.5 | — |
| `brightness` | 0.0–1.0 | 0.5 | — |
| `mix` | 0.0–1.0 | 1.0 | — |

### VCA Compressor (`compressor_vca`)
Clean, precise VCA-style compressor.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `threshold` | -60..0 | -20 | dB |
| `ratio` | 1–20 | 4.0 | :1 |
| `attack` | 0.1–500 | 10 | ms |
| `release` | 10–2000 | 100 | ms |
| `knee` | 0–24 | 6.0 | dB |
| `makeup` | 0–24 | 0.0 | dB |
| `mix` | 0.0–1.0 | 1.0 | — |

### Opto Compressor (`compressor_opto`)
Smooth optical-style compressor.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `threshold` | -60..0 | -20 | dB |
| `ratio` | 1–20 | 3.0 | :1 |
| `attack` | 5–200 | 20 | ms |
| `release` | 50–3000 | 300 | ms |
| `makeup` | 0–24 | 0.0 | dB |
| `mix` | 0.0–1.0 | 1.0 | — |

### Doubler (`delay_doubler`)
Creates stereo width by mixing a delayed copy of the signal.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `time` | 0–100 | 6.0 | ms |
| `mix` | 0.0–1.0 | 0.3 | — |

### Pitch Shift (`pitch_shift`)
Pitch shift effect using Signalsmith Stretch with stepped or free-form control.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -1..+1 | 0.0 | — |
| `minSemitones` | -12..+12 | -12.0 | st |
| `maxSemitones` | -12..+12 | 12.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |
| `stepMode` | 0–1 | 1 | — |

### Transpose (`transpose`)
High-quality transpose effect optimized for integer semitone steps using Signalsmith Stretch.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -36..+12 | 0.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |

### Transpose (STFT) (`transpose_stft`)
STFT phase-vocoder transpose intended for direct comparison with the default Signalsmith-based transpose, tuned for low-latency down-tuning to -12 semitones.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -12..+12 | 0.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |

### Gain (`gain`)
Simple gain stage.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `gainDb` | -24..+24 | 0.0 | dB |

### Splitter (`splitter`)
Creates parallel paths by inserting a splitter and an auto-join mixer.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| — | — | — | — |

**Notes**:
- The `splitter` effect is user-addable.
- The `mixer` node is inserted automatically to rejoin branches and is not shown in the FX list.

## Resource References

### ResourceRef Structure
Nodes requiring external files (NAM models, IRs) use `ResourceRef`:

| Field | Type | Description |
|-------|------|-------------|
| `resourceType` | string | Library type: `"nam"` or `"ir"` |
| `resourceId` | string | Library resource ID |
| `filePath` | string | Direct file path (fallback) |
| `embeddedId` | string | Embedded resource reference |

### Resolution Priority
1. **Library reference** — `resourceType` + `resourceId`
2. **Embedded reference** — `embeddedId` (for portable presets)
3. **File path** — `filePath` (user files)

```json
{
  "id": "amp1",
  "type": "amp_nam",
  "resource": {
    "resourceType": "nam",
    "resourceId": "plexi-bright"
  }
}
```

## Resource Library

### Library Structure
```
~/.guitarfx/
└── library/
    ├── index.json           # Catalog with metadata
    ├── nam/
    │   └── models/
    │       └── plexi-bright.nam
    └── ir/
        └── impulses/
            └── 4x12-sm57.wav
```

### LibraryResource Entry
| Field | Type | Description |
|-------|------|-------------|
| `type` | string | `"nam"` or `"ir"` |
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `category` | string | Grouping (e.g., "Marshall", "Fender") |
| `filePath` | string | Actual file location |
| `hash` | string | SHA-256 content hash |
| `size` | int | File size in bytes |

### Content Deduplication
Resources are content-addressed by hash. Duplicate files are detected during import and reference the existing library entry.

### Embedded Resources
For portable preset sharing, resources can be embedded:
- Base64-encoded file content in preset JSON
- Extracted to cache on load
- Hash verification for integrity

## Adding New Effects

1. Generate a new UUID v4 (e.g. `[System.Guid]::NewGuid()` in PowerShell) — this is the permanent ID.
2. Add a `constexpr const char* kYourEffect = "<uuid>";` constant to `EffectGuids.h`.
3. Implement the `EffectProcessor` interface.
4. Create a registration function: set `info.type = EffectGuids::kYourEffect` and add a human-readable `info.aliases = {"category_variant"}` string for debugging/legacy use.
5. Define parameter metadata in `info.parameters`.
6. Place in `core/src/dsp/effects/`.
7. Call the registration function from `RegisterAllEffects()` in `BuiltinEffects.h`.
8. Effect appears in UI automatically via registry queries.

> **Renaming an existing effect?** The UUID stays the same — just update `info.displayName`. Add the old alias string to `info.aliases` if it was previously used in preset JSON.

## See Also
- [Signal Chain](signal-chain.md) — How effects execute in the graph
- [Data Models](data-models.md) — ResourceRef and preset schema
- [User Interface](user-interface.md) — Effect browser UI
- [Composite Effects](composite-effects.md) — Bundling effects into reusable composites
