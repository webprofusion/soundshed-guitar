# FX Library

## Key Files
- `core/src/dsp/EffectProcessor.h` — Base interface for all effect processors
- `core/src/dsp/EffectRegistry.h` — Type registration and factory
- `core/src/dsp/effects/` — Individual effect implementations
- `core/src/presets/PresetTypes.h` — `ResourceRef` structure

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
| `kEqGraphic` | `ef8240ba-c973-4e09-ab65-4faf56a8ecbf` | `eq_graphic` |
| `kDelayDigital` | `673d3e7a-e9ef-4c5d-a4c4-619dff3355ed` | `delay_digital` |
| `kDelayDoubler` | `778aaef4-40e3-4efa-8782-6a8bfa1d1661` | `delay_doubler` |
| `kReverbRoom` | `7467cbf1-6c7f-4f07-b5dd-a303d25b475c` | `reverb_room` |
| `kReverbChamber` | `4ef25e86-9763-40bc-aca6-636b542df60b` | `reverb_chamber` |
| `kReverbSpring` | `0df83b32-23d0-4530-a50e-e0824a5ccf01` | `reverb_spring` |
| `kReverbAdvanced` | `92558944-f0da-4d97-ab75-bed8b63abc31` | `reverb_advanced` |
| `kReverbIr` | `497d3c9d-ed6b-4c71-8e6d-0f9d61564dbc` | `reverb_ir` |
| `kReverbAmbient` | `b3f5445e-0cb9-43a3-ac2f-9216fb8e42dc` | `reverb_ambient` |
| `kChorus` | `decdd132-029a-46a5-a362-edcde007a450` | `chorus` |
| `kFlanger` | `1a3f3793-7e80-4e3d-ab7b-3ce3ce032fe7` | `flanger` |
| `kPhaser` | `3aa9dc81-31c2-40d5-9b1b-b0b9d1295e9b` | `phaser` |
| `kTremolo` | `c9debb02-d7e7-43e3-8330-b387be46dcf4` | `tremolo` |
| `kAutoWah` | `b06c6d84-01b3-4d0a-ad98-40eecb64438e` | `auto_wah` |
| `kSpatial3D` | `a3196960-a89b-4388-829e-cbf8d8dd91c3` | `spatial_3d` |
| `kPitchShift` | `0c15f065-8335-4932-9d2f-366d436ec30a` | `pitch_shift` |
| `kTranspose` | `9b89cc46-e05b-4f06-981e-1d74d1f628cf` | `transpose` |
| `kTransposeStft` | `66b3a43a-72eb-4c7a-9c47-50e9ab24b718` | `transpose_stft` |
| `kOctave` | `2e4d5380-5a79-412f-bfc0-bf84ef74d561` | `octave` |
| `kGain` | `0bcd895e-5d36-4247-a351-6bed1fcb37a8` | `gain` |
| `kSynthSaw` | `608e846e-0e60-4064-9c83-37c0df573c38` | `synth_saw` |
| `kSplitter` | `f5f2541b-fcea-4cfd-9e62-eeddf583ef4e` | `splitter` |
| `kMixer` | `d7d1e40f-9c79-4582-9a82-d5fa5bbbfb97` | `mixer` |
| `kInputAnalyzer` | `2ea17ea3-8f2a-4eea-8e14-babf0d8be5a6` | `input_analyzer` |

### Backward Compatibility via Aliases

`EffectTypeInfo` has an `aliases` field — a list of legacy type IDs that resolve to the canonical UUID:

```cpp
info.type    = EffectGuids::kFuzz;
info.aliases = {"fuzz"};  // old presets using "fuzz" load correctly
EffectRegistry::Instance().Register(info.type, info, factory);
```

`EffectRegistry::Resolve(typeId)` is called during preset deserialization so alias resolution is transparent to all callers.

### Graphic Equalizer (`eq_graphic`)

The Graphic Equalizer has factory five- and ten-band bell templates for Bass, Guitar, and General Purpose use. Every band persists `Enabled`, `Gain`, target `Freq`, and `Q` parameters (`band1Enabled`, `band1Gain`, `band1Freq`, `band1Q`, and so on through band 10). The panel displays only active, enabled bands and prevents manually edited band frequencies from crossing adjacent bands. Effect templates are generic catalog metadata: choosing one copies its complete parameter map into the node, and later user edits remain in that node's serialized signal-chain configuration. Shared presets therefore do not depend on a user's local template library. The same catalog format supports future custom templates for any effect type. The Bass layouts use focused 45–150 Hz controls alongside clarity, presence, and attack bands based on [eight key bass EQ ranges](https://www.behindthemixer.com/art-bass-eq-using-eight-key-frequency-ranges/). The Guitar layouts use the [Neural DSP electric guitar EQ guide](https://neuraldsp.com/articles/electric-guitar-eq-guide/): 80 Hz warmth, 250–500 Hz mud reduction, 800 Hz clarity, 3–5 kHz articulation, and a 10 kHz harshness cut.



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
| `reverb` | Reverberation | Room, chamber, spring, advanced, IR, ambient |
| `utility` | Utility processing | Gain, splitter, mixer, signal analyzer |
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

#### NAM oversampling (global setting)

Oversampling is **not** a preset parameter. It is a global quality setting under
Settings -> DSP Performance -> NAM Processing Quality, alongside the slimmable
model size, and it applies to every NAM node (`amp_nam`, the optimized NAM amp,
NAM FX, and NAM Blend) in every preset and mixer slot.

| App setting | Range | Default |
|-------------|-------|---------|
| `audio.nam.oversampling` | index 0-5: Off, 2x, 4x, 8x, 16x, 32x | `0` (Off) |
| `audio.nam.antiAliasPhase` | index 0-2: Minimum Phase, Linear Short, Linear Long | `0` (Minimum Phase) |

`PluginController::ApplyNamOversamplingSettingsFromAppSettings()` sanitizes both
values, stores them in the process-wide globals in `NAMOversampling.h`, and
pushes them to live nodes as `oversampling` / `antiAliasPhase` node config —
the same mechanism used for `slimmableSize`. Nodes created later read the
globals when their models are prepared. Because both settings change the
resampler's reported latency, applying them also re-reports plugin latency to
the host.

Oversampling uses the NAM-Oversampler processing model: the host signal is
resampled to an integer multiple of the model's native rate, the NAM core's
temporal convolutions are time-scaled by the same multiple, and the result is
resampled back to the host rate. This preserves the model's physical receptive
field instead of shortening its dilations at higher rendering rates.

Minimum Phase is the low-latency real-time default. The linear-phase options
trade more latency for phase-linear anti-alias filtering; their latency is
reported to the host and the dry/blended paths are delayed to remain aligned.

With oversampling Off, `NamOversamplingProcessor` still resamples whenever the
host rate differs from the model's native rate — the same mismatch the old
`BlockSincResampler` path handled — so `antiAliasPhase` is not inert at Off for
a 44.1 kHz host driving a 48 kHz model. When the host and model rates match,
Off allocates no resampler at all and calls `model.process()` directly.

### IR Cabinet (`cab_ir`)
Impulse response convolution for cabinet simulation.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mix` | 0.0–1.0 | 1.0 | — |
| `outputGain` | -24..+24 | 0.0 | dB |
| `quality` | 0–3 | 1 | — |

Quality levels: 0=Economy, 1=Standard, 2=High, 3=Full

**Resource**: Audio file (`.wav`)

### Plugin Host (`plugin_host`)
JUCE-only utility effect that hosts an external plugin supported by JUCE's plugin hosting APIs. It is registered by the JUCE adapter, so core-only builds do not expose this effect type.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mix` | 0.0-1.0 | 1.0 | - |
| `inputGain` | -24..+24 | 0.0 | dB |
| `outputGain` | -24..+24 | 0.0 | dB |

**Resource**: Plugin file or bundle (`resourceType: "plugin"`). On Windows this enables VST3 hosting; macOS builds additionally enable AudioUnit hosting; Linux builds additionally enable LV2 hosting.

**Plugin UI/state**: The node parameter panel can open the hosted plugin's native editor. Plugin state is stored on the graph node as standard base64-encoded `config.pluginStateBase64`; the JUCE adapter captures the live hosted-plugin state before preset save and also exposes a manual capture action in the node panel. Older presets saved with JUCE's `MemoryBlock::toBase64Encoding()` format are still accepted when restoring state.

**Runtime notes**: The current signal graph routes stereo audio only. Hosted plugins may accept or produce MIDI at the JUCE level, but MIDI events are not yet routed between graph nodes.

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
| `lowQ` | 0.1–10 | 0.707 | — |
| `lowMidGain` | -12..+12 | 0.0 | dB |
| `lowMidFreq` | 100–2000 | 400 | Hz |
| `lowMidQ` | 0.1–10 | 1.0 | — |
| `highMidGain` | -12..+12 | 0.0 | dB |
| `highMidFreq` | 500–8000 | 2000 | Hz |
| `highMidQ` | 0.1–10 | 1.0 | — |
| `highGain` | -12..+12 | 0.0 | dB |
| `highFreq` | 2000–16000 | 8000 | Hz |
| `highQ` | 0.1–10 | 0.707 | — |

### Digital Delay (`delay_digital`)
Clean digital delay.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `timeMs` | 1–2000 | 300 | ms |
| `feedback` | 0.0–0.95 | 0.3 | — |
| `mix` | 0.0–1.0 | 0.3 | — |

### Algorithmic Reverbs
Room, chamber, and advanced reverb share a common algorithmic engine. Spring and ambient use dedicated processors because their topology and voicing diverge more strongly from the shared room/chamber design:

- `reverb_room`
- `reverb_chamber`
- `reverb_spring`
- `reverb_advanced`
- `reverb_ambient`

Each effect exposes the controls most relevant to that style rather than sharing one universal surface.

#### Room Reverb (`reverb_room`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.46 | — |
| `size` | 0.0–1.0 | 0.42 | — |
| `damping` | 0.0–1.0 | 0.56 | — |
| `preDelay` | 0–220 | 8.0 | ms |
| `mix` | 0.0–1.0 | 0.22 | — |

#### Chamber Reverb (`reverb_chamber`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.42 | — |
| `size` | 0.0–1.0 | 0.38 | — |
| `tone` | 0.0–1.0 | 0.42 | — |
| `preDelay` | 0–220 | 12.0 | ms |
| `mix` | 0.0–1.0 | 0.16 | — |

#### Spring Reverb (`reverb_spring`)

Dedicated spring-tank model with short dispersive delays, resonant drip emphasis, and nonlinear tank drive.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.42 | — |
| `tone` | 0.0–1.0 | 0.52 | — |
| `drive` | 0.0–1.0 | 0.18 | — |
| `mix` | 0.0–1.0 | 0.18 | — |

#### Advanced Reverb (`reverb_advanced`)

Common controls:

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.40 | — |
| `size` | 0.0–1.0 | 0.40 | — |
| `mix` | 0.0–1.0 | 0.16 | — |
| `damping` | 0.0–1.0 | 0.66 | — |
| `preDelay` | 0–220 | 12.0 | ms |
| `tone` | 0.0–1.0 | 0.42 | — |
| `width` | 0.0–1.2 | 0.88 | — |

Advanced controls:

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `diffusion` | 0.0–1.0 | 0.74 | — |
| `lowCut` | 20–1200 | 140 | Hz |
| `highCut` | 1000–20000 | 7600 | Hz |
| `modRate` | 0.02–8.0 | 0.28 | Hz |
| `modDepth` | 0.0–1.0 | 0.26 | — |
| `ducking` | 0.0–1.0 | 0.08 | — |
| `drive` | 0.0–1.0 | 0.00 | — |

#### Ambient Reverb (`reverb_ambient`)

Long, diffuse late reverb with soft early reflections, slow modulation, and a wide stereo bloom.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.70 | — |
| `space` | 0.0–1.0 | 0.72 | — |
| `diffusion` | 0.0–1.0 | 0.84 | — |
| `preDelay` | 0–200 | 26.0 | ms |
| `tone` | 0.0–1.0 | 0.42 | — |
| `width` | 0.0–1.25 | 1.08 | — |
| `modRate` | 0.02–2.0 | 0.18 | Hz |
| `modDepth` | 0.0–1.0 | 0.38 | — |
| `mix` | 0.0–1.0 | 0.28 | — |
| `outputGain` | -18..+12 | 0.0 | dB |

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

### 3D Spatial (`spatial_3d`)

Positions the signal as a point source anywhere around the listener — left/right,
front/behind, above/below and near/far — and can animate that position.

**What it actually is.** `EffectProcessor::Process` is a two-channel interface, and the
graph is stereo throughout. There is no surround bus to pan into, so this is a
*binaural* renderer: it synthesises the cues a real head would produce and delivers them
over two channels.

**Under what conditions it works:**

| Listening on | Result |
|---|---|
| Headphones | All axes. This is what it is designed for. |
| Loudspeakers | Left/right and distance work. Front/back and height largely collapse — set `listenMode` to Speakers, which reduces the cues that are not surviving. |
| Mono fold-down | Speakers mode removes the interaural delay entirely, so folding down does not comb-filter. |
| Any listener | Height and front/back come from a generic pinna model, not the listener's own ears. Most people get a clear effect; some get very little. This is inherent to non-personalised binaural rendering. |

**Cue synthesis**

| Cue | Method | Axis |
|---|---|---|
| Interaural time difference | Woodworth `(r/c)(θ + sin θ)`, r = 8.75 cm, max ±0.66 ms, on a fractional delay line. The head-shadow filter's own group delay is subtracted so the ITD matches the model. | left/right |
| Interaural level difference | Split 35% boost to the near ear / 65% cut to the far ear, so a source does not swell in loudness as it passes the sides. | left/right |
| Head shadow | One-pole lowpass crossfaded into the far ear, 20 kHz → 1.5 kHz. | left/right |
| Front/back | High shelf (+2 dB front, −8 dB behind) plus a 0.7 ms rear reflection. Deliberately *not* level-compensated: shadowing is a treble phenomenon, so the bass is left alone and a source behind is slightly quieter, as in life. | front/behind |
| Elevation | Pinna notch sweeping 6 kHz (below) → 11 kHz (above) at −9 dB, plus a ±3 dB shelf tilt. | up/down |
| Distance | Inverse-distance gain (reference 1.5 m), air-absorption lowpass past 1.5 m, and a rising early-reflection send so the direct/reflected ratio carries distance too. | near/far |

**Parameters**

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `azimuth` | −180…180 | 0 | ° (0 front, +90 right) |
| `elevation` | −90…90 | 0 | ° |
| `distance` | 0.2…10 | 1.5 | m |
| `mix` | 0…1 | 1.0 | — |
| `roomAmount` | 0…1 | 0.25 | — |
| `listenMode` | 0–1 | 0 | Headphones / Speakers |
| `delayMode` | 0–1 | 0 | Smooth / Doppler |
| `outputTrim` | −12…12 | 0 | dB |
| `motionMode` | 0–6 | 0 | Off, Orbit, Arc, Figure 8, Spiral, Drift, Pendulum |
| `motionRate` | 0.01…4 | 0.06 | Hz |
| `syncMode` / `syncDivision` | — | Free | tempo sync |
| `motionDepth` | 0…1 | 0.6 | azimuth swing |
| `motionElevDepth` | 0…1 | 0.3 | elevation swing |
| `motionDistDepth` | 0…1 | 0.2 | distance breathing |
| `motionDirection` | −1/+1 | +1 | — |
| `motionPhase` | 0…360 | 0 | ° |
| `motionSmooth` | 0…1 | 0.4 | trajectory inertia |
| `motionSeed` | 0…999 | 1 | Drift reproducibility |

Read-only feedback: `currentAzimuth`, `currentElevation`, `currentDistance`,
`currentItdUs`, `currentIldDb`, `effectiveRate`. These drive the `spatialPosition`
message and hence the on-screen puck.

**Factory presets** (per-effect, `isFactory`): Static · Centre, Gentle Orbit, Slow
Carousel, Wide Sway, Overhead Arc, Figure of Eight, Fly-By, Ambient Drift, Rising
Spiral, Tempo Orbit · 1 Bar, Speaker Safe Sway. Each specifies the complete parameter
set, because effect presets are copied wholesale into a graph node rather than merged.

**Behaviour worth knowing about**

- `azimuth`/`elevation`/`distance` are the *anchor* the motion orbits around, not an
  override — you can reposition a running orbit.
- Orbit and Spiral always complete a full 360° circle; `motionDepth` does not shorten
  their sweep. Use Figure 8 or Pendulum for a partial sweep. Arc ignores `motionDepth`.
- Positioning with a delay line means moving the source changes phase over time, which
  *is* a frequency shift. At factory preset rates it stays under one cent (verified by
  test). `delayMode = Doppler` additionally tracks the propagation delay to the source,
  which bends pitch audibly on approach and adds up to ~29 ms of variable delay to the
  wet path — intended for `mix = 1`.
- `mix = 0` is not bit-exact passthrough; it is the input delayed by exactly
  `GetLatencySamples()`, so the dry path cannot comb against the wet one. Latency is a
  constant and never changes with parameters.
- Drift mode is deterministic: the same `motionSeed` always replays the same trajectory.

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

### Transpose (Hybrid) (`transpose_hybrid`)
Low-latency hybrid transpose path for down-tuning. Uses a dual-band STFT sustain path, switches medium and deeper downshifts to the more stable polyphonic analysis mode, and blends in a latency-aligned dry transient assist to keep pick attacks tighter without leaving the low fundamentals unshifted. Live semitone changes fade the reconfigured wet path back in to reduce clicks and STFT warmup artifacts.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -15..0 | -5.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |
| `transientAssist` | 0.0–1.0 | 0.65 | — |
| `transientHoldMs` | 2.0–40.0 | 12.0 | ms |
| `brightness` | 0.0–1.0 | 0.35 | — |

### Transpose (STFT) (`transpose_stft`)
STFT phase-vocoder transpose intended for direct comparison with the default Signalsmith-based transpose, tuned for low-latency down-tuning to -12 semitones.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -12..+12 | 0.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |
| `mode` | 0/1 (`Low Latency`/`Polyphonic`) | 0.0 | enum |
| `quefrencyMs` | 0.0–5.0 | 0.0 | ms |
| `timbre` | 0.5–2.0 | 1.0 | ratio |
| `normalize` | 0/1 (`Off`/`On`) | 1.0 | enum |

`mode=Polyphonic` switches to a larger, higher-overlap STFT profile for better chord handling at the cost of higher latency. Keep `quefrencyMs=0` for most polyphonic material; the upstream library notes that cepstral formant preservation is less reliable with polyphonic input.

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

### Signal Analyzer (`input_analyzer`)
Pass-through utility node that renders live diagnostics for the signal entering the node.

| Output | Description |
|--------|-------------|
| dBFS | Live peak/RMS in dBFS |
| Converted units | Peak/RMS as %FS plus RMS dBu/dBV/Vrms |
| LUFS | Live momentary, short-term, and integrated loudness per BS.1770-4 K-weighting |
| Channels | Whether the measured signal is mono or stereo (`channelMode`/`stereo` + `activeChannelCount`). Two channels carrying identical/dual-mono content report as mono; distinct L/R content reports as stereo |
| Spectrogram | Rolling FFT-based spectrogram of node input audio |
| Bark perception | 24-band Bark critical-band energy visualization for perceptual frequency weighting insight |

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

Framework-specific effects, such as the JUCE plugin host, may live in the adapter layer instead. They should still use a stable UUID from `EffectGuids.h` and register with `EffectRegistry` before presets or the effect catalog are loaded.

> **Renaming an existing effect?** The UUID stays the same — just update `info.displayName`. Add the old alias string to `info.aliases` if it was previously used in preset JSON.

## See Also
- [Signal Chain](signal-chain.md) — How effects execute in the graph
- [Data Models](data-models.md) — ResourceRef and preset schema
- [User Interface](user-interface.md) — Effect browser UI
- [Composite Effects](composite-effects.md) — Bundling effects into reusable composites
