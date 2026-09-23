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
| `kDelayTape` | `c46cecdc-d800-416e-a9cf-b9a13bd3ab35` | `delay_tape` |
| `kDelayAnalog` | `5c3965fc-cf52-4d14-9e5a-8a7120a9aae4` | `delay_analog` |
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
| `kRingMod` | `c13068c1-9c50-4c7c-be9e-eef808990651` | `ring_mod` |
| `kAutoWah` | `b06c6d84-01b3-4d0a-ad98-40eecb64438e` | `auto_wah` |
| `kWah` | `8ae7a185-8075-466f-a83b-72f8dfa50af0` | `wah` |
| `kSpatial3D` | `a3196960-a89b-4388-829e-cbf8d8dd91c3` | `spatial_3d` |
| `kPitchShift` | `0c15f065-8335-4932-9d2f-366d436ec30a` | `pitch_shift` |
| `kTranspose` | `9b89cc46-e05b-4f06-981e-1d74d1f628cf` | `transpose` |
| `kTransposeStft` | `66b3a43a-72eb-4c7a-9c47-50e9ab24b718` | `transpose_stft` |
| `kOctave` | `2e4d5380-5a79-412f-bfc0-bf84ef74d561` | `octave` |
| `kGain` | `0bcd895e-5d36-4247-a351-6bed1fcb37a8` | `gain` |
| `kSynthSaw` | `608e846e-0e60-4064-9c83-37c0df573c38` | `synth_saw` |
| `kGuitarToMidi` | `c2b0fdc1-ba9a-411c-8d19-4f6eeab86e33` | `guitar_to_midi` |
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

### Parameter tapers
A parameter's taper (`ParameterDef::taper`, `core/src/dsp/ParamTaper.h`) decides how control
travel maps onto its range. It is linear unless the effect declares otherwise. A **log** taper
spaces the range by ratio, so each stretch of travel covers the same number of octaves: on the
ring modulator's 1–2000 Hz Frequency, 1–100 Hz takes 60% of the sweep instead of 5%, and half
travel is the geometric mean, 44.7 Hz. It applies everywhere a 0..1 position meets the range:
the knob's drag, wheel and indicator (`core/ui/ts/paramTaper.ts`), a custom layout's slider,
and MIDI and DAW automation of a `node.*` slot (`AutomationSlotTable`). Values are stored and
sent in native units either way, so declaring a taper changes no preset; it does change where
existing automation lands.

Declare it with `LogTaper({...})` on an `EffectParamSpec`, which fails the build if the range is
not positive and increasing, or `WithLogTaper({...})` on an inline `ParameterDef`, which
`AutomationNodeParamRangeTests` checks for every registered effect. The effect catalog sends
`"taper": "log"` only for a tapered parameter, so an older UI sees the catalog it always did. A
composite's exposed parameter with `"curve": "log"` gets the log taper; `"exp"` stays linear.
Over a range it cannot map (a node narrowing it, a composite declaring one), a log taper falls
back to linear on both sides.

Log-taper parameters: `ring_mod` `frequency`; `delay_digital` `highCut` and `lowCut`; `cab_ir`
`lowCutHz` and `highCutHz`; `reverb_advanced` `lowCut` and `highCut`.

## Effect Categories

| Category | Description | Examples |
|----------|-------------|----------|
| `amp` | Amplifier simulation | NAM amp models, Neural FX |
| `cab` | Cabinet simulation | IR convolution, simple cab |
| `drive` | Gain/clipping/saturation | Overdrive, distortion, fuzz |
| `dynamics` | Dynamics processing | Noise gate, compressor, limiter |
| `eq` | Equalization | Parametric EQ |
| `modulation` | Modulation effects | Chorus, flanger, phaser, tremolo, ring modulator, auto-wah, wah |
| `pitch` | Pitch manipulation | Pitch shift, transpose, octave |
| `delay` | Time-based delay | Digital delay, tape echo, analog (BBD) delay, doubler |
| `reverb` | Reverberation | Room, chamber, spring, advanced, IR, ambient |
| `utility` | Utility processing | Gain, splitter, mixer, signal analyzer |
| `synth` | Synthesized tones, and notes for them | Synth saw, Guitar to MIDI (experimental) |

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

### Where SetParam runs

`SetParam` can run on the audio thread. MIDI and DAW automation apply there, under the DSP lock
(`PluginController::ProcessQueuedMidi`), and any node parameter can be bound to an automation
slot. So `SetParam` must not allocate, lock or rebuild. Nor can a rebuild move to the message thread
under the lock: the audio thread outputs silence for any block that finds the lock held.

A parameter whose change needs a rebuild records the new value in `SetParam` and calls
`DeferredRebuild::NoteRequested()` (`core/src/dsp/DeferredRebuild.h`). The message thread finishes
it in `PluginController::ApplyDeferredNodeRebuilds`, which the node-param handler and the idle loop
call:

1. Under the lock, `TakeDeferredRebuild()` returns the work, with what the build reads copied into
   it, or shared with it when it is large and never changed in place (the reverb's impulses).
2. Off the lock, `DeferredRebuild::Build()` does the expensive part.
3. Under the lock again, `CommitDeferredRebuild()` swaps the result in, in O(1). It drops the work
   if the effect was rebuilt in between, and leaves what it replaces in the work, to be freed after
   the lock is released.

A composite forwards both calls to its inner graph. The IR Cabinet's Normalize and Low Latency, and
the Convolution Reverb's Quality and Low Latency, work this way.

## Built-in Effect Types

### NAM Amp (`amp_nam`)
Neural amp model processing.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `inputGain` | -24..+24 | 0.0 | dB |
| `outputGain` | -24..+24 | 0.0 | dB |

**Resource**: NAM model file (`.nam`)

#### NAM quality (per-instance setting)

NAM quality is **not** a preset parameter. It lives under Settings -> DSP
Performance -> NAM Processing Quality and applies to every NAM node (`amp_nam`,
the optimized NAM amp, NAM FX, and NAM Blend) in every preset and mixer slot of
**one plugin instance**.

| App setting | Range | Default |
|-------------|-------|---------|
| `audio.nam.slimmableSize` | 0.0-1.0 | `1.0` |
| `audio.nam.oversampling` | index 0-5: Off, 2x, 4x, 8x, 16x, 32x | `0` (Off) |
| `audio.nam.antiAliasPhase` | index 0-2: Minimum Phase, Linear Short, Linear Long | `0` (Minimum Phase) |

**Ownership.** A DAW loads every plugin instance into one process, so these
settings are deliberately *not* stored in process-wide globals — two instances in
one project can sit at different tiers.

- **Plugin**: the values belong to the instance. They are saved in host state
  (`SerializeState`, `state["namQuality"]`) and restored with the project.
  `app.json` only seeds a brand-new instance; instances never write to it, and
  `ReloadSharedSyncSourcesFromDisk()` re-asserts the instance's own values so the
  cross-instance settings sync cannot overwrite them.
- **Standalone**: unchanged — `app.json` owns them.

**Offline rendering.** A DAW flips `AudioProcessor::setNonRealtime()` around a
bounce, freeze, or export; `PluginProcessorAdapter` forwards that to
`PluginController::SetOfflineRendering()`. With no realtime deadline to protect,
`ApplyOfflineRenderBoost()` renders at full quality: slimmable size goes to
maximum and oversampling is lifted to at least 2x. Both are *floors* — a user
already running 8x keeps 8x — and the anti-alias phase is left alone, since the
host compensates its latency either way and changing it would alter the rendered
phase response. The user's stored settings are never modified, so returning to
real time restores their live tier. Note that the boost can change reported
plugin latency (Off has no resampler at matched rates; 2x does), which is why
`SetOfflineRendering()` re-reports latency to the host.

**Delivery.** `PluginController::ApplyNamQualitySettings()` sanitizes the values
and pushes them through `MultiPresetMixer::SetNodeTypeConfigDefault()` for the
four NAM node types. `SignalGraphExecutor` keeps these as *node-type config
defaults*: they are applied to existing nodes immediately and re-applied in
`CreateProcessors()` to nodes built later, so a preset switch or a new mixer slot
inherits the instance's tier. A node's own `config` entry still wins. Defaults
are forwarded into `CompositeEffectProcessor`'s inner executor as well, so NAM
nodes nested inside a composite are covered. Because oversampling and the AA
filter both change the resampler's reported latency, applying them also
re-reports plugin latency to the host.

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

### NAM Blend (`amp_nam_blend`)
Plays a set of NAM models captured at different settings of one amp or pedal, and
picks and mixes them from the node's knobs. The set is a blend definition in the blend
library (see [features.md §7](features.md) and [data-models.md](data-models.md)); a
node names it with `config.blendId`, and the controller fills the node's resources in
from the definition every time it builds a chain (`ApplyBlendDefinitions`,
`controller/internal/BlendSupport.cpp`). Presets never store the models themselves.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `blend` | 0..1 | 0.0 | amount |
| `inputGain` | -24..+24 | 0.0 | dB |
| `outputGain` | -24..+24 | 0.0 | dB |
| `mix` (Advanced) | 0..1 | 1.0 | amount |
| `useCalibration` (Advanced) | toggle | 1 | |
| any captured parameter (`gain`, `bass`, …) | 0..1 | median of the captures | normalised |

| Config | Meaning |
|--------|---------|
| `blendId` | The blend definition the node plays |
| `blendMode` | The definition's mode, `interpolate` or `snap`; rewritten from the definition on every build |
| `blendModeOverride` | The node's own mode, which wins; empty follows `blendMode` |

**Which models play.** Each model carries the settings it was captured at, normalised
to 0..1 (`ResourceRef.parameters`). When the node has a value for any of those
parameters, the engine takes the two models nearest by squared distance over the
parameters some model was captured at (a missing value costs 4) and weights them by
inverse distance. A value for a parameter no model was captured at is ignored, so a
parameter dropped from the blend does not flatten the mix. When the node sets none of
them, the `blend` sweep crossfades the two models either side of its position, each
model sitting at its captured primary value or, with none, at its place in the list.
In snap mode only the nearest plays. A model asked for less than 2% of the mix is
left out, so a knob on a captured setting runs one model. The UI mirrors this rule in
`blendUtils.selectBlendMix()` for the "matched model" readout.

**Mixing.** Changes are never cut. A model coming into the mix fades in over 30 ms and
one leaving fades out; input and output gain changes ramp across a block. A model that
was not running first runs unheard for its prewarm length (the NAM model's own figure
at its native rate, at least 60 ms and at most 250 ms), so it is not heard with stale
history; until one of the models asked for is ready, the current mix holds. Models out
of the mix are not run, and at most four run at once during a fast sweep. In stereo,
left and right run on two threads (`rtparallel::DualLaneExecutor`).

**CPU.** One model when a knob sits on a captured setting or in snap mode, two while
crossfading, briefly up to four during fast sweeps. Oversampling and slimmable size
follow the per-instance NAM quality above.

**Resource**: NAM model files (`.nam`), one per capture.

### IR Cabinet (`cab_ir`)
Impulse response convolution for cabinet simulation.

A preset's IRs load before its node is prepared (`SignalGraphExecutor::CreateProcessors`, then
`Prepare`), while the effect has only the default rate and block size. So a load before the first
`Prepare` only keeps the impulses, and `Prepare` builds the convolvers once, for the host. An IR
loaded into a prepared node is built straight away. The Convolution Reverb works the same way.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mix` | 0.0–1.0 | 1.0 | — |
| `outputGain` | -24..+24 | 0.0 | dB |
| `speakerDrive` | 0.0–1.0 | 0.0 | — |
| `quality` | 0–3 | 1 | — |

Quality levels: 0=Economy, 1=Standard, 2=High, 3=Full

**Normalize and Low Latency** (`normalizeIR` and `lowLatency`, both on by default) are built into
the convolvers: the normalisation gain into the coefficients, and the latency into the partition
layout. A preset sets them before its IRs load, so loading one rebuilds nothing extra. A change
once IRs are loaded, from the node panel or from MIDI or DAW automation, is not built in `SetParam`
(see *Where SetParam runs*). The message thread builds it off the DSP lock from copies of the
impulses, and swaps it in under the lock with the same 30 ms crossfade an IR change uses. The
outgoing side of that crossfade is the convolvers that were playing, history and all. A change from
the node panel is built before its message returns, and one from automation at the next idle tick.
`GetParam` reports the requested value straight away. The rebuild also applies a Quality change
still waiting for the next IR load. The host hears about the new latency once the rebuild is in.
Normalize is not a declared parameter, so the node panel has no control for it. Presets set it,
and so can a slot address that names it (`node.cab_ir.normalizeIR`).

Speaker Drive is the Simple Cabinet's level-dependent speaker stage (`SpeakerDrive.h`) in front
of the convolution: what an IR, being a linear snapshot, cannot capture. It bends only what
reaches the IR; the dry mix stays clean. At 0 it is exactly transparent.

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

**State recall rules** (see `docs/data-models.md` for where each copy lives):

- Auto-capture depends on the plugin notifying the host, and many plugins change
  non-parameter state silently. Anything that rebuilds the graph therefore folds the live
  state back into the working copy first (`CaptureLiveHostedPluginStateIntoActivePreset`).
- State is per scene. Only the active scene is loaded into the DSP, so a save writes live
  state to that scene and leaves the other scenes' stored chunks alone.
- An empty capture is never authoritative — it is ignored rather than allowed to erase a
  stored chunk.
- Pointing a node at a different plugin drops the previous plugin's chunk and identity keys;
  a chunk is only ever carried across a rebuild when the plugin identity still matches.
- Every mixer slot persists its own state, not just the one holding the editing focus.
- In a DAW, all of this rides in the project via `SerializeState`. In standalone, the store's
  preset stays authoritative for the graph, and only the plugin chunks are recovered from the
  previous session (`RestoreStandaloneHostedPluginState`) — so unsaved *graph* edits are still
  discarded, while the opaque plugin state, which has no other home, survives a restart.

**Runtime notes**: The signal graph routes stereo audio, and notes from a Guitar to MIDI node to every Plugin Host downstream of it (see *Guitar to MIDI*); nothing else reaches a hosted plugin as MIDI, and MIDI a plugin produces goes nowhere. A plugin with no audio input (an instrument) is handed a silent buffer rather than the node's input, so its output is only its own; Mix then blends it with that input.

### Noise Gate (`dynamics_gate`)
Input noise reduction.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `threshold` | -80..0 | -60 | dB |
| `attack` | 0.1–50 | 1.0 | ms |
| `hold` | 0–500 | 50 | ms |
| `release` | 1–500 | 50 | ms |
| `hysteresis` | 0–24 | 4.0 | dB |
| `range` | -90..0 | -80 | dB |
| `stereoLink` | 0–1 | 1 | Independent / Linked |

`thresholdDb`, `attackMs`, `holdMs` and `releaseMs` are accepted as legacy spellings and are
declared as `ParameterDef::aliases`, so `CanonicalizeNodeParams` folds them onto the ids above
when a stored node is loaded. Presets shipped before this change carry the suffixed names.
`GlobalChainEditor::NormalizeConfig` folds the global chain's own config too, not just the
executor's copy of the graph: the config is what the UI is sent and what the settings blob is
written from, so a panel reading `attack` out of a node holding only `attackMs` would show a
default instead of the setting and then write that default back over it.

**The global gate is edited from the control bar.** The GATE toggle and threshold knob are the
whole of it on the bar; the chevron beside the dB readout opens a flyout with every parameter
above, `advanced` ones under their own heading. `ts/gateSettings.ts` builds that panel from the
effect catalogue rather than a list of its own, so a parameter added here appears there with its
range, unit and grouping; each change goes out as a `gate.<id>` `setGlobalChainParam`.

**Attack and release shape the applied gain, not the detector.** The detector runs fast and at
a fixed rate so the gate sees a pick attack the moment it arrives; the user's times control how
the gain ramps between `range` and unity. This is what keeps the close from clicking — the gate
sits in front of the amp, so a gain step at the threshold level would be amplified with the rest
of the signal.

`hysteresis` is how far below `threshold` the level has to fall before the gate may close, which
stops a decaying note chattering it open and shut. `range` is the attenuation when closed rather
than a full mute. `stereoLink` keys one detector off the louder channel so a stereo signal cannot
half-close; turning it off gates each channel on its own level.

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

### Tape Echo (`delay_tape`)

`core/src/dsp/effects/TapeDelayEffect.h`. A tape echo: an Echoplex EP-3's single head, or a
three-head machine's fixed ones. What makes it tape is that the delay time moves:

- **Wow and flutter** are a *speed* error (`TapeTransport.h`), so they are specified as pitch
  deviation and the time swing follows from it. At full depth wow is about 31 cents and
  flutter about 16, and the swing is the same at a 60 ms delay as at 1200 ms.
- **Time changes glide** the read head over `glide`, bending the pitch of what is already on
  the tape. The glide is capped between half and one-and-a-half speed, so it dives at most an
  octave and never plays backwards. `glide` 0 is instant, like the digital delay.
- **Repeats darken and thicken pass by pass**, because the playback EQ (head bump, high/low
  cut, and `age`'s extra HF loss) sits inside the feedback loop.

`time` is the longest head (head 3). Heads 1 and 2 sit at 0.317 and 0.633 of it — an
approximation of a Space Echo's spacing, not taken from a service manual. The multi-head modes
feed back the sum of their heads, which is what builds their patterns.

| Parameter | Range | Default | Unit | Group |
|-----------|-------|---------|------|-------|
| `time` | 20–1500 | 400 | ms | Time |
| `syncMode` | Free / Tempo | Free | enum | Time |
| `syncDivision` | 1/1–1/32T | 1/4 | enum | Time |
| `glide` | 0–2000 | 120 | ms (time to settle) | Time |
| `feedback` | 0.0–1.10 | 0.35 | — | Time |
| `headMode` | Head 3, Head 2, Head 1, 1+2, 2+3, 1+3, 1+2+3 | Head 3 | enum | Heads |
| `wow` | 0.0–1.0 | 0.25 | — | Tape |
| `flutter` | 0.0–1.0 | 0.25 | — | Tape |
| `age` | 0.0–1.0 | 0.35 | — | Tape |
| `saturation` | 0.0–1.0 | 0.30 | — | Tape |
| `highCut` | 500–16000 | 5000 | Hz | Tape (advanced) |
| `lowCut` | 20–800 | 90 | Hz | Tape (advanced) |
| `headBump` | 0.0–1.0 | 0.35 | — | Tape (advanced) |
| `mix` | 0.0–1.0 | 0.30 | — | Output |
| `level` | -12–+12 | 0 | dB (repeats only) | Output |
| `spread` | 0–50 | 0 | ms | Output (advanced) |
| `ducking` | 0.0–1.0 | 0 | — | Output (advanced) |

- `age` lowers `highCut` toward 35% of itself, adds occasional dropouts and adds hiss. Hiss is
  gated on the input, so a worn tape nobody is playing into is silent.
- `feedback` above 1 self-oscillates, bounded by the saturator and the record-path rail; more
  feedback, louder runaway. The head bump is a boost inside the loop, so the feedback tap is
  divided by the playback EQ's measured peak — below 1 the loop always decays.
- Factory presets: Echoplex EP-3 (default), Slapback, Three Heads, Worn Tape, Dub.

### Analog Delay (`delay_analog`)

`core/src/dsp/effects/AnalogDelayEffect.h`. A bucket-brigade delay. A BBD's delay is
`stages / (2 × clock)`, so on a real pedal the Time knob is the clock — and the device's own
Nyquist, `clock / 2`, moves with it. The reconstruction filter tracks the clock, so **repeats
lose bandwidth as Time rises**, and fewer stages at the same Time lose more. Measured at 4096
stages: 7.4 kHz at 50 ms, 2.3 kHz at 300 ms, 1.2 kHz at 600 ms; 1024 stages at 300 ms, 670 Hz.

Not modelled: the line runs at host rate rather than being resampled to the clock, so there is
no clock aliasing or whine. The reconstruction filter adds about 0.19 ms of its own lag, as a
real pedal's does.

| Parameter | Range | Default | Unit | Group |
|-----------|-------|---------|------|-------|
| `time` | 20–800 | 320 | ms | Time |
| `syncMode` | Free / Tempo | Free | enum | Time |
| `syncDivision` | 1/1–1/32T | 1/4 | enum | Time |
| `glide` | 0–500 | 40 | ms (time to settle) | Time (advanced) |
| `feedback` | 0.0–1.15 | 0.35 | — | Time |
| `stages` | 1024 (MN3007), 2048 (MN3008), 3328 (MN3011), 4096 (MN3005) | 4096 | enum | BBD |
| `tone` | 0.0–1.0 | 0.5 | — (0.8–8 kHz) | BBD |
| `compander` | 0.0–1.0 | 0.7 | — | BBD |
| `saturation` | 0.0–1.0 | 0.35 | — | BBD |
| `noise` | 0.0–1.0 | 0.2 | — | BBD (advanced) |
| `modRate` | 0–8 | 0.4 | Hz | Modulation |
| `modDepth` | 0–12 | 0 | ms | Modulation |
| `mix` | 0.0–1.0 | 0.3 | — | Output |
| `level` | -12–+12 | 0 | dB (repeats only) | Output |
| `spread` | 0–50 | 0 | ms | Output (advanced) |
| `ducking` | 0.0–1.0 | 0 | — | Output (advanced) |

- `compander` is an NE571-style pair (`Compander.h`): transparent on a steady signal from
  -46 to 0 dBFS at any amount, but its release makes repeats breathe and the noise floor pump
  after a note, and it lowers the idle noise floor by about 20 dB. The repeats are fed back in
  the compressed domain, before the expander, which keeps the compander out of the loop's gain.
- The BBD noise floor is gated on the input, so an idle delay is silent.
- `feedback` above 1 self-oscillates; more feedback, louder runaway. With `saturation` at 0
  only the device rail bounds it, and a runaway at 1.15 reaches about +6 dBFS.
- `clockHz` can be read with `GetParam` for the resolved BBD clock.
- Factory presets: DM-2 (default), Memory Man, Short Analog, Clean Analog, Runaway.

### Convolution Reverb (`reverb_ir`)
Impulse response reverb (`IRReverbEffect.h`). Takes a mono, stereo or true-stereo (4-channel) IR.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mix` | 0.0–1.0 | 0.3 | — |
| `outputGain` | -24..+24 | 0.0 | dB |
| `tone` | 0.0–1.0 | 1.0 | — |
| `lowLatency` | 0/1 | 1 | toggle |
| `quality` | 0–3 | 3 | — |

Quality sets how much of the IR is kept: 0=Economy (1.5 s), 1=Standard (3 s), 2=High (6 s),
3=Full (all of it). A tier shorter than the IR cuts it 256 samples after 99.9% of its energy
instead, when that comes first. The last 2048 samples kept are faded out. The IR is resampled to
the host rate once per IR and rate, and normalised by its L2 norm, anchored to 48 kHz so the level
does not change with the host rate.

**Quality and Low Latency** are built into the convolvers: where the IR is cut, and so its
normalisation gain, into the coefficients, and the latency into the partition layout. A preset sets
them before its IR loads, so loading one rebuilds nothing extra. A change once the IR is loaded,
from the node panel or from MIDI or DAW automation, is not built in `SetParam` (see *Where SetParam
runs*). The message thread builds it off the DSP lock and swaps it in under the lock. The work
shares the resampled impulses rather than copying them, since a reverb IR runs to seconds and the
work is taken under the lock. There is no crossfade: the new convolvers start with no history, so
the tail of what was playing stops at the swap. A change from the node panel is built before its
message returns, and one from automation at the next idle tick. `GetParam` reports the requested
value straight away, and the host hears about the new latency once the rebuild is in.

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
| `lowCut` | 20–1200 | 140 | Hz, log taper |
| `highCut` | 1000–20000 | 7600 | Hz, log taper |
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
Filter-based cabinet with no IR required: five cabinet types, a mic with type, position and
distance, an optional second mic, speaker drive and stereo spread. Use an IR cabinet when one
particular speaker and microphone's exact notches matter; the Simple Cab reaches their broad
shape, and can be matched to an IR from the library (below).

The voicing is a pure model (`SimpleCabVoicing.h`) that the effect runs and everything else
asks: the panel's response curve, Auto Level, IR export and IR matching all use the same
response, so none of them can disagree with what is heard. The defaults are the original 4x12
voicing exactly, so presets saved before these controls existed sound the same.

| Parameter | Range | Default | Unit | Notes |
|-----------|-------|---------|------|-------|
| `cabinet` | 0–4 | 0 | enum | 4x12 Closed, 1x12 Open, 2x12 Open, 2x12 Closed, 4x10 Open. Types are level-matched at their default settings |
| `size` | 0.0–1.0 | 0.5 | — | Moves the low resonance and low cut together, ±half an octave; higher is a bigger box |
| `bass` | 0.0–1.0 | 0.5 | — | Height of the low resonance, and depth of the low cut |
| `mids` | 0.0–1.0 | 0.5 | — | ±6 dB bell at the cabinet's low-mid frequency (450–700 Hz) |
| `presence` | 0.0–1.0 | 0.5 | — | Upper-mid peak, 2–4.5 kHz depending on cabinet |
| `brightness` | 0.0–1.0 | 0.5 | — | Speaker roll-off frequency (sixth order) |
| `micType` | 0–2 | 0 | enum | Dynamic (the reference), Ribbon (dark top, more proximity bass), Condenser (extended top) |
| `micPosition` | 0.0–1.0 | 0.5 | — | 0 = dust-cap centre (bright, hard presence), 0.5 = cap edge, 1 = cone edge (dark, fuller low mids) |
| `micDistance` | 0.0–1.0 | 0.0 | — | 0 to 1 m (quadratic). Loses the close-up bass boost and adds the floor bounce's comb |
| `speakerDrive` | 0.0–1.0 | 0.0 | — | Level-dependent cone excursion and voice-coil compression (`SpeakerDrive.h`); transparent at 0 and at low level |
| `outputGain` | -24..+24 | 0.0 | dB | |
| `autoLevel` | 0/1 | 0 | toggle | Holds loudness to the default voicing's, so tone moves don't change level |
| `mic2Blend` | 0.0–1.0 | 0.0 | blend | Mic 1 (A) to mic 2 (B). Mic 2 costs nothing until blended in (advanced) |
| `mic2Type` | 0–2 | 1 | enum | Advanced |
| `mic2Position` | 0.0–1.0 | 0.5 | — | Advanced |
| `mic2Distance` | 0.0–1.0 | 0.0 | — | Advanced. The time between the two mics is part of the blend's sound |
| `mic2Polarity` | 0/1 | 0 | toggle | Inverts mic 2 (advanced) |
| `spread` | 0.0–1.0 | 0.0 | — | Voices the two sides like two speakers of one cab, so a mono input comes out stereo (advanced) |
| `mix` | 0.0–1.0 | 1.0 | — | Dry blend, kept for older presets (advanced). The dry signal is not time-aligned with the cabinet's, so a partial mix combs: at 50% there is a ~15 dB notch around 3.5 kHz, which the response curve shows. Blend a second mic instead |

**Panel tools.** The params panel draws the response curve the engine reports
(`getEffectResponse`) over the live spectrum of the cab's input, and offers **Export as IR**
(`exportEffectAsIr`: a 48 kHz IR into the library, mono unless spread makes the sides differ;
Speaker Drive, being level-dependent, is not captured) and **Match** (`matchSimpleCabToIr`:
tries every cabinet and mic type and searches the tone controls for the closest 1/6-octave
shape to a library IR, keeping the node's Output, Auto Level and Speaker Drive).

Seven factory presets ship with it; the first, **Closed 4x12**, is the defaults and starts new
nodes. Presets set every control except Output.

### Drive Pedals (`overdrive`, `distortion`, `fuzz`)
Three effects, each a family of classic circuits behind a **Model** switch: the overdrives,
distortions and fuzzes from [The 25 Most Important Guitar Pedals](https://joshuaheathscott.substack.com/p/the-25-most-important-guitar-pedals).
Each model is built from the original's topology and component values: where its gain goes in
frequency, where its diodes or transistors sit, and its tone circuit. The model tables are in
`OverdriveEffect.h`, `DistortionEffect.h` and `FuzzEffect.h`. They share one chassis
(`DrivePedal.h`), a set of analog stages (`DriveStages.h`) and an oversampler (`HalfBandIir.h`).

What all three share:

- **They work in volts.** A signal at the nominal operating level (Settings) stands for a
  humbucker strummed firmly, 0.25 V RMS, so each model clips where the hardware does. Soft
  playing or the guitar's volume rolled back cleans the overdrives up.
- **Level is calibrated.** With Level at 0 dB, every model at every Drive setting is as loud
  as bypass (K-weighted) for a guitar at the nominal level, so switching a pedal on changes
  the tone, not the volume. To push an amp, raise Level, as you would on a real pedal. The
  LPB-1 is the exception: it is a boost, and Drive is how much.
- **Oversampled and antialiased.** The clipping runs near 384 kHz: 8x at 44.1 or 48 kHz, 4x
  at 88.2 or 96, 2x above. Every clipping curve has a closed-form integral, and is
  antialiased with it (ADAA). On a 1.3 kHz note at full drive, audible aliasing is at least
  60 dB below the signal on every model, and past 78 dB on most. The half-bands are
  minimum-phase IIR, about 5 samples of delay, so the pedals report no latency.
- **No DC.** The outputs are AC-coupled like the hardware's, so asymmetric clipping comes out
  as even harmonics and never as an offset that thumps or biases the next stage.
- **Even harmonics where the hardware makes them.** Op-amp gain stages are solved as the
  feedback loops they are (`ProcessOpAmpStage`): once the output reaches a rail, the gain legs'
  capacitors hold the output's average against the input, so rails that sit unevenly about the
  bias make the stage switch off-centre. That is where the RAT's and Distortion+'s even
  harmonics come from, and the Centaur's at high gain. The DS-1's come from its booster
  transistor running off-centre, and the Fuzz Face's from its DC feedback. Each was fitted to
  NAM captures of the pedal: the RAT holds a 45% duty cycle and a 2nd harmonic 15 dB down at
  every level, as its capture does, and the Fuzz Face's 2nd harmonic comes to within about 4 dB
  of the fundamental at the nominal level, as a capture's does. The TS-808's stays symmetric, as a TS9 capture's (50 dB
  down) says it should.
- **Click-free.** Knobs glide over 25 ms. Model and Clipping change the circuit outright, so
  the wet signal fades out over 8 ms, the change lands in silence and it fades back in.
  **Mix** blends in phase: the dry signal takes the same filter round trip as the wet one.
- **Cost.** A mono path; 9–31 µs per 64-sample block at 48 kHz depending on the model,
  about 1–2% of a 64-sample deadline.

**Overdrive** — `drive`, `tone`, `level` as on the pedal; `bass` moves the bass cut ahead of
the clipping two octaves either way; `clipping` swaps the diodes.

| Model | Circuit |
|-------|---------|
| TS-808 | Tube Screamer. Only mids above 720 Hz reach the 1N914s in the op-amp's feedback, and the clean signal sums on top; 21–41 dB of gain; fixed 723 Hz tone stage. The mid hump |
| Centaur | Klon. Clean and clipped paths summed, the clean turned down as gain rises; the gain climbs steeply early in the knob's travel from a clean boost at 0; germanium diodes to ground, rolled off at 1.6 kHz; Tone is the Treble shelf. Fitted to captures of a Behringer Centaur at five gain settings |
| Bluesbreaker | Marshall's "amp in a box": the TS layout with the bass left in and less gain, its clipped path rolled off at 1.75 kHz; a gentle tilt tone. Symmetric, as captures of an original and a reissue are |
| Timmy | Transparent: a low-gain stage into silicon diodes to ground, flat mids; Tone is the cut-only Treble |
| Fulldrive | A TS with more gain and fuller bass ("flat mids"); a treble roll-off tone |
| LPB-1 | One transistor, up to +24 dB of full-range boost. Clips only when pushed near its 9 V supply |

**Distortion** — `drive`, `tone` (the RAT's Filter, bright clockwise), `level`; `tight` sets
how much bass reaches the gain stage (clockwise is tighter); `clipping`; and a three-band EQ,
`low` (±15 dB shelf at 100 Hz), `mid` (±15 dB bell at `midFreq`, 200–5000 Hz) and `high`
(±15 dB shelf at 5 kHz), flat at 0 dB on every model and the Metal Zone's own controls.

| Model | Circuit |
|-------|---------|
| RAT | LM308 gain stage with two RC legs: up to 45 dB in the mids and 67 dB above 1.5 kHz, pulled back by the op-amp's bandwidth; 1N914s to ground; the Filter. Its uneven rails give it a steady 2nd harmonic |
| DS-1 | Transistor booster, op-amp, silicon diodes to ground, and a low-pass/high-pass blend tone stack that scoops the mids at noon. The booster saturates unevenly, so even harmonics grow as you play harder |
| Distortion+ | A 741 gain stage into germanium diodes to ground; raspy. Its rails sit very unevenly on 9 V, so it switches off-centre with a strong 2nd harmonic. No tone control. Fitted to two MXR units, Distortion 0–10 |
| Metal Zone | Mid pre-emphasis, a soft first clipping stage, then a second stage whose gain Dist also sets (the dual-gang pot), a steep roll-off, and the parametric EQ. Nearly clean at Dist 0, saturated by noon. Fitted to two MT-2s with the EQ at noon |

**Fuzz** — `drive` (labelled Fuzz), `tone`, `level`; `bias` moves the drive transistor's
operating point (below noon it starves, gating and sputtering like a dying battery; above,
it runs hot and compressed); `bass` sets how much low end reaches the circuit.

| Model | Circuit |
|-------|---------|
| Fuzz Face | Two germanium transistors: round on one side, clipped on the other, full bass, and the low input impedance softening the pickup. Its DC feedback moves the switching point as it clips, so the 2nd harmonic nears the fundamental. Cleans up as the signal falls |
| Big Muff | Input booster (Sustain), two clipping stages with diodes in their feedback, and the 408 Hz / 1.8 kHz tone stack that scoops the mids by about 9 dB at noon. Bias leans the booster, trading odd harmonics for even |
| Tone Bender | MkII: a third germanium stage ahead of a Fuzz Face pair; more gain, tighter bass, more compression |
| Fuzz-Tone | Maestro FZ-1: germanium on a 1.5 V supply, biased near cutoff. Brassy, thin, and gated as notes decay |
| Super-Fuzz | Preamp, full-wave rectifier (the octave up) and a hard stage. Tone is the scoop switch, from none to full; Bias trades the fundamental for the octave |

Where a pedal never had a tone control, Tone is a tilt about 800–1000 Hz that is flat at noon.
Model indices are stored in presets, so new models are appended, never inserted.

| Parameter | Range | Default | Unit | Effects |
|-----------|-------|---------|------|---------|
| `model` | 0–5 / 0–3 / 0–4 | 0 | enum | all |
| `drive` | 0.0–1.0 | 0.5 / 0.6 / 0.7 | — | all; gain tapers like an audio pot, even in dB (the Centaur's climbs steeply early, as its linear gain pot does) |
| `tone` | 0.0–1.0 | 0.5 | — | all |
| `bass` | 0.0–1.0 | 0.5 | — | overdrive, fuzz |
| `tight` | 0.0–1.0 | 0.5 | — | distortion |
| `clipping` | 0–6 | 0 | enum | overdrive, distortion: Stock, Silicon, Asymmetric, LED, Germanium, MOSFET, Open. The level change each brings is mostly undone, so it changes character rather than volume |
| `bias` | 0.0–1.0 | 0.5 | — | fuzz |
| `low` / `mid` / `high` | -15..+15 | 0.0 | dB | distortion |
| `midFreq` | 200–5000 | 800 | Hz | distortion; log taper, so noon is about 1 kHz |
| `level` | -24..+24 | 0.0 | dB | all |
| `mix` | 0.0–1.0 | 1.0 | — | all |

**Older presets keep their loudness.** The earlier pedals came out 13–25 dB above bypass at
Level 0 dB, and presets set Level against that. A stored drive node with no `model` key can
only come from before the Model switch (a node created since carries every parameter), so
reading one (a preset, a composite's inner graph, the global chain) migrates it
(`MigrateLegacyNodeParams`, `DriveLegacyMigration.h`): it gets the first model of its family
(TS-808, RAT, Fuzz Face) explicitly, its Drive, Tone and Mix unchanged, and the Level at which
that model is as loud as the old pedal was at its Drive, Tone and Level. The Level comes from a
table measured against the old pedals, which `DriveLegacyMigrationTests` keeps verbatim
(`helpers/LegacyDrivePedals.h`) and checks against, factory content included; `--calibrate`
re-derives the table if a first model's voicing changes. A migrated node is saved with its
model the next time its preset is, and is not migrated again.

**Recalibrating.** A change to a model's voicing changes its loudness, and so its trim table
(`trimDb`, nine drive points) and its `clipLevelExponent`. `DrivePedalTests` holds every model
to within 1 dB of bypass on a plucked-string guitar phrase; `DrivePedalTests --calibrate` prints
the correction to add to each trim entry and the refitted exponent.

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

### Wah (`wah`)

A conventional, foot-controlled wah: a resonant bandpass swept by **Pedal Position**. Map
that parameter to an expression pedal, a MIDI CC or host automation (right-click the control
for MIDI Learn). Every other parameter describes the pedal, so a factory preset is a *model*
of a wah rather than a setting of one. The envelope-driven version is `auto_wah`.

**Signal path, per channel**

```
in ─┬─ resonant bandpass (TPT state-variable filter) × peak gain ──┐
    └─ one-pole lowpass at 300 Hz (non-resonant bleed) × Low End ──┴─ + ─ treble shelf at 2.5 kHz ─ × Level ─ mix
```

| Behaviour | How |
|---|---|
| Sweep | The centre frequency moves exponentially from Heel Freq to Toe Freq, so equal pedal travel is an equal musical interval. Taper warps travel as `position^(3^-taper)`: negative holds the sweep low until late in the travel, as the stock Cry Baby does (-0.25 puts mid-pedal near 750 Hz on a 450–1600 Hz sweep); positive gets there early. |
| Resonance | Q moves geometrically from Q at the heel to Q × Toe Q Scale at the toe. Julius Smith's fit to a measured GCB-95 runs from Q 8 to Q 2. |
| Peak gain | `2·√Q`, tilted by Toe Gain (dB, applied progressively along the travel). A unity-peak bandpass passes pink-noise power in proportion to 1/Q whatever its frequency, so pink noise — close to a guitar's long-term spectrum — holds a steady level across both the sweep and the Q knob. Where Q falls toward the toe, the heel is the taller peak: 6 dB on the GCB-95 curve, in line with Holters and Zölzer's measured 6–8 dB. |
| Saturation | The filter's damping rises with the square of the resonant stage's own output, so a hot input flattens and widens the peak and adds odd harmonics, as a saturating inductor or a clipping transistor stage does. |
| Pedal smoothing | Frequency, Q and gain glide in the log domain with the Response time constant. That removes the zipper noise of a 7-bit MIDI CC, and doubles as the lag of an optical (LDR) wah. Level, Mix, Low End and Treble smooth over a fixed 10 ms. |
| Auto-Engage | For controllers without a toe switch. The wah fades in over 25 ms as soon as the pedal leaves the heel, and fades back to the dry signal once the pedal has rested at or below 4% travel for 400 ms, so a quick heel-toe rock never cuts out. A wah loaded with its pedal parked at the heel starts off. |

The trapezoidal (topology-preserving) state-variable filter stays stable and free of
artefacts while its coefficients change every sample, which a direct-form biquad does not.
Centre frequencies are clamped to 0.45 × the sample rate. Latency is zero.

**Parameters**

| Parameter | Range | Default | Unit | Group |
|-----------|-------|---------|------|-------|
| `position` | 0–1 | 0.5 | heel → toe | Pedal |
| `response` | 1–150 | 12 | ms | Pedal (advanced) |
| `autoEngage` | 0/1 | 0 | toggle | Pedal |
| `heelFreq` | 150–1000 | 440 | Hz | Voicing |
| `toeFreq` | 600–5000 | 2000 | Hz | Voicing |
| `taper` | -1…1 | -0.25 | — | Voicing (advanced) |
| `q` | 0.5–20 | 8 | Q at the heel | Voicing |
| `toeQScale` | 0.1–2 | 0.25 | × | Voicing (advanced) |
| `toeGain` | -12…12 | 0 | dB | Voicing (advanced) |
| `lowEnd` | 0–1 | 0.15 | — | Voicing |
| `treble` | -12…12 | 0 | dB | Voicing |
| `saturation` | 0–1 | 0.2 | — | Voicing (advanced) |
| `level` | -12…18 | 0 | dB | Output |
| `mix` | 0–1 | 1.0 | — | Output |

The defaults are the Cry Baby GCB-95 voicing, which is also the preset a new node starts
with. Read-only feedback: `currentFrequency`, `currentQ`, `engaged`.

**Factory presets** set every voicing parameter plus `mix`, and deliberately leave out
`position` and `autoEngage`. Those belong to the player's controller, so loading a voicing
never moves the pedal or switches the wah off underfoot. The presets are voiced
approximations of each pedal, not circuit captures; where a sweep range is marked estimated,
no published figure was found.

| Preset | Heel–toe (Hz) | Basis |
|---|---|---|
| Cry Baby GCB-95 | 440–2000 | Dunlop spec (350–450 Hz heel, 1.5–2.5 kHz toe); Smith's measured fit (Q 8 → 2); ElectroSmash's mid-pedal frequency for the taper |
| Vox V847 | 450–1600 | ElectroSmash circuit analysis; medium-high Q, slight level drop |
| Vox Clyde McCoy '67 | 420–1700 | Sharpest Q of the classics, ICAR taper, rounder bass (reviewer consensus); range estimated |
| Joe Satriani Big Bad Wah | 430–1700 | Vox's Wah 1 voice: a hot-rodded V847 with lower Q and more lows (reviews); Vox publishes no frequencies, so range estimated |
| Thomas Organ Cry Baby '68 | 300–1450 | Estimated from the JH1D, which Dunlop bases on it; smooth, lower Q |
| Cry Baby 535Q | 440–2200 | Dunlop spec, range 1; Q knob high; boost +6 dB |
| Cry Baby 95Q | 390–2000 | Dunlop spec; boost +3 dB |
| Jimi Hendrix JH1D | 300–1450 | Dunlop spec (290–310 Hz, 1.40–1.51 kHz, +16.5 dB at both ends); quick-ramping pot |
| EVH95 Eddie Van Halen | 340–2100 | Dunlop spec (300–380 Hz, 1.9–2.3 kHz, +20 / +21 dB); high-Q inductor |
| Slash SW95 | 320–1700 | Dunlop spec (270–370 Hz, 1.5–1.9 kHz); lush top, drives with its boost |
| Dimebag Cry Baby From Hell | 295–1400 | Dunlop spec, range 5; Q high; boost +6 dB |
| Kirk Hammett KH95 | 340–1600 | Dunlop spec (300–380 Hz, 1.4–1.8 kHz; +17 dB heel, +21 dB toe, hence Toe Gain +4.5) |
| Zakk Wylde ZW45 | 300–1900 | Dunlop spec (250–350 Hz, 1.4–2.4 kHz, +17 dB at both ends); GCB-95 Q; thick lows, a less harsh top |
| Jerry Cantrell JC95 | 355–1450 | Dunlop spec (320–390 Hz; toe 1.05–2.07 kHz on its Fine Tune knob, here at noon; +18 dB heel, +20 dB toe); dark |
| Buddy Guy BG95 | 340–1700 | Dunlop spec for the BG voice (290–390 Hz, 1.5–1.9 kHz, +16 dB); warm, medium Q estimated |
| John Petrucci JP95 | 220–1350 | Dunlop spec (200–240 Hz, 1.2–1.5 kHz); wide Q, fat lows and rounded highs (review); the internal volume and EQ trims are estimated |
| Joe Bonamassa JB95 | 300–1450 | Dunlop spec (290–310 Hz, 1.40–1.51 kHz, +16.5 dB); smooth Halo-inductor Q; buffered, so a slightly darker top |
| Cry Baby CM95 Clyde McCoy | 410–2200 | Dunlop spec (410 Hz–2.2 kHz, up to +18 dB); smooth, lower Q (Halo inductor); its low input impedance softens the top |
| CAE MC404 | 400–2050 | Dunlop spec with the yellow inductor (400 Hz, 1.9–2.2 kHz, +16 dB); boost off |
| Cry Baby 105Q Bass | 180–1800 | Dunlop spec (+25 dB heel, +32 dB toe, hence Toe Gain +7.5); bass retained |
| Morley Power Wah | 250–3200 | Optical: wide, low Q, 45 ms lag, no inductor saturation. Morley's own 25 Hz–4 kHz claim is trimmed |
| Steve Vai Bad Horsie | 375–1900 | Optical, medium-high Q, chewy vocal mids (reviews); Morley publishes no frequencies, so range and a 35 ms lag are estimated |
| Steve Vai Bad Horsie 2 · Contour | 400–2100 | Contour mode with the knob at noon and Level +3 dB: fewer mids, a peakier toe (reviews); range estimated |
| Mark Tremonti Power Wah | 450–1500 | A narrower, modern sweep with the harsh top removed (reviews); boost at +4 dB of its 0–20 dB; range estimated |
| George Lynch Dragon 2 | 300–2400 | Wah mode with Level +3 dB; voiced to cut through high gain (reviews); range estimated |
| Ibanez Weeping Demon | 380–2300 | A state-variable filter mixing bandpass and lowpass, so a flat low end (DAFx-15 analysis); range estimated |
| Fulltone Clyde Deluxe · Jimi | 400–1650 | Fulltone manual's late-'60s Clyde mode; range estimated |
| Fulltone Clyde Deluxe · Wacked | 280–1900 | Fulltone manual's Colorsound-like deep-bass mode; range estimated |
| Real McCoy RMC3 | 420–1800 | Thomas brown inductor, ICAR taper, Low knob (Analog Man); range estimated |
| Xotic XW-1 | 400–1900 | Xotic manual: '67–'68 Clyde basis, bias, bass and treble controls; range estimated |
| Budda Bud-Wah | 400–1700 | Fasel-style inductor, mid honk, tamed top (reviews); range estimated |
| Colorsound Wah | 300–2400 | Wide sweep, deep bass, long throw (reviews); range estimated |
| Maestro Boomerang | 350–1500 | Reverse-log pot, low-mid hump (seller descriptions); range estimated |
| Mission ReWah Pro | 330–2500 | Oversized inductor core: wider range, more bass, little saturation (review) |

Sources: the Dunlop product manuals (jimdunlop.com/content/manuals); J. O. Smith's Faust
`crybaby` model and his LAC 2008 paper; Holters and Zölzer, *Physical modelling of a wah-wah
effect pedal*, DAFx-11; ElectroSmash's GCB-95 and V847 analyses; the DAFx-15 paper on the
Ibanez Weeping Demon; the Fulltone, Xotic, Real McCoy and Vox Big Bad Wah manuals and product
pages; and for the Morley signature wahs, Morley's product copy and published reviews. Tom
Morello's TBM95 shares the GCB-95's published specification, so the GCB-95 preset covers it.

### Ring Modulator (`ring_mod`)

Multiplies the signal by a carrier oscillator. With a sine carrier at Fc, every frequency F in
the input becomes the pair F − Fc and F + Fc, and neither F nor Fc is left in the output.
Because the new partials sit a fixed *distance* from the old ones rather than at a fixed ratio,
notes and chords turn into metallic, bell-like or robotic clusters. Mix below 1 brings the dry
signal back, which makes it amplitude modulation. In Tracking mode the carrier follows the note
being played, so the sidebands keep the same musical relation to every note. `RingModEffect.h`
has the processing, `RingModSupport.h` the parameter table, oscillators and presets, and
`dsp/PitchTracker.h` the pitch tracker.

**Signal path, per channel**

```
in ─┬─ DC block (10 Hz) ─ × carrier ─ high-pass (20 Hz) ─ tone low-pass ─ × Level ──┐
    │                         ▲                                                     │
    ├─ pitch tracker ─────────┘ (Tracking mode)                                     │
    └───────────────────────────────────────────────────────────────────────────────┴─ mix ─ out
```

| Behaviour | How |
|---|---|
| Carrier | Sine, triangle or square, phase-aligned so a Waveform change crossfades over 10 ms without cancelling. Each is scaled to unity RMS (sine ×√2, triangle ×√3), so the effect matches its bypass level and the waveforms match each other (measured within 0.1 dB). The cost is crest factor: a sine carrier's output can peak 3 dB above the input's, a triangle's 4.8 dB. |
| Tracking | Mode = Tracking runs the carrier at the pitch being played, moved by Interval (semitones) and Fine (cents), and glides to each new note over Glide (0–200 ms, in the log domain). Intervals whose products stay harmonic are the musical ones. At 0, every partial kF lands on (k ± 1)F, the note's own harmonics (measured: 100% of the output power). A just fifth (7 semitones and 2 cents is 3:2) puts them on odd multiples of F/2, an octave below. The carrier holds the last note through silence and chords, sits at Frequency until a first note is found, and the LFO still sweeps around it. The tracker only listens in Tracking mode, and restarts from a clean history on entering it. |
| Pitch tracker | YIN (de Cheveigné and Kawahara) run every 5 ms on a copy of the mono input, low-passed at 2 kHz and decimated to about 12 kHz, then refined at the full rate by normalised cross-correlation over the few lags around the estimate. The range is 45 Hz to 2 kHz, a guitar's fundamentals and its natural harmonics; a tone whose first YIN dip lies above 2 kHz reads as no pitch, rather than as the subharmonic an octave down. Steady tones read within 0.7 cents at 22.05–192 kHz. On the DI guitar demo it agrees with brute-force full-rate YIN within 10 cents on every frame both find a pitch, with no octave errors. A note change is followed in 15–40 ms. A jump of over a semitone must repeat on two detections before it is accepted, so a single stray octave is ignored, while bends and vibrato are followed directly. The correlation refinement keeps a decaying note's pitch from reading sharp. Frames where a note is dying away, and signals below −55 dBFS, are skipped. A note counts as dying when the newest quarter of the window has under half the power of the same span a whole number of periods earlier; for a steady note the two hold the same part of the waveform, so sawtooths and plucks from 46 to 110 Hz find their pitch on every detection (against the window's mean power, as it was judged before, 73–98% of them). When detections stop finding a pitch, the pitch held goes back past the newest estimate, whose window may already end in a little silence (half a millisecond of it puts a low note several cents out), and past any made on a falling level. So the pitch held is read right wherever the stop falls between detections: within 3 cents after an abrupt stop or a 5–40 ms release, 46 Hz to 1.3 kHz at 44.1–96 kHz, with or without a −70 dBFS noise floor (before, up to 95 cents on a 46 Hz note). Under a −60 dBFS floor a slow release is read into the noise, and the pitch held can be up to 9 cents out. |
| Output high-pass | A second-order Butterworth at 20 Hz on the ring-modulated signal. A partial landing on the carrier frequency multiplies to DC, which in Tracking at Interval 0 is up to 0.7 of the signal. At 80 Hz it is down 0.02 dB. |
| Band-limiting | The square's edges get a two-sample polynomial BLEP and the triangle's corners a BLAMP. At a 2.9 kHz carrier (48 kHz), the harmonics that fold back below 10 kHz, where they would land as inharmonic tones, drop from −17.8 to −59.2 dB for the square and from −43.6 to −83.9 dB for the triangle. What the BLEP leaves sits within a few kHz of Nyquist, which Tone removes. The carrier is held to at most 8 kHz and a quarter of the sample rate. |
| LFO | Sweeps the carrier by up to ±3 octaves (in octaves, so the sweep sounds even around any Frequency). The shapes are sine, triangle, square, and Random, which holds a new value each cycle. The output is slewed over 1 ms, so the square and Random shapes slide rather than step. The rate is free (0.05–20 Hz) or synced to the host tempo. |
| Smoothing | Frequency glides in the log domain over 20 ms, so a MIDI CC does not zipper the sidebands. LFO Depth, Tone, Spread, Level and Mix smooth over 10 ms. Values set before the first block (a preset loading) apply straight away rather than gliding in from the defaults. |
| Tone | A Butterworth low-pass on the wet signal, from 400 Hz at 0 to 24 kHz at 1 (clamped to 0.49 × the sample rate, so fully open is flat across the audio band). |
| Stereo Spread | Runs the right channel's LFO up to half a cycle ahead of the left's, and its carrier up to a quarter of a cycle ahead, so a mono input comes out stereo at about the same level on each side. When Spread returns to zero, the right carrier is phase-locked back onto the left over about a second, as a brief detune of at most 10 Hz rather than a jump. After that the effect offers the executor its mono path again. |

The carrier and LFO are computed every 16 samples and the carrier's phase increment ramps
linearly between updates. A DC offset on the input would otherwise come out as a tone at the
carrier frequency, which is why the DC block is there. A non-finite input sample clears the
filter state at the end of its block, and resets the pitch tracker. Latency is zero. A
64-sample stereo block at 48 kHz costs about 1.5 µs in an optimised build, and about 3.2 µs in
Tracking mode (MSVC `/fp:fast` and clang `-ffast-math` alike), under 0.3% of the block's
deadline. A block that runs a pitch detection costs 8–16 µs at the 99th percentile.

**Parameters**

| Parameter | Range | Default | Unit | Group |
|-----------|-------|---------|------|-------|
| `frequency` | 1–2000 | 440 | Hz, log taper | Carrier |
| `waveform` | Sine / Triangle / Square | Sine | enum | Carrier |
| `mode` | Fixed / Tracking | Fixed | enum | Carrier |
| `interval` | -24…24 | 0 | semitones, whole | Tracking |
| `fine` | -50…50 | 0 | cents | Tracking (advanced) |
| `glide` | 0–200 | 15 | ms | Tracking (advanced) |
| `lfoDepth` | 0–3 | 0 | octaves either way | LFO |
| `lfoRate` | 0.05–20 | 1 | Hz | LFO |
| `lfoShape` | Sine / Triangle / Square / Random | Sine | enum | LFO |
| `syncMode` | Free / Tempo | Free | enum | LFO |
| `syncDivision` | 1/1 … 1/32T | 1/4 | enum | LFO |
| `tone` | 0–1 | 1 | — | Output |
| `spread` | 0–1 | 0 | — | Output (advanced) |
| `level` | -12…12 | 0 | dB (wet) | Output |
| `mix` | 0–1 | 1 | — | Output |

Frequency is on a log taper (see [Parameter tapers](#parameter-tapers)): the growl region below
100 Hz takes the first 60% of the knob's travel, and an expression pedal mapped to it sweeps the
same way. Double-click the value to type an exact one. In Tracking mode Frequency is only the fallback
before a first note. Read-only feedback: `carrierFrequency` (the left carrier, LFO included),
`trackedFrequency` (the pitch the tracker holds, 0 before a first note) and `effectiveRate`
(the LFO rate, after tempo sync).

**Factory presets** set every parameter except `syncDivision`, with Sync off, since each names
its own LFO rate, and Glide at its default. The first holds the defaults, and it is what a new
node starts with.

| Preset | Carrier | LFO | Notes |
|---|---|---|---|
| Classic Ring | 440 Hz sine | — | The defaults |
| Robot Voice | 30 Hz sine | — | The low-frequency growl of the classic TV robot voices |
| Bell Tones | 740 Hz sine | — | Tone 0.8, Mix 0.7: some dry note under the bells |
| Sci-Fi Sweep | 500 Hz sine | Sine, 0.25 Hz, ±1.5 oct | A slow, wide sweep |
| Computer Chatter | 900 Hz square | Random, 8 Hz, ±1 oct | Tone 0.6 tames the square's buzz |
| Stereo Warble | 280 Hz triangle | Triangle, 3 Hz, ±0.3 oct | Full Spread |
| Harmonic Ring | Tracking, Interval 0 | — | The note's own harmonics, octave-up heavy; Tone 0.9, Mix 0.8 |
| Sub-Octave Ring | Tracking, a just fifth (7 st + 2 cents) | — | Odd harmonics of half the note; Tone 0.7, Mix 0.8 |
| Octave Shimmer | Tracking, +12 st | — | Mix 0.5 keeps the note under the shimmer |

### Pitch tracking in Synth Voice, the Auto Arpeggiator and the tuner

The same `dsp/PitchTracker.h` (see *Pitch tracker* under Ring Modulator) follows the note for
three more features, each of which used to run a brute-force YIN of its own.

| Feature | How it uses the tracker |
|---|---|
| Synth Voice (`synth_saw`) | The mono input feeds the tracker, and its accepted pitch is what the oscillator glides to over Glide. After an onset (the input envelope rising by 15% in a sample) the glide runs four times faster until two confident detections, about 10 ms. While the tracker finds no pitch (silence, noise, a note dying away) the oscillator goes, over Glide, to the pitch the tracker holds: the note's own from before it began to stop. It used to keep the last estimate it was given, which may already have heard the start of the silence, and a low note's tail could settle over 30 cents out (13 cents at the default Glide); wherever the stop falls it now settles within 1.5 cents. It sounds only while the envelope is above Gate. Notes from 45 Hz to 2 kHz. At 48 kHz in 64-sample blocks it costs 2.7 µs per block on average and 12 µs at the 99th percentile, and a note change reaches the oscillator in 13–60 ms. The old detector cost 50 and 130 µs (2.2 ms at 192 kHz, against that block's 333 µs deadline), took up to 83 ms, and stopped at 50 Hz. |
| Auto Arpeggiator pitch trigger (`arp_auto`) | The left input feeds the tracker. Every 2048 samples at 48 kHz (43 ms at any rate) the frame's pitch is smoothed (0.6 of the new, 0.4 of the old; ×0.8 for a frame without one) and compared with Pitch: two frames past it turn the arp on, restarting the pattern on the beat, and five short of it turn it off. A frame's pitch comes from its latest detection that found one, and none when the frame is quieter than RMS 0.003. The old detector took the first period under its threshold rather than the bottom of the dip, in whole samples, so it read up to 190 cents sharp and a note a semitone below the threshold tripped it. It found no pitch at all at 96 kHz and above, and cost a burst of about 340 µs in one block of every 32. On the DI demo the on/off state matches the old one in 98–99.9% of blocks in Above mode. In Below mode the arp now drops out in long gaps between notes, as a full-rate YIN reference does, where the old one mostly stayed on. |
| Tuner (`TunerEngine`) | Runs the tracker on the audio thread (about 2.4 µs per 64-sample block) and averages its detections over each reading, 2048 samples at 48 kHz (43 ms at any rate). A worker thread names the note against the reference pitch and reports it, as before. A new note restarts the average. Averaged, the 5 ms detections read a held note with a third to a tenth of the scatter of the single 85 ms window the tuner used to analyse. So the tuner needs no longer window of its own, and the UI still averages six readings. With white noise 20 dB below an A2, readings scatter by 0.3 cents, where they scattered by 2.6 and sat 1.7 cents sharp. On the DI demo's held notes, 99% of readings are within 10 cents of full-rate YIN, against 75% before, and none is more than 50 cents out, against 24% before. Notes above about 1.1 kHz no longer read an octave low, and 176.4 and 192 kHz work. The worker no longer spends 2.3 ms on YIN every 43 ms. |

### Guitar to MIDI (`guitar_to_midi`)
**Experimental**: listed only with Settings → Experimental Effects on. Turns single notes into
MIDI for a virtual instrument in a Plugin Host downstream of it in the same chain. It makes no
sound of its own: Guitar Thru passes the guitar on or mutes it.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mode` | Notes / Notes + Bend | Notes + Bend | enum |
| `bendRange` | 2 / 12 / 24 / 48 semitones | 2 | enum |
| `channel` | 1..16 | 1 | — |
| `transpose` | -24..+24 | 0 | st |
| `threshold` | -60..-20 | -50 | dB |
| `dynamics` | 0.0–1.0 | 0.7 | — |
| `lowestNote` | E2 / D2 / B1 / F#1 | D2 | enum |
| `thru` (Guitar Thru) | 0/1 toggle | 1 | — |

**Notes** come from `dsp/NoteTracker.h`, which sits on the shared pitch tracker and decides
where notes start, move and stop:

- A pick is found by the power above 4 kHz jumping 6 dB over its 50 ms average, which catches
  a re-pick of a note that is still ringing. The note starts at the first pitch the tracker has
  accepted; over a ringing note, only once two detections agree, since a window straddling two
  notes reads between them, and for the same pitch only once most of the window is past the pick.
- Velocity is the pick's peak between Threshold (1) and -6 dBFS (127), blended by Dynamics with a
  fixed 100.
- A move of over a semitone without a pick (a hammer-on, pull-off or slide) is legato: the new
  note starts, then the old one stops, at the same sample. An octave has to hold for three
  detections, since a power chord or a strong second harmonic can flip the tracker by an octave.
- The note stops when it is muted (20 dB under its own level, after its first 50 ms), when the
  level stays 6 dB under Threshold, or after 100 ms with no pitch.

In **Notes** mode pitches are rounded, and a bend has to pass the next note by 0.2 semitones
before it moves there, legato. In **Notes + Bend** mode the note holds and pitch bend follows the
guitar, its tuning included; a bend that runs 0.3 semitones past Bend Range moves to the nearest
note and bends on from there. Bend Range must match the instrument's (most default to 2), or the
pitch comes out wrong. Changing channel, transpose, mode or bend range mid-note ends the note and
starts it again under the new setting.

**Lowest Note** narrows the pitch tracker's range (`PitchTracker::SetLowestFrequency`, which the
other trackers leave at 45 Hz), and with it the window: 13 ms at E2 against 22 ms at the full
range. On synthetic lines at 44.1–192 kHz and the drop-D default, a note starts 15–20 ms after
the pick at the median and within 30 ms at worst; the full range adds about 5 ms and some octave
errors. Threshold cannot usefully go below about -52 dBFS: the tracker reads nothing under
-55 dBFS RMS.

**Routing**: the notes reach every Plugin Host downstream, however many nodes lie between, and
none on a parallel branch that does not pass through this node; they stop at a composite's
edge. Several Guitar to MIDI nodes can feed one instrument, each on its own channel. The Plugin
Host's note player (`NotePlayer` in `dsp/NoteEvents.h`) reconciles what it has sent with the
note each source holds at the end of every block, so nothing hangs when a source is bypassed or
removed, a block is dropped while the plugin is busy, or the plugin is reset or replaced; a
source brought back from bypass starts afresh rather than replaying the note it had.

It is monophonic: a chord comes out as one of its notes, not always the same one. MIDI goes
only to hosted plugins: it is not sent out of the app or to a DAW.

### Pitch Shift (`pitch_shift`)
Pitch shift effect using Signalsmith Stretch, free or snapped to whole semitones, within a range an expression pedal sweeps.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -12..+12 | 0.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |
| `stepMode` (Snap to Semitone) | 0/1 toggle | 1 | — |
| `minSemitones` (Range Min) | -12..+12 | -12.0 | st |
| `maxSemitones` (Range Max) | -12..+12 | 12.0 | st |

The shift applied is `semitones` held inside `minSemitones`..`maxSemitones`, rounded to a
whole semitone while `stepMode` is on (rounding first, so the range wins). The bounds are read
in either order. The node reports that range and step to automation
(`EffectProcessor::GetAutomationRange`), so a MIDI CC, expression pedal or DAW lane mapped to
`semitones` sweeps Range Min to Range Max, gliding when free and stepping when snapped. The
params panel gives the Semitones knob the same range and step. At an applied shift of 0 st the
effect is transparent and reports no latency, so a pedal range that includes 0 changes the
reported latency as it passes through it.

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
