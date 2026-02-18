# FX Library

## Key Files
- `src/src/dsp/EffectProcessor.h` — Base interface for all effect processors
- `src/src/dsp/EffectRegistry.h` — Type registration and factory
- `src/src/dsp/effects/` — Individual effect implementations
- `src/src/presets/PresetTypes.h` — `ResourceRef` structure

## Overview

The FX library defines available effect types, their parameters, and resource configuration. Effects register with the `EffectRegistry` for dynamic discovery and instantiation. External resources (NAM models, IRs) are referenced via `ResourceRef` with resolution through the `ResourceLibrary`.

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
| `dynamics` | Dynamics processing | Noise gate, compressor, limiter |
| `amp` | Amplifier simulation | NAM amp models |
| `cab` | Cabinet simulation | IR convolution |
| `eq` | Equalization | Parametric EQ, tilt EQ |
| `distortion` | Distortion/saturation | Drive, fuzz, overdrive |
| `modulation` | Modulation effects | Chorus, flanger, phaser |
| `delay` | Time-based delay | Digital, tape, analog |
| `reverb` | Reverberation | Room, hall, plate, spring |
| `utility` | Utility processing | Gain, splitter, mixer |

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

1. Implement `EffectProcessor` interface
2. Create a registration function that populates `EffectTypeInfo` and calls `EffectRegistry::Instance().Register()`
3. Define parameter metadata in `info.parameters`
4. Place in `src/src/dsp/effects/`
5. Call registration function from `RegisterAllEffects()` in `BuiltinEffects.h`
6. Effect appears in UI automatically via registry queries

## See Also
- [Signal Chain](signal-chain.md) — How effects execute in the graph
- [Data Models](data-models.md) — ResourceRef and preset schema
- [User Interface](user-interface.md) — Effect browser UI
- [Composite Effects](composite-effects.md) — Bundling effects into reusable composites
