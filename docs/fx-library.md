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
| `kNoiseReduction` | `b29b11b4-79ee-4e90-84f0-8a8ae5c6350b` | `noise_reduction` |
| `kMultibandGate` | `51e2f89c-c174-4c1b-aa40-2e5a08dc745c` | `dynamics_multiband_gate` |
| `kMultibandCompressor` | `2bdf5236-8d4a-485c-9302-3a0479c3614d` | `dynamics_multiband_compressor` |
| `kOverdrive` | `fa9e05a8-168a-4293-aa91-6b770de3da1d` | `overdrive` |
| `kDistortion` | `686773c9-30ac-4f33-b0f8-9222146d45b1` | `distortion` |
| `kMultibandDistortion` | `86222aa0-53a4-4768-a05a-c68ef29a2bf4` | `multiband_distortion` |
| `kFuzz` | `3a38b19c-1d97-4989-b5bb-12bcc59d1e6b` | `fuzz` |
| `kEqParametric` | `4b4025ca-64cd-4180-be79-81873b618dba` | `eq_parametric` |
| `kEqGraphic` | `ef8240ba-c973-4e09-ab65-4faf56a8ecbf` | `eq_graphic` |
| `kPickupMatch` | `df1f4ed4-5d84-4a21-9474-ad6ab014fca5` | `pickup_match` |
| `kEqPitchTracking` | `b8b9922c-72d0-4a5b-a387-4ebd13789e37` | `eq_pitch_tracking` |
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
| `kReverbVerbal` | `db5be015-42c7-4bc2-a890-cad6478a6c93` | `reverb_verbal` |
| `kChorus` | `decdd132-029a-46a5-a362-edcde007a450` | `chorus` |
| `kFlanger` | `1a3f3793-7e80-4e3d-ab7b-3ce3ce032fe7` | `flanger` |
| `kPhaser` | `3aa9dc81-31c2-40d5-9b1b-b0b9d1295e9b` | `phaser` |
| `kTremolo` | `c9debb02-d7e7-43e3-8330-b387be46dcf4` | `tremolo` |
| `kRotary` | `b1d69469-4d71-47df-8404-d25bdc29a577` | `rotary` |
| `kVibe` | `fa81f9aa-5bd6-4726-baff-bb3df65a77da` | `vibe` |
| `kRingMod` | `c13068c1-9c50-4c7c-be9e-eef808990651` | `ring_mod` |
| `kDispersal` | `83377dc3-04a5-416a-afd7-f75cdf188bba` | `dispersal` |
| `kAutoWah` | `b06c6d84-01b3-4d0a-ad98-40eecb64438e` | `auto_wah` (retired: runs as `kWah`) |
| `kWah` | `8ae7a185-8075-466f-a83b-72f8dfa50af0` | `wah` |
| `kSpatial3D` | `a3196960-a89b-4388-829e-cbf8d8dd91c3` | `spatial_3d` |
| `kPitchShift` | `0c15f065-8335-4932-9d2f-366d436ec30a` | `pitch_shift` |
| `kTranspose` | `9b89cc46-e05b-4f06-981e-1d74d1f628cf` | `transpose` |
| `kTransposeStft` | `66b3a43a-72eb-4c7a-9c47-50e9ab24b718` | `transpose_stft` |
| `kOctave` | `2e4d5380-5a79-412f-bfc0-bf84ef74d561` | `octave` |
| `kHarmonizer` | `dc741ddb-1224-46a9-8256-39977d8953cb` | `harmonizer` |
| `kGain` | `0bcd895e-5d36-4247-a351-6bed1fcb37a8` | `gain` |
| `kSynthSaw` | `608e846e-0e60-4064-9c83-37c0df573c38` | `synth_saw` |
| `kAutoArp` | `e4a7c9d0-3b52-4f16-8a9e-2c7f1d0e5b83` | `arp_auto` |
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
`lowCutHz` and `highCutHz`; `reverb_advanced` `lowCut` and `highCut`; `harmonizer` `highCut`;
`vibe` `rate`; `tremolo` `crossover`; `dynamics_gate` `swell`; `reverb_verbal` `decay`, `preDelay`,
`riseTime`, `echoTime` and `shapeTime`; `eq_pitch_tracking` `width1`–`width3` and `cutoff`;
`dispersal` `stages`, `frequency` and `focus`. Stages is the one stepped parameter on a log
taper: both UIs label a whole step before a taper, so it reads "64", not "64.0".

### Conditional controls
A parameter that means nothing in some modes can say when it shows: `ParameterDef::visibleWhenKey`
names an enum parameter and `visibleWhenValues` the values it shows for (on an `EffectParamSpec`,
`visibleKey` and `visibleFor`). The catalog sends `"visibleWhen": {"key", "values"}`, and the params
panel (`core/ui/ts/signalPath/paramsPanel/paramVisibility.ts`) draws such a control hidden while the
condition fails and re-checks on every change, from the panel, a picker or the engine (MIDI, host
automation), hiding a group block left with nothing to show. It is display only: the parameter
keeps its value, and presets and automation still set it. Verbal's machine-only controls use it;
the native UI shows every control.

### Effect pickers
An enum with too many choices to dial through can ask to be drawn as a picker. It sets
`ParameterDef::control = "picker"`, and can describe each label with `ParameterDef::labelInfo`:
a `group`, a `subgroup`, an `icon` and a line of `detail`, one entry per label.

The catalog sends both, and the value is still the label's index.
- **Web UI** (`signalPath/paramsPanel/enumPicker.ts`): draws a trigger with the current choice's
  picture and name. Its flyout has a tile per group, a search box, and the choices under subgroup
  headings. Pictures come from `pickerIcons.ts`, by name.
- **Custom layouts:** still draw a knob.

Used by `pickup_match` `source`, `target` and the two design types.

### Effect presets
Every effect's Presets menu draws on three lists:

- **The effect's own factory presets**, `EffectTypeInfo::presets` in the registry: parameter values
  only, so they can name no model, IR or blend. The one marked `isDefault`, or else the first,
  seeds a new node. New lists are built with `factory_presets::Builder` (`FactoryPresetSupport.h`):
  every declared default but the player's own controls, then the preset's changes, so choosing one
  never leaves another's settings behind. `EffectFactoryPresetTests` holds each such list to that,
  checks its default is the parameter defaults, and renders every preset on the demo DI.
- **A factory archive's**: the `effectPresets` section of a bundled `.soundshed.presets`, in the
  shape of the user's store (effect type -> `[{id, name, parameters, resources?, config?}]`).
  These can choose the archive's own NAM models and IRs (`resources`) and blends
  (`config.blendId`). At startup `LoadFactoryPresetArchives` moves those references onto the
  archive-scoped ids it registers the resources and blends under, and scopes the preset ids the
  same way (`EffectPresetArchiveSupport.h`); an entry naming anything the archive does not carry
  is left out and logged. They are listed as factory presets and, like a user preset, applied by
  the engine (`applyEffectPreset`), replacing the node's resources and config where the entry
  carries them. A model or IR one of them chooses counts as in use. With factory archive loading
  off, none is offered.
- **The user's own**, saved from a node with everything it was set to.

The menu lists the first two together under Factory, in alphanumeric order (numbers by value,
case ignored), then the user's own. The order in the registry still decides which is the default.

To author factory presets that choose a model, IR or blend: turn on Factory Preset Archive Tools
(Settings, Feature Toggles) and Include My Effect Presets in Preset Exports, save each preset from
a node with Save current as, then export a preset folder. The archive then carries your effect
presets, hosted plugins' excepted, with the models, IRs and blends they choose; an entry whose
model or IR is not a library resource, or could not be read, is left out with a warning. Put the
archive in `core/ui/presets/factory/`. Archive installs and archive sessions ignore the section.

## Effect Categories

| Category | Description | Examples |
|----------|-------------|----------|
| `amp` | Amplifier simulation | NAM amp models, Neural FX |
| `cab` | Cabinet simulation | IR convolution, simple cab |
| `drive` | Gain/clipping/saturation | Overdrive, distortion, fuzz |
| `dynamics` | Dynamics processing | Noise gate, noise reduction, compressor, limiter |
| `eq` | Equalization | Parametric EQ |
| `modulation` | Modulation effects | Chorus, flanger, phaser, tremolo, ring modulator, wah (pedal or envelope) |
| `pitch` | Pitch manipulation | Pitch shift, transpose, octave |
| `delay` | Time-based delay | Digital delay, tape echo, analog (BBD) delay, doubler |
| `reverb` | Reverberation | Room, chamber, spring, advanced, IR, ambient |
| `utility` | Utility processing | Gain, splitter, mixer, signal analyzer |
| `synth` | Synthesized tones, and notes for them | Synth saw, Guitar to MIDI |

How each category looks in both UIs (the FX library's order, name and colour, its icon, the
effect view's background and artwork) is defined in `core/ui/data/effect-presentation.json`.

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
(`PluginController::ProcessAudio`), and any node parameter can be bound to an automation
slot. So `SetParam` must not allocate, lock or rebuild. Nor can a rebuild move to the message thread
under the lock: in real time the audio thread outputs silence for any block that finds the lock held
(and while rendering offline it waits for it, which stalls the bounce instead).

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

A composite forwards both calls to its inner graph. The IR Cabinet's Quality, Normalize and Trim
Leading Silence, and the Convolution Reverb's Quality, work this way.

## Built-in Effect Types

### NAM Amp (`amp_nam`)
Neural amp model processing.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `inputGain` | -24..+24 | 0.0 | dB |
| `outputGain` | -24..+24 | 0.0 | dB |

**Resource**: NAM model file (`.nam`)

#### How the model runs

Every NAM node builds its models through `nammodelcache::GetModel` (`core/src/dsp/NamModelCache.h`),
one per channel, from a parse cached per file. A WaveNet, or an A2 slimmable container, runs on the
fused WaveNet pass (`core/src/dsp/nam/FusedWaveNet.h`): every model shape measured does, and it costs
2.0-4.8x less CPU than the NAM core (through the NAM amp effect at 48 kHz with 64-sample blocks, 2.8x
on a standard and on an A2 model), with output within float rounding of the NAM core's and
bit-identical whatever the host block size. Its weights are packed once per file and shared by every
node and channel, so building a cached model and preparing it takes about 0.4 ms, where the NAM core
took 14 ms for a standard model and 24 ms for an A2 one (`NAMPerformanceBenchmark`). Other architectures
(LSTM, ConvNet) and WaveNet options it does not compute run on the NAM core, as before. arm64 builds
run the fused pass on its NEON kernel, which measured 1.1-2.4x the NAM core on Apple silicon,
1.5-2.4x on arm64 Linux and 2.3-6.6x on Windows on ARM (`GUITARFX_FUSED_WAVENET_NEON`,
docs/neural-amp-modeler-core-integration.md).

Turning off **Fast NAM Engine** in Settings → DSP Performance (`audio.nam.fusedEngine`, on by
default) puts every model on the NAM core and rebuilds the running presets. Launching with
`GUITARFX_NAM_ENGINE=core` in the environment does the same and outranks the setting, for A/B
listening and support. The session log names the engine on the first model load. The details are in
[neural-amp-modeler-core-integration.md](neural-amp-modeler-core-integration.md).

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
  `ReloadSharedSyncSourcesFromDisk()` skips them when it merges the store's changes and
  re-asserts the instance's own values, so the cross-instance settings sync cannot
  overwrite them.
- **Standalone**: unchanged — `app.json` owns them.

**Offline rendering.** A DAW flips `AudioProcessor::setNonRealtime()` around a
bounce, freeze, or export; `PluginProcessorAdapter` forwards that to
`PluginController::SetOfflineRendering()`, on whatever thread the host used: JUCE's
VST3 wrapper calls it from `process()` every block, LV2's from `run()`, and CLAP's
render extension from the main thread while audio runs. It only records the mode.
A bounce renders at the instance's own tier, so it sounds like playback, and the
switch changes neither the latency nor anything under the DSP lock. While offline,
`ProcessAudio` waits for the DSP lock instead of giving the block up: a bounce asks
for the next block at once, so a block given up would be silence written into the
file for as long as the message thread held the lock.

1.6.0 raised NAM to full size and at least 2x oversampling for renders. That
re-prepared every NAM node under the DSP lock, 16-140 ms per instance with one
NAM node, and every block asked for meanwhile came back silent, which in FL Studio
left up to a second of silence at the start of renders. To render at a higher
tier, raise it for the instance before bouncing.

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
resampled to an integer multiple of the model's native rate, the model's
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
left and right run on two threads (`rtparallel::DualLaneExecutor`) once a lane costs
enough to be worth it (signal-chain.md, Threads and Silence).

**CPU.** One model when a knob sits on a captured setting or in snap mode, two while
crossfading, briefly up to four during fast sweeps. Oversampling and slimmable size
follow the per-instance NAM quality above.

**Resource**: NAM model files (`.nam`), one per capture.

### Heavy American (`amp_builtin`)
A built-in high-gain amp head, for use with a cab or IR after it (`BuiltinAmpEffect.h`, voiced in
`BuiltinAmpVoicing.h`). Up to four clip stages, two before the tone stack and two after, then a
power stage.

**Level.** Output is the only level control. With it at 0 dB, a guitar at the nominal operating
level (Settings, -18 dBFS by default) comes out at that level, as heard through a cab, whatever
Voice, Gain, Preamp Stages, Character, Power Drive, Bias and Sag are set to: about -18 dBFS RMS.
Across those controls it holds to 0.3 dB RMS and about 1 dB at worst on the demo guitar. The
makeup is static, measured on real guitar at the nominal level (`kHeardGainDb`, re-measured with
`BuiltinAmpEffectTests --measure-levels`), so the amp still plays like one: softer playing comes
out quieter on the clean end and barely changes on the high-gain end. The tone controls, Bright,
Pre Emphasis and Input Trim are not compensated. Input Trim stands for a hotter or cooler pickup,
but Power Drive and Sag still hold the level at any trim: their makeup feeds the power stage as hot
as the trim makes it (`kTrimGainDb`), to 0.33 dB RMS over trims of -18 to +12 dB. The amp reads
the nominal level the way the drive pedals do: a guitar there drives it exactly as hard wherever
that level is set, and comes out at it.

| Parameter | Range | Default | Notes |
|---|---|---|---|
| `voice` | Clean / Drive | Clean | Both channels come out at the same level |
| `gain` | 0–1 | 0.45 | Level-compensated, so it is not a volume control |
| `character` | 0–1 | 0.5 | Vintage (soft knee, even harmonics, loose) to modern (hard knee, tight); level-compensated |
| `bright` | off / on | off | +3 dB shelf at 2.5 kHz before the first stage |
| `preEmphasis` | 0–1 | 0 | Up to +6 dB more on that shelf (advanced) |
| `stageCount` | 1–4 | 2 | Clip stages; level-compensated, and fades a stage in or out over 20 ms |
| `stageGain` | ±24 dB | 0 | Input Trim, before the first stage; not compensated |
| `bass` / `middle` / `treble` | 0–1 | 0.5 | ±9 dB at 120 Hz, 750 Hz, 3.5 kHz |
| `contour` | 0–1 | 0.2 | Up to -12 dB at 600 Hz |
| `presence` | 0–1 | 0.5 | ±6 dB at 4 kHz |
| `output` | ±24 dB | 0 | |
| `powerDrive`, `sag`, `bias`, `depth`, `resonance`, `damping` | | | The power section (advanced). Power Drive, Bias and Sag are level-compensated, like Gain. Sag pulls the power stage's ceiling down as you dig in, so loud notes clip and compress harder than soft ones around a level that holds; with Power Drive at 0 it does nothing |

**Factory presets** (`BuiltinAmpPresets.h`) set every control, Output and the hidden power
section included. Output is the exception to the rule the other effects keep: an amp preset is a
whole channel, and its Output takes out what its tone controls and Bright add. Every one is within
1 dB of 0 and lands within 0.3 dB of the default, as heard, on the demo DI and riffs, as recorded
or at the nominal level. Clean Channel (default), Edge of Breakup, Classic Crunch (the Clean voice nearly
dimed), Tight Modern Rhythm, Tight Djent (hard knee, four stages), Scooped Thrash, Vintage High
Gain (soft knee, sag and bias) and Singing Lead (four stages, mids forward, compressed by sag).

**Older presets keep their loudness.** The amp was rebuilt after 1.5.0. The 1.5.0 amp had no level
makeup and came out 12–18 dB above a guitar at the nominal level at any Gain past a quarter, so its
presets set Output against that, often -15 to -24 dB. A stored amp node with no `character` key can
only come from before the rebuild, so reading one migrates it (`MigrateLegacyNodeParams`,
`BuiltinAmpLegacyMigration.h`). It gets Character's default explicitly and the Output at which the
new amp is as loud as the old one was, every other control unchanged. That Output comes from the
old amp's heard gain, tabulated by voice, Preamp Stages, Gain and Input Trim, against the new
amp's own Input Trim table. On top of that come a model of the old power stage for its Power
Drive and Sag, and how much more of a tone-stack cut or boost each amp let through.

Against the old amp, which `BuiltinAmpLegacyMigrationTests` keeps verbatim
(`helpers/LegacyBuiltinAmp.h`), the factory pack's amps keep their loudness to within 0.4 dB.
Random old settings hold to about 0.7 dB RMS, 1 dB at worst. An old node that would need more than
+24 dB of Output, one already turned up on the old amp, stops there. `--calibrate` re-derives the
tables if the new amp's voicing or makeup changes. A migrated node is saved with its Character the
next time its preset is, and is not migrated again.

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
| `trimLeadingSilence` | 0/1 | 0 | toggle |

Quality levels: 0=Economy, 1=Standard, 2=High, 3=Full

**Two IR slots.** IR A and IR B each have their own Level, Pan, Invert and mic settings. With
both loaded, IR Blend (`irBlend`) crossfades from A at 0 to B at 1, and L/R Split (`lrSplit`)
plays A from the left input and B from the right instead. One IR alone plays in full, in either
slot, as if IR Blend were all the way to it, whatever IR Blend, L/R Split and the IR B offset
say: its own slot's settings apply and the other slot's do not.
Loading or clearing a slot crossfades over 30 ms from what was playing, the outgoing side at the
level it played at. Until 2026-10-17 a cab with only IR B passed its input dry, because
`Process()` waited for slot A's convolvers. The outgoing side of that crossfade also played at the incoming slots' levels then, so adding IR B
to IR A, or clearing it, with IR Blend above 0 jumped the level instead of fading it.

**Normalize** (`normalizeIR`, on by default) is built into the convolvers: the normalisation gain
into the coefficients. A preset sets it before its IRs load, so loading one rebuilds nothing extra.
A change once IRs are loaded, from the node panel or from MIDI or DAW automation, is not built in `SetParam`
(see *Where SetParam runs*). The message thread builds it off the DSP lock from copies of the
impulses, and swaps it in under the lock with the same 30 ms crossfade an IR change uses. The
outgoing side of that crossfade is the convolvers that were playing, history and all. A change from
the node panel is built before its message returns, and one from automation at the next idle tick.
`GetParam` reports the requested value straight away. Normalize is not a declared parameter, so the node panel has no control for it. Presets set it,
and so can a slot address that names it (`node.cab_ir.normalizeIR`).

**Quality** is a rebuild setting the same way: where the IR is cut, and so its normalisation gain,
is built into the convolvers, and a change on a loaded cab is built by the message thread and
crossfaded in. Until 2026-10-17 it only recorded the change, and the cab kept playing the old
length until the next IR load, Prepare, or Normalize or Trim Leading Silence change.

**No latency.** The cab has no latency at all, whatever block the host promises or sends.
The first 256 samples of the IR (512 for an IR of 16,448 samples or more) run as a direct
FIR, and the rest as FFT partitions that grow with their offset into the IR, each one exactly as
late as its part of the IR has to be (`RealtimeConvolver`). An IR no longer than the FIR runs on
the FIR alone. At a small host block this costs less CPU than the 64- and 128-sample partitions
it replaced; at a large one, about the same (docs/plans/zero-latency-ir-cab.md). So the dry
signal lines up with the wet one below 100% Mix with no delay of its own. Speaker Drive bends a
copy of the input, so the dry stays clean.

Until 2026-10-16 the cab's Low Latency switch, on by default, meant 64 or 128 samples of latency
when the host promised blocks that small and 256 otherwise (5.3 ms at 48 kHz in most DAWs). The
wet signal was that late, so a Mix below 100% comb-filtered against the undelayed dry one. Low
Latency off ran one uniform partition, 256 to 2048 samples from the largest block the host
promised, and from later that day delayed the dry to match (`DryPathDelay`). Measured against the
zero-latency convolver, off saved 7-9 ns per sample per convolver on a cab-length IR (about 0.04%
of a core), cost 1.2-1.9x more on a 0.5-1 s IR and up to 3.7x on a 3 s reverb at blocks up to 256,
and sounded the same, shifted. So on
2026-10-17 the switch was retired from both IR effects. A preset or MIDI binding that still sets
`lowLatency` is ignored, and one saved with it off loses its 5-43 ms of latency, which a host
compensates for as it does any latency change. Until 2026-10-13 a short IR took the Low Latency
off size even with it on: a host promising 8192 samples and sending 64 got a cab adding 2048
samples (43 ms at 48 kHz).

**Trim Leading Silence** (`trimLeadingSilence`, off by default, under Advanced) takes off what an
IR file has in front of its sound: the time the sound took to reach the mic, and any silence the
capture left before it. With no convolver latency, that is the only delay a cab still adds, and
Quality's length counted it against the IR. An IR's onset is its first sample within -40 dB of its
peak, in either channel of a stereo IR (`IrLeadingSilence.h`), and the trim stops 4 samples short
of it so the attack's rise is kept. With two IRs loaded, both lose the earlier one's onset, so IR B
keeps its place against IR A and an IR B offset means what it did. The trim is counted at the
host's rate and taken after resampling, so two IRs at different rates move by exactly the same
time. Quality's length is counted from where the trim stops. It is a rebuild setting like Normalize,
built off the DSP lock by the message thread when changed on a loaded cab. The cab reports no
latency either way: an IR's own pre-delay was never counted as latency. It is off by default so a
preset keeps the timing it was made with.

Of 66 distinct cab-length IRs measured on 2026-10-16 (resources/ and a development profile), 51
reach -40 dB within 0.1 ms, 4 within 0.5 ms, 10 take 0.5-1.6 ms and one 3.5 ms. What the trim
removes from any of them is at most -44 dB of the IR's peak and -49 dB of its energy, at 44.1, 48
and 96 kHz, resampled IRs included.

Speaker Drive is the Simple Cabinet's level-dependent speaker stage (`SpeakerDrive.h`) in front
of the convolution: what an IR, being a linear snapshot, cannot capture. It bends only what
reaches the IR; the dry mix stays clean. At 0 it is exactly transparent.

**Alignment** (`slotBOffset`, -10..+10 ms, default 0, group "Alignment") is IR B's time against
IR A, positive when B plays later. Two blended IRs sum like two mics on one speaker: lined up they
reinforce, a fraction of a millisecond apart they comb-filter, and either can be what is wanted.
`IrSlotAlignment.h` applies it as a delay on the input of whichever slot plays later (B for a
positive offset, A for a negative one), so the earlier slot is untouched and the reported latency
does not change. Being a delay rather than a change to the impulse, it moves without a rebuild,
so it is safe from automation on the audio thread; a new value glides in over about 20 ms
(a one-pole), bending pitch briefly instead of clicking. Whole-sample offsets are exact;
fractional ones use 4-point Lagrange interpolation, linear below one sample. With only one IR
loaded the offset does nothing, and at 0 the slots read their input untouched, so a node that
never sets it sounds as it did before it existed. Polarity is the existing `slotAPolarity` and
`slotBPolarity`.

The web UI shows an **Alignment** section under the Main and Advanced tabs, so it is in view
whichever tab is open, once both IRs are loaded. It sets the offset by dragging B's waveform over
A's, by clicking along a strip of how alike the two are at every offset, by typing or by nudging,
and draws the two summed against the two added without interfering, so a cancellation shows as a
dip. With one IR loaded, in either slot, the section shows only that IR's response.

**Auto Align** asks the engine (`analyzeIrAlignment`, `dsp/IrAlignment.h`) where B lines up: it band-limits both IRs to 100 Hz-6 kHz, cross-correlates
them over ±10 ms, takes the largest correlation within 2 ms of the offset that lines up their
onsets (so a strong resonance or a loud room reflection cannot pull it a period off), and places
the peak between samples with a parabola. A negative peak means B lines up inverted, and Auto
Align then sets B's polarity against A's. It runs only when asked, never on an IR change or a
preset load, so an offset set on purpose stays where it was put.

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
- A load restores the chunk once, before the plugin is prepared, as hosts do. A prepare
  restores only a chunk that instance has not been given, so preparing again (a device
  change, say) leaves the plugin as it is rather than putting back the last capture.
- Pointing a node at a different plugin drops the previous plugin's chunk and identity keys;
  a chunk is only ever carried across a rebuild when the plugin identity still matches.
- A rebuild keeps the instance. Every chain edit (adding, removing or moving a node, a split,
  a resource, a scene) builds the whole chain again, and each Plugin Host in it that still names
  the same plugin takes over the instance its node had, editor window, listeners and sounding
  notes included, instead of creating one (`EffectProcessor::ExpectHandOver`). It used to
  create one, so an edit cost the instantiation of every plugin in the chain, seconds and
  hundreds of MB each for heavy amp sims, with the old instances alive alongside the new until
  they faded out. Now an edit costs only the plugin it adds. The node is configured with the
  state just captured from that instance, so nothing is restored; a different chunk (a scene
  of its own, an effect preset, an undo) is restored onto it. Only the running slot with the
  same id hands over, so another preset never inherits a plugin. The instance moves once the
  new chain is installed (`MultiPresetMixer::CompleteInstanceHandOvers`, off the DSP lock);
  until then the new node passes its input through, and after it the old one does for the rest
  of its ~21 ms fade-out. If the old node has gone by then, the new one loads the plugin itself.
  Plugin Hosts inside a blend or custom effect are still created again.
- A VST3 is scanned once. Most VST3s ship no `moduleinfo.json`, so JUCE can describe one only
  by loading its binary and creating and initialising a component of each class in it, which it
  then throws away and unloads before making the real instance. That scan was 80-95% of a first
  load for heavy amp sims (Guitar Rig 7 7.8 s of 8.4 s, BIAS FX 2 7.2 s of 7.6 s, measured
  2026-10-23). What it found is kept in `<settings>/cache/hosted-plugin-descriptions.xml`
  (`HostedPluginDescriptionCache`), keyed by path and fingerprinted by the plugin's binaries'
  names, sizes and modification times, so an update in place is scanned again. A load with a
  cached description goes straight to creating the instance: twelve heavy plugins went from
  31.5 s to 5.8 s. A cached description the plugin can no longer be created from is dropped and
  the load rescans. The `LoadPluginFromPath timings ms` line in the session log gives each
  load's scan, instantiate, state and prepare times.
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
| `mode` | Gate / Swell | Gate | — |
| `swell` | 50–4000, log | 800 | ms |

`thresholdDb`, `attackMs`, `holdMs` and `releaseMs` are accepted as legacy spellings and are
declared as `ParameterDef::aliases`, so `CanonicalizeNodeParams` folds them onto the ids above
when a stored node is loaded. Presets shipped before this change carry the suffixed names.
`GlobalChainEditor::NormalizeConfig` folds the global chain's own config too, not just the
executor's copy of the graph: the config is what the UI is sent and what the settings blob is
written from, so a panel reading `attack` out of a node holding only `attackMs` would show a
default instead of the setting and then write that default back over it.

**Learn sets `threshold` from the noise.** With the strings muted, Learn listens to the gate's
own detector for 2 s (after 100 ms to settle) and suggests a threshold the noise cannot reach:
`noise floor + hysteresis + 3 dB`, clamped to the parameter's range, so that even the close level
sits above the noise and hiss can neither open the gate nor hold it open after a note. The noise
floor is the 95th percentile of the detector's 10 ms slice peaks, so a cable knock, which the
detector's peak hold stretches over four or five slices, does not drag the threshold up: anything
under about 90 ms is left out. A floor more than 15 dB over the median slice, or above -12 dBFS,
means something longer was played, and the listen is reported as that rather than applied. It measures the detector rather than the input, so the suggestion is right
wherever the gate sits in the chain, after an amp included.

The measurement is `GateNoiseLearner` (`NoiseGateLearn.h`), fed per sample from `Process` and
`ProcessMono` only while a listen runs; it allocates and locks nothing, and leaves the audio
bit-identical. `GateLearnService` answers `gateLearn`: it starts a listen on the global gate or a
preset's node, holds the mixer awake (`MultiPresetMixer::SetHoldAwake`, since the quiet it listens
to would otherwise put the chains to sleep), polls on idle, restarts the listen on a gate rebuilt
meanwhile, and reports `gateLearnStatus`. The engine never sets `threshold` itself: the UI applies
the suggestion as an ordinary edit, so it lands in the preset, undo and the host. The button is
at the top of the global gate's flyout and in a gate node's panel, under a meter
(`ts/gateLearn.ts`) of the noise heard against Threshold and its hysteresis band. Learn switches
a bypassed gate on first, since a bypassed gate hears nothing.

**The global gate is edited from the control bar.** The GATE toggle and threshold knob are the
whole of it on the bar; the chevron beside the dB readout opens a flyout with every parameter
above, `advanced` ones under their own heading. `ts/gateSettings.ts` builds that panel from the
effect catalogue rather than a list of its own, so a parameter added here appears there with its
range, unit and grouping; each change goes out as a `gate.<id>` `setGlobalChainParam`. The rows are
`ts/flyoutParamControls.ts`'s, shared with the global Pickup Match tab.

**Attack and release shape the applied gain, not the detector.** The detector runs fast and at
a fixed rate so the gate sees a pick attack the moment it arrives; the user's times control how
the gain ramps between `range` and unity. This is what keeps the close from clicking — the gate
sits in front of the amp, so a gain step at the threshold level would be amplified with the rest
of the signal.

The detector holds each peak for 20 ms, which bridges the 12 ms between peaks of a low open E
(16 ms for a seven-string's low B),
and then falls with a 3 ms time constant. It is down at the close threshold about 40 ms after the
playing stops, and `hold` starts from there. Until 2026-10-13 it fell over a 25 ms one-pole
instead, which took about 170 ms after a riff stopped dead. All that time the gate passed the
input noise floor at full level, and in front of a high-gain amp that was a quarter-second burst
of hiss after every stop. 1.5.0's gate, whose detector followed `release`, had closed in about
60 ms at the same settings.

The detector listens through a band-pass: a one-pole high-pass at 120 Hz, so hum and rumble
cannot hold the gate open, and a one-pole low-pass at 5 kHz, so hiss cannot either. At 48 kHz
the detector reads white noise about 5 dB lower through the low-pass, while tones from 82 Hz to
1 kHz move by under 0.2 dB (2 dB at 4 kHz). At only 6 dB/oct it does not slow the detector's
response to a pick attack.

`hysteresis` is how far below `threshold` the level has to fall before the gate may close, which
stops a decaying note chattering it open and shut. `range` is the attenuation when closed rather
than a full mute. `stereoLink` keys one detector off the louder channel so a stereo signal cannot
half-close; turning it off gates each channel on its own level.

**Swell** turns the gate into a volume swell, the rolled-up volume knob of a violin-like
attack. When the gate opens, the gain climbs to unity over `swell` along a squared curve (so it
sounds even rather than rushing up at the start) instead of over `attack`, which Swell ignores.
A note picked while the last still rings would otherwise come in at full level, so a pick
detector (`PickAttackDetector`, the one the harmonizer and transposer use) dips the gain in a
few milliseconds and the swell starts again; a pick only retriggers once the last swell is past
30%. It has no look-ahead, so the first millisecond or two of a pick over a ringing note can
still be heard. Between detached notes the gate closes at `release` as usual.

**Factory presets** (`DynamicsPresets.h`) set how the gate opens and closes, and leave out
`threshold` and `stereoLink`: the threshold belongs to the player's pickups and how much noise
their rig makes, and choosing how a gate closes should not move where it closes.

| Preset | Attack / Hold / Release | Hysteresis | Range |
|---|---|---|---|
| Standard Gate (default) | 1 / 50 / 50 ms | 4 dB | -80 dB |
| Soft Reduction | 2 / 100 / 200 ms | 6 dB | -18 dB, an expander more than a gate |
| High-Gain Tight | 0.5 / 20 / 25 ms | 6 dB | -80 dB |
| Staccato Chug | 0.2 / 5 / 10 ms | 8 dB | -90 dB |
| Natural Decay | 2 / 150 / 300 ms | 10 dB | -60 dB |
| Ambient Friendly | 5 / 250 / 500 ms | 12 dB | -30 dB, so tails into a delay or reverb fade, not stop |
| Volume Swell | Swell, 600 ms; hold 80, release 250 ms | 10 dB | -80 dB |
| Slow Swell | Swell, 1100 ms; hold 150, release 500 ms | 12 dB | -80 dB |

### Multi-band Gate (`dynamics_multiband_gate`)
A noise gate in six frequency bands, each turned down only once its own level falls below its
own threshold. A one-band gate shuts everything at one moment, hiss and body together; here the
hiss bands close as a decaying note's top end fades into them while its lower bands, still well
above their noise, play on. Each band is a soft downward expander rather than a switch, so there
is no hysteresis or hold to set and nothing chatters. Engine: `MultibandGateEffect.h`, with the
crossover, gain curve and parameter table in `MultibandGateSupport.h`. No latency.

- **Bands.** Fixed crossovers at 150, 400, 1000, 2500 and 6000 Hz: lows (hum, rumble), low
  mids (body), mids, upper mids, presence and air (most of the hiss). Fourth-order
  Linkwitz-Riley in a cascade, each lower band run through the second-order all-passes of the
  crossovers above it, so open the six add up to a flat all-pass of the input. A split by
  subtraction (Noise Reduction's) adds back exactly but leaks about 6 dB/octave: a 1 kHz note
  reads only 8 dB down in the air band, which would hold it open on every note.
- **Detectors.** Each band's mean square, smoothed over 20 ms (lows), 8 ms (low mids), 4 ms
  (mids) and 3 ms above, so a low note's ripple stays near a dB. One detector per band drives
  both channels (the louder side's), except in dual mono.
- **Gain.** Below a band's threshold (plus `offset`) its gain falls 4 dB per dB (1:5
  expansion) through a 6 dB soft knee, down to `range`. Worked out every 16 samples; the applied
  gain ramps per sample, up at `attack`, down at `release`.

| Param | Range | Default | Notes |
|---|---|---|---|
| `threshold1`..`threshold6` | -96..0 dB | -65 dB | Lows, Low Mids, Mids, Upper Mids, Presence, Air (group Thresholds) |
| `offset` | -24..+24 dB | 0 | Moves all six together |
| `attack` | 0.1..50 ms | 1 ms | How fast a band opens |
| `release` | 5..1000 ms | 120 ms | How fast it closes (log taper) |
| `range` | -80..0 dB | -40 dB | The deepest cut |

**Learn** is the noise gate's (`GateLearnService`), through `NoiseLearnHost`: one
`GateNoiseLearner` per band, started together and fed in lockstep. Each band's threshold is
its noise floor (95th percentile of the 10 ms slice peaks) plus 8 dB, which with the curve above
takes about 20 dB off the noise floor itself. A listen fails as playing if any band heard
playing. `offset` is left alone, as the player's trim. `gateLearnStatus` carries `bands`
(docs/user-interface.md), and the node panel draws one meter per band.

**Factory presets** leave the thresholds and `offset` alone (they belong to the player's rig):
Standard (default; attack / release 1 / 120 ms, -40 dB), Subtle (1 / 150 ms, -12 dB), Gentle
(2 / 250 ms, -20 dB), Tight (0.5 / 50 ms, -60 dB), Staccato (0.2 / 25 ms, -70 dB) and Slow
Fade (2 / 500 ms, -30 dB).

### Multi-band Compressor (`dynamics_multiband_compressor`)
A compressor in four frequency bands, each compressed on its own level with its own threshold,
ratio, knee and makeup, and each with On and Solo. It holds down a boomy low string without squashing the pick attack above it,
or tames fizz without thinning the body. Engine: `MultibandCompressorEffect.h`, with the
parameter table, crossover ordering and gain curve in `MultibandCompressorSupport.h`. Plan:
docs/plans/multiband-compressor.md. No latency.

- **Bands.** Low, Low Mid, High Mid and High, split at three crossovers the player moves
  (defaults 200, 800 and 3000 Hz). The split is the Multi-band Gate's, shared in
  `dsp/LinkwitzRileyCrossover.h`: fourth-order Linkwitz-Riley with all-pass compensation, so
  below every threshold the bands add up to a flat all-pass. Each crossover is kept a third of
  an octave above the one below, and the top one under 0.45 × the sample rate.
- **Detectors.** Each band's mean square over 8, 4, 2 and 1 ms (low band first). Linked across
  channels (the louder side), separate in dual mono.
- **Gain.** A knee `kneeN` wide centred on the threshold (a parabola, so no corner; 0 is a hard
  knee), then (1 − 1/ratio) of the overshoot. The reduction is smoothed in
  dB by `attack` and `release`, stretched 2× in the low band and 1.5× in the low mid (a fast
  release on a low note pumps and distorts it). Worked out every 16 samples; the applied gain
  ramps per sample.
- **Mix** blends each band with itself uncompressed, so dry and wet share the crossover's
  phase and a half-way Mix does not comb. Makeup is on the compressed part; `output` trims the
  sum. Makeup, Mix and Output glide over 20 ms.
- **On and Solo.** A band turned off passes untouched (no compression, no makeup) and meters no
  reduction; its detector lets go at its release, so turning it back on starts from rest. While
  any band is soloed only the soloed bands are heard, each as it would be (compressed if on,
  untouched if off), and Output still applies. Both glide over 20 ms, so switching does not click.
  Solo is saved with the preset like any other param.

| Param | Range | Default | Notes |
|---|---|---|---|
| `crossover1`..`crossover3` | 40..1000, 150..5000, 600..12000 Hz | 200, 800, 3000 Hz | Group Crossovers, log taper |
| `threshold1`..`threshold4` | -60..0 dB | -24 dB | Groups Low, Low Mid, High Mid, High |
| `ratio1`..`ratio4` | 1..20 | 3 | Log taper; 1 leaves the band alone |
| `knee1`..`knee4` | 0..24 dB | 6 dB | Width centred on the threshold; 0 is a hard knee |
| `gain1`..`gain4` | -12..+12 dB | 0 | Makeup |
| `on1`..`on4` | toggle | on | Off passes the band untouched |
| `solo1`..`solo4` | toggle | off | Any soloed: only the soloed bands are heard |
| `attack` | 0.1..100 ms | 10 ms | Log taper |
| `release` | 10..1000 ms | 120 ms | Log taper |
| `mix` | 0..1 | 1 | Per band, in phase |
| `output` | -18..+18 dB | 0 | |

**The node panel** draws the bands over the spectrum: the input shaded, the output as a line,
each band's gain reduction hanging from the top with its number, and under the plot each band's
level against its threshold, with the knee shaded around it. A band that is off is labelled so and
never shows reduction; bands silenced by another's solo are shaded over, and a soloed band's name
turns gold. The crossovers can be dragged. The engine side is the EQ's spectrum
tap plus `EffectProcessor::BandMeterCount`/`ReadBandMeters`: a watched node with band meters is
also tapped after it runs, and `sldS` carries its output bins and bands (docs/user-interface.md).

**Factory presets:** Standard (default), Tame the Boom (low band only, -30 dB 4:1), Tight Low
End (lows 6:1 from 150 Hz, fast), Even Out (all bands 2.5:1 at -30 dB, +3 dB), Smooth Top (above
3.5 kHz, 4:1, fast), Glassy Clean (body held, top open), Parallel Squash (8:1 at -40 dB, +8 dB,
Mix 0.5) and Gentle Glue (1.8:1 at -18 dB, slow).

### Noise Reduction (`noise_reduction`)
Noise reduction for guitar and bass pickups: mains hum and single-coil buzz taken out by
cancellation, hiss by a band-split downward expander. No latency, and not a gate: the hum is
cancelled under the playing as well as between notes, and a decaying note is never chopped. It
belongs first in the chain, before anything that adds gain. `NoiseReductionEffect.h` runs it,
`NoiseReductionSupport.h` holds the parameter table, the voicings, the canceller bank and the
floor tracker, and `NoiseReductionHiss.h` the hiss stage.

```
in ─┬─ hum canceller (LMS per harmonic) ─┬─ low band ──────────────────┬─ out
    │    ▲ learns only while quiet        ├─ mid band  × expander gain ─┤
    │    ▲ frequency-locked to the mains  └─ high band × expander gain ─┘
    └─ quiet detection (input and residual, each against its own floor)
```

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `pickup` | Single Coil / Humbucker / Active / Bass | Single Coil | — |
| `hum` | 0–1 | 1 | amount |
| `mains` | Auto / 50 Hz / 60 Hz | Auto | — |
| `hiss` | 0–1 | 0.5 | amount (1 = up to 30 dB of cut) |
| `threshold` | 0–24 | 10 | dB above the measured noise |
| `release` (advanced) | 20–500 | 100 | ms |
| `listen` (advanced) | Output / Removed Noise | Output | — |

**Hum.** A pickup is a coil in the mains' magnetic field, so its noise is the mains frequency and
its harmonics, and a single coil's buzz reaches several kilohertz. `HarmonicBank` is a bank of
adaptive sinusoid cancellers (Widrow's adaptive noise canceller with the reference generated, not
measured): a cosine and sine pair per harmonic, weighted by LMS to match that harmonic's amplitude
and phase. The harmonics are independent and each learns with a 300 ms time constant, which makes
each notch about 1 Hz wide while learning. With the weights held, the bank is a fixed sum of
sinusoids subtracted from the input, so it takes away the hum it learned and nothing else.

- **Learning only while quiet.** The weights adapt at full rate while the input is quiet and fade
  out steeply as it rises, so a note being played, even one sitting exactly on a hum harmonic, is
  never learned away (`NoiseReductionEffectTests` holds a 4 s note at 300 Hz, the 5th harmonic of
  60 Hz, to 0.1 dB). Quiet is judged twice, each against a noise floor of its own, and either is
  enough: what is left after cancelling, within 6 dB of its floor, and the input as it came, within
  3 dB of its floor. The residual alone can deadlock: let the mains drift while learning is off and
  the residual climbs above the floor it set while it was cancelling well, reads as playing, and
  learning never resumes. The input's level does not depend on the canceller, so it breaks that.
- **Following the mains.** Mains wanders by hundredths of a hertz, which at the 96th harmonic is a
  few hertz, far outside a 1 Hz notch. Every 100 ms of quiet, a frequency-locked loop reads how far
  each harmonic's learned phase has turned, fits one frequency error to all of them (weighted, and
  only harmonics clearly above the weights' noise), and moves the fundamental half of the way.
  It stays within 0.5 Hz of 50 or 60 Hz. The test locks 50.2 Hz mains to within 0.01 Hz.
- **Auto mains.** Two small probe banks listen for 50 Hz and 60 Hz on harmonics the two do not
  share (1, 2, 3, 4, 5 and 7), learning faster than the canceller so drift cannot hide either.
  Every half second Auto weighs their learned power by how quiet each block was, and switches the
  canceller to the other mains if it is 6 dB stronger and above -100 dBFS.
- **Pickup** sets how many harmonics are followed: up to 4.8 kHz for Single Coil (buzz), 2.4 kHz
  for Bass, 1.2 kHz for Humbucker and 600 Hz for Active, whose pickups cancel most of it. The count
  is fixed from the nominal mains, so drift never moves a harmonic in or out.
- **Hum** scales the subtracted estimate (a 20 ms glide). At zero the canceller stops; turned up
  again, it starts from nothing, since its oscillators stood still meanwhile.

**Hiss.** Two fourth-order Butterworth low-passes split the signal by subtraction: low is the
first, mid the second less the first, high the input less the second, so the three add back to the
input exactly and at unity gains nothing changes. The splits are 2 and 5 kHz (Single Coil,
Humbucker), 2.5 and 6 kHz (Active) and 1.2 and 3.5 kHz (Bass). Below the lower split nothing is
touched. Each upper band's power (5 ms smoothing, the same both ways, so the detector does not
ride the hiss's own peaks; a pick still opens a band within a millisecond) is compared with that
band's noise level: 3 dB over its floor, since a minimum sits below the mean. At the noise level
and below the band gets the full cut Hiss asks for, at Threshold above it unity, and between them
a straight line in decibels. Gains open over 2 ms and close over Release. The test measures 15 dB
of cut on steady hiss at Hiss 50% and 30 dB at 100%, with the band below 800 Hz within 0.3 dB.

**Noise floors are measured, not set** (`FloorTracker`): the lowest smoothed level over the last
5 s, in ten half-second subwindows, fed one minimum per block. Any pause, palm mute or decay shows
the floor, so Threshold means the same whatever the interface's gain. Only five seconds of
unbroken playing can lift a floor, and ceilings cap how far (-40 dBFS broadband, -50 dBFS per hiss
band). For the first 250 ms after a reset the smoothed levels are still rising from zero, so the
floors take no readings and the hiss stage does not expand.

**Listen** set to Removed Noise plays the input less the output: only what was taken away, to
check that it is only noise.

Each channel has its own canceller, detectors and floors (`CanWiden` false,
`KeepsChannelsSeparate` true), with a mono path; a mono path turning stereo hands the right
channel what the left has learned. A non-finite input resets the channel within a block. Release
cost, measured with `/O2 /fp:fast /arch:AVX2`: about 6.5 µs per 64-sample block for Single Coil
(80 harmonics at 60 Hz) and 3–4 µs for the others, against a 1333 µs deadline at 48 kHz.

**On the DI take** (`NoiseReductionEffectTests`, 20 s of the demo riffs with 60 Hz buzz to 4 kHz
and hiss 74 dB down added): the canceller takes the added hum down 12 dB under the playing, where
learning mostly waits, and both stages together take the added noise down 26 dB between the riffs,
while every loud 50 ms window keeps its level to 0.3 dB. On the clean take alone, Auto stays on
50 Hz and the loop moves off nominal, so the recording seems to carry some 50 Hz hum of its own.

**Factory presets** leave out `mains` and `listen`: where the player plugs in, and what they are
checking. Single Coil (the default), Humbucker, Active Pickups (Hiss 60%), Bass (Hiss 40%,
Release 160 ms), Hum Only (Hiss 0), High Gain (Hiss 85%, Threshold 14 dB, Release 70 ms) and Gentle
(Hum 70%, Hiss 25%, Threshold 6 dB, Release 200 ms).

### Parametric EQ (`eq_parametric`)
4-band parametric equalizer with two bell mids and independently selectable Shelf / Bell shapes
for the low and high bands. Shapes default to Shelf, including in older presets that omit the
shape parameters. The curve, Q controls, saved presets, Global EQ and backing-track EQ all follow
the selected shapes; changing shape preserves the band's frequency, gain and Q.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `lowGain` | -12..+12 | 0.0 | dB |
| `lowFreq` | 20–500 | 100 | Hz |
| `lowQ` | 0.1–10 | 0.707 | — |
| `lowShape` | 0 (Shelf), 1 (Bell) | 0 | enum |
| `lowMidGain` | -12..+12 | 0.0 | dB |
| `lowMidFreq` | 100–2000 | 400 | Hz |
| `lowMidQ` | 0.1–10 | 1.0 | — |
| `highMidGain` | -12..+12 | 0.0 | dB |
| `highMidFreq` | 500–8000 | 2000 | Hz |
| `highMidQ` | 0.1–10 | 1.0 | — |
| `highGain` | -12..+12 | 0.0 | dB |
| `highFreq` | 2000–16000 | 8000 | Hz |
| `highQ` | 0.1–10 | 0.707 | — |
| `highShape` | 0 (Shelf), 1 (Bell) | 0 | enum |

Frequency number controls step by 1 Hz. Graphic EQ frequency edits are applied on Enter or
focus loss, not on each keystroke, so typing `800` is not interrupted by adjacent-band limits.
Finished entries are constrained to the allowed range; Escape cancels an edit, and an empty
or invalid entry restores the previous frequency. Gain sliders continue to update live.

**Factory presets** set every band: Flat (default), Mud Cut (-4 dB at 300 Hz), Fizz Tamer
(-4 dB at 6 kHz and a -5 dB shelf from 9 kHz, after an amp), Mid Hump (+5 dB at 720 Hz with the
lows and highs eased, a Tube Screamer's curve), Scoop, Presence Lift, Tight Low End (a -6 dB
shelf below 150 Hz, before a high-gain amp) and Glassy Clean. There is no output control, so the
curves move the level: from -3.5 dB (Scoop) to +3 dB (Mid Hump) on the demo DI.

### Pickup Match (`pickup_match`)
Makes the player's pickup sound like another kind: a single coil like a humbucker, or a humbucker
like a single coil (`PickupMatchEffect.h`). Put it first in the chain, before any drive, compressor
or amp, so they hear the new pickup.

**How.** A passive pickup is close to a linear filter. Its inductance resonates with the winding,
cable and pot load into a second-order low-pass with a peak. Matching divides the target pickup's
response by the source's: one biquad whose zeros cancel the source's peak and whose poles put the
target's in. It is unity at DC, plus the two pickups' output difference. Each polynomial is
prewarped at its own peak, so both peaks land where they belong at any sample rate. Going there
and back again (single coil to humbucker, then humbucker to single coil) is flat to rounding.

**Pickups.** Either side can be one of 258 choices: seven generic classes, Custom Design (index 7,
below), then 250 measured models (`PickupModels.generated.h`). Each model is a second-order fit to its measured response,
28 Hz to 20 kHz, into 10 ft of low-capacitance cable and a 500K pot.

The table comes from `node tools/gen-pickup-models.mjs <dir>`, pointed at the folder of measurement
pages, which is not in this repository. The generator:
- Checks that each page holds the pickup it was asked for. Three source pages failed or repeated
  another: two "+" names that the scraper sent unencoded, and one duplicate.
- Records the RMS fit error. About ten models miss by more than 1.5 dB, mostly actives and
  noiseless types whose curves carry their own EQ, and the picker marks them "approximate".
- Keeps the table **append-only**. A preset stores a pickup as its index, so a model keeps its
  place and new ones go on the end. `PickupMatchEffectTests` pins the first entries.

The classes are medians of the measured fits. The fits reach a median of
0.3–0.4 dB RMS up to 12 kHz on passive pickups.

| Class | Resonance | Q | Level |
|---|---|---|---|
| Single Coil (Neck) | 3551 Hz | 1.33 | -0.9 dB |
| Single Coil (Middle) | 3551 Hz | 1.38 | +1.7 dB |
| Single Coil (Bridge) | 3217 Hz | 1.23 | +2.1 dB |
| P-90 | 1577 Hz | 1.09 | +6.6 dB |
| Humbucker (Neck) | 2800 Hz | 1.01 | +7.5 dB |
| Humbucker (Bridge) | 2343 Hz | 0.97 | +9.7 dB |
| Active Humbucker | 4973 Hz | 0.63 | +11.1 dB |

The active class is the roughest. Actives carry their preamp's EQ, fit worst (up to 5 dB), and
vary in level from unit to unit.

| Parameter | Range | Default | Notes |
|---|---|---|---|
| `source` | pickup | Single Coil (Bridge) | "Your Pickup". A picker, not a knob (below) |
| `target` | pickup | Humbucker (Bridge) | "Sound Like". A picker, not a knob (below) |
| `amount` | 0–3 | 1 | 0 is no change, and the audio passes through bit for bit (as it does with the same pickup on both sides). 1 is the chosen pickup. Past 1 it exaggerates the difference (below). Moves the resonance and Q by ratio, the level by dB, and scales String Sensing |
| `levelMode` | Pickup Output / Matched | Pickup Output | Pickup Output keeps the level difference, so a humbucker drives what follows harder. Matched holds the level for an even A/B |
| `maxBoost` | 0–24 dB | 12 dB | Advanced. Caps the treble shelf going to a brighter pickup. The cap is on the prewarped frequencies, so it holds right up to Nyquist at any rate. Without it a P-90 to an active pickup reached +66 dB at Nyquist at 8 kHz |
| `output` | -12..+12 dB | 0 | |
| `sourceDesignType` … `sourceCapacitanceTrim` | | | "Your Pickup" as a custom design: Type, DC Resistance (3–20 kΩ), Wire Gauge (42–44 AWG), Magnet, Cover, Coil Wiring, Capacitance Trim (±50%). Read only while `source` is Custom Design |
| `targetDesignType` … `targetCapacitanceTrim` | | | The same for "Sound Like" |
| `sensing` | 0–1 | 1 | String Sensing: how much of the position and width difference to apply; scaled by Amount too |
| `sourcePosition`, `targetPosition` | From Pickup / Neck / Middle / Bridge | From Pickup | Where each pickup sits on the string, for String Sensing |

**Factory presets:** Single Coil to Humbucker (default), Neck Single Coil to Humbucker, Humbucker
to Single Coil, Neck Humbucker to Single Coil, Single Coil to P-90, and Humbucker to P-90.

**Custom Design** (`PickupDesign.h`) builds a pickup from its construction. Coil L, R and C, with eddy
losses as a shunt resistance across the coil, into the reference load is still a second-order
low-pass, so a design comes out as the resonance, Q and level every other pickup has.

The model is fitted to 397 measured pickups. A back-test predicted each pickup with itself left out
(private research notes, `research/pickup-design`):
- **Inductance:** grows with wire length (DC resistance ÷ the gauge's ohms per metre) to the power
  1.12, from a per-type intercept. Ceramic adds about 19% over A5, and A2/A3 4–7%. Median error 7.5%.
- **Capacitance:** the type's median. Construction does not predict it (18% error whatever is
  known), so **Capacitance Trim** stands in for insulation and wind pattern.
- **Eddy loss:** a per-type shunt. It takes about 3.4 dB off the peak, and the shunt gets peak height
  to 0.7 dB median.
- **Together:** the loaded peak to about **1 semitone** median, against 6–7 semitones of spread
  within a type.

Thinner ground:
- **Brass cover:** from two measured pickups. Nickel silver showed no damping in three.
- **Coil wiring:** series, parallel and split are circuit maths for two matched coils with little
  coupling. Wiring leaves a single coil alone.
- **Magnet grade:** changes inductance only. Pole gauss flips direction between types, so it cannot
  set output.

**Not offered:**
- **P-90 type:** the two data sources disagree by 5.6 semitones.
- **Pickup position:** it combs the harmonics by note, which a fixed filter cannot follow.

A reference load (760 pF, 114 kΩ) puts designs on the catalogue's scale. It is calibrated once, so
that each type's median design lands on its generic class: Strat, Tele and PAF within 0.8 semitone
and 8% Q (`PickupMatchEffectTests` holds it).

The params panel shows a side's design controls in a block of their own, only while that side is
Custom Design. The block also shows what the engine predicts: wire length, L, R, C, peak, Q and
output. These come from `DescribeSettings`, sent with the response curve.

**The picker.** Both pickup choices declare `control: "picker"` and a `labelInfo` entry per label
(see Effect pickers). Each entry gives the family as the group, the brand as the subgroup, a family
picture (`pickup-single`, `pickup-p90`, `pickup-humbucker`, `pickup-active`, `pickup-other`) and
one line of detail. The detail gives the position, the peak, and the output against a generic neck
single coil. The params panel also draws the conversion's response curve, "Pickup Conversion".

**Not in the effect picker, for now.** The UI marks the type `catalogHidden` (`presetV2.ts`), so the FX
library, the add-effect menu and the replace chooser leave it out. The Tone Builder's service also skips
hidden effects. The global chain's node is the way in. A preset that already holds a Pickup Match still
loads, plays and edits it.

**In the global chain.** The pre-chain carries a Pickup Match, `global_pickup`, straight after the input
and bypassed until switched on, so one setting converts the player's pickup ahead of every preset.
It is edited in the input levelling flyout (the chevron under the input meter), on its Pickup Match tab.

**Experimental, behind a feature flag.** Settings → Features → Power Features → Pickup Match
(`features.pickupMatch.enabled`, `Features.PickupMatch`) is off by default. While it is off:
- the flyout has no tab strip and shows the input profiles alone;
- the indicator beside IN stays hidden;
- a global Pickup Match left on is switched off (`pickup.enabled` false), so nothing the player can't
  see changes the sound. This waits for the engine's app settings, since the flag reads as its
  default until they arrive (`haveAppSettingsArrived` in `appSettingsStore.ts`);
- Nano Q's effect picker leaves the effect out. The web UI's pickers leave it out either way (see above).

A preset that already holds a Pickup Match node still loads, plays and edits it with the flag off.

The tab is built from the same pieces as the params panel, so the two never drift:
- the pickers;
- the conversion curve;
- the custom-design blocks;
- knob rows from `ts/flyoutParamControls.ts`, the code the gate flyout uses.

A change goes out as a `pickup.<id>` `setGlobalChainParam` for any of the effect's parameters, and as
`pickup.enabled` for on and off. While it is on, a picture of the target pickup sits beside the IN
label; clicking it opens the tab.

**My Guitars.** A dropdown on the tab holds the player's saved settings by guitar name, in name order. They are Pickup Match's ordinary
user effect presets (`saveEffectPreset` / `deleteEffectPreset`, filed under the effect's GUID), so a
Pickup Match node's Presets button offers the same list.
- Saving sends every parameter and no `nodeId`. The engine looks a node id up in the active preset's
  chain, and the global node is not in it.
- Loading sends only the parameters that differ, as `pickup.<id>`, and leaves on/off alone.
- Which guitar was chosen is not stored. The dropdown shows the one whose every saved parameter the node
  holds now, and the indicator names it. After a knob turn it reads "Unsaved settings" until the guitar
  is saved again. Delete removes the guitar shown.

**String Sensing.** The measurements are electrical, and they leave out what players hear most between
pickups: where a pickup sits on the string, and how wide it senses.
- **Position:** a pickup at distance p from the bridge on scale L hears harmonic n in proportion to
  sin(πnp/L). Near the bridge the low harmonics barely move the string, so a bridge pickup sounds thin
  and a neck pickup full: about 9–10 dB at 100 Hz on a Strat, fading out by 600 Hz.
- **Width:** a coil senses a window of the string. A humbucker's two coils, 18 mm apart, take the upper
  harmonics down a further 2–5 dB above 2.5 kHz against a single coil.

Both act by harmonic number, so by note. `tools/gen-pickup-sensing.mjs` averages them over the notes from
low E to the high E's 12th fret, which holds for chords. It fits each geometry with a low shelf, a peak and
a high shelf, to within 0.5 dB RMS, in `PickupSensing.generated.h`. `npm run check:pickup-sensing` holds
that file to the script, in `verify` and CI.

There are nine geometries: single coil, P-90 or humbucker, at the neck, middle or bridge. Each uses
typical dimensions, not measured ones:
- Fender 648 mm and Gibson 628 mm scales.
- 160/100/41 mm from the bridge for single coils, 152/100/45 mm otherwise.
- Coil windows of 8, 13 and 7 mm.
- 18 mm between a humbucker's coils.

Which geometry a side uses:
- **Family:** actives count as humbuckers, and the Other family as single coils.
- **Position:** the side's Position, or else the catalogue entry's own. A pickup without one (the P-90 and
  active classes, models measured on their own, custom designs) sits where its kind usually does: a single
  coil in the middle, anything else at the bridge.

Conversion runs the target's three bands and the source's three with their gains negated. An RBJ shelf or
peak with its gain negated is the exact inverse, so there and back is flat, and the same geometry on both
sides runs nothing. Max Treble Boost also caps the top a narrower pickup adds.

In the effect's own response (`PickupMatchEffectTests`), String Sensing makes Single Coil (Bridge) →
Single Coil (Neck) 10 dB fuller at 100 Hz against 1 kHz. The electrics alone moved that by 0.1 dB.

**Exaggerating: Amount past 1.** A faithful conversion can be subtle through a drive and a cab, so
Amount runs to 3. Past 1 the conversion carries on the same way, a caricature of the difference.
Single Coil (Bridge) → Humbucker (Bridge), with level matched, is 7 dB down at 3 kHz at 1, 15 dB at 2
and 20 dB at 3. String Sensing scales with it: bridge to neck is +10 dB at 100 Hz at 1 and +19 dB at 2.

Limits keep a caricature stable and usable:
- the resonance stays at 300 Hz or above;
- Q stays between 0.3 and 6;
- the level change stays within 18 dB;
- each sensing band stays within 18 dB.

Where the two pickups' own values already go further than a limit, the limit widens to include them, so
nothing up to 1 is held back. The catalogue's widest level gap is 36 dB, and it still comes through at 1.
Max Treble Boost still caps treble added at any Amount.

**Not modelled.** The measurements are electrical only. A humbucker's two coils, and a pickup's
distance from the bridge, comb the string's harmonics by harmonic number. The notches move with the note played, so a fixed filter cannot follow them exactly; String
Sensing applies their average. The classes also assume the
measured load. A short cable, a 1M input or a rolled-off volume knob moves a real pickup's peak.

### Pitch Tracking EQ (`eq_pitch_tracking`)
**Experimental**: offered in the FX library (and Nano Q's effect picker) only with the
Experimental Effects feature on.

An EQ whose bands follow the note being played. A fixed band sits on a different harmonic of
every note; here each band is placed at a multiple of the fundamental `dsp/PitchTracker.h` finds
(see *Pitch tracker* under Ring Modulator), so it lifts or cuts the same harmonic, and the tone
keeps the same character, anywhere on the neck.

    in -+- band 1 - band 2 - band 3 - low-pass - x Level - out
        +- pitch tracker (mono sum) -> each band at Harmonic x f0, the low-pass at Cutoff x f0

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `harmonic1` / `2` / `3` | 0.5–16, half steps | 2 / 4 / 6 | × f0 |
| `gain1` / `2` / `3` | −18 to +18 | 4 / 3 / 2 | dB |
| `width1` / `2` / `3` | 0.1–2, log taper | 0.5 | harmonics |
| `lowPass` | Off / On | Off | enum |
| `cutoff` | 1–32, log taper (shown with the low-pass on) | 8 | × f0 |
| `glide` (advanced) | 0–200 | 20 | ms |
| `lowestNote` (advanced) | E2 / D2 / B1 / F#1 | E2 | enum |
| `level` | −12 to +12 | 0 | dB |

- **Bands** are RBJ peaking sections. Harmonic 1 is the fundamental, 2 its octave, 3 the octave
  and a fifth, 0.5 an octave below. Width is the bandwidth in harmonics, that is in units of f0,
  so Q is Harmonic / Width (held to 0.2–80): a width under 1 touches one harmonic whatever note
  is played. At width 0.5, +12 dB on the 3rd harmonic moves the 2nd and 4th by 0.6 and 1.1 dB.
- **Low-pass** is key-tracked as a synth filter is: a 12 dB/octave Butterworth at Cutoff × f0, so
  low notes are not fizzy and high ones not dull. At Cutoff 4 the 8th harmonic loses 12.3 dB on
  a G2 and 12.5 dB on a G4.
- **Tracking.** Until a first note is found the effect is flat, sample for sample; it then fades
  in over 15 ms. The followed pitch glides to each new note over Glide, in octaves, and the
  coefficients ramp every 32 samples. Through silence and chords, which the tracker cannot read,
  the bands stay on the last note it held. The tracker needs a few periods of a note, so the
  attack of each new note goes through with the previous note's EQ (15–40 ms, longest on low
  notes). Lowest Note narrows the tracker's window for a guitar that never goes below it.
- A band that the note pushes past 0.45 of the sample rate fades out over the half-octave below,
  rather than piling up at the top.
- Read-only feedback: `trackedFrequency` (the tracker's pitch), `fundamental` (the pitch the
  bands follow, after Glide), `bandFrequency1`–`3` and `lowPassFrequency` (0 before a first note,
  or with the low-pass off).
- About 2.3 µs per 64-sample stereo block at 48 kHz in Release. On 20 s of the demo DI riff every
  preset adds nothing above 12 kHz (no zipper or clicks from the moving filters), and the
  tracker holds a guitar pitch for 98% of blocks.

**Factory presets** set everything but Level and Lowest Note, the player's own. The first holds
the defaults, and it is what a new node starts with.

| Preset | What it does |
|---|---|
| Even Warmth | 2nd, 4th and 6th harmonics up 4, 3 and 2 dB |
| Odd Bite | 3rd, 5th and 7th up 5, 3 and 2 dB: reedier, more forward |
| Hollow | 2nd, 4th and 6th cut 9, 7 and 5 dB: a hollow, clarinet-like tone |
| Singing Fundamental | Fundamental +6 dB, 2nd +2 dB, 8th −3 dB, for leads |
| Tame Fizz | 7th, 9th and 11th cut 5 dB, low-pass at 12 × f0, for a fizzy drive |
| Even Brightness | Only the key-tracked low-pass, at 6 × f0 |

### Digital Delay (`delay_digital`)
Clean stereo digital delay, with its tone filters and drive inside the feedback loop.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `time` | 1–2000 | 300 | ms (`timeMs` is accepted as an alias) |
| `syncMode` | Free / Tempo | Free | — |
| `syncDivision` | 1/1 … 1/32T | 1/4 | — |
| `feedback` | 0.0–0.95 | 0.4 | — |
| `mix` | 0.0–1.0 | 0.3 | — (dry × (1 − mix) + wet × mix) |
| `highCut` | 200–20000, log | 8000 | Hz, one-pole, in the loop |
| `lowCut` | 20–5000, log | 20 | Hz, one-pole, in the loop |
| `drive` | 0.0–1.0 | 0.0 | — (saturates the feedback write; the first repeat stays clean) |
| `stereoMode` | Normal / Ping-Pong | Normal | — |
| `spread` | 0–50 | 0 | ms on the right tap (advanced) |
| `modRate` | 0–10 | 0 | Hz (advanced) |
| `modDepth` | 0–20 | 0 | ms either way (advanced) |
| `ducking` | 0.0–1.0 | 0.0 | — (advanced; the wet dips while you play) |
| `direction` | Forward / Reverse | Forward | — |

Time is not smoothed, so changing it (or choosing a preset) jumps the read head; the tape and
analog delays glide.

**Ping-Pong** feeds the two lines from the input's mid and side: 0.8 of the mid plus the side on
the left, 0.2 of the mid less the side on the right, and the feedback crosses between them. With
the same signal on both sides that is a centred feed with a skew for the repeats to bounce on; a
stereo input keeps its image. Nothing is decided from the samples. In dual mono ping-pong is off
and each side repeats on its own.

**Reverse** plays each Time-long slice of the input backwards. Two read heads half a slice
apart each read twice as far back as they are into their slice, so each runs backwards through
the last slice at normal speed, from now to a slice ago; each is faded in and out with a sin²
window and the two windows sum to one, so no slice starts or ends with a click. Slices shorter
than 10 ms are held at 10 ms. Spread lengthens the right channel's slices. The repeats that
Feedback sends round are reversed again, so they alternate between backwards and forwards, as
most reverse pedals' do. The line holds 4.2 s, two of the longest slices. On a guitar phrase,
Reverse sits about 1.3 dB under Forward.

**Factory presets** set every control, the advanced ones too, and leave out Division unless the
preset is tempo-synced: DD-3 (default), Slapback (100 ms, one repeat), Dotted Eighth (1/8 dotted
at the song's tempo, 375 ms when Sync is off), Ping-Pong, Ducked Lead (the echoes bloom in the
gaps), Ambient Wash (650 ms, long and dark, modulated and spread) and Lo-Fi Echo (telephone-band
repeats, driven and warbling), Reverse (550 ms slices swelling in behind the note) and Reverse
Wash (800 ms, longer feedback, darker and spread).

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
| `quality` | 0–3 | 3 | — |

Quality sets how much of the IR is kept: 0=Economy (1.5 s), 1=Standard (3 s), 2=High (6 s),
3=Full (all of it). A tier shorter than the IR cuts it 256 samples after 99.9% of its energy
instead, when that comes first. The last 2048 samples kept are faded out. The IR is resampled to
the host rate once per IR and rate, and normalised by its L2 norm, anchored to 48 kHz so the level
does not change with the host rate.

**Quality** is built into the convolvers: where the IR is cut, and so its normalisation gain, into
the coefficients. A preset sets it before its IR loads, so loading one rebuilds nothing extra. A change once the IR is loaded,
from the node panel or from MIDI or DAW automation, is not built in `SetParam` (see *Where SetParam
runs*). The message thread builds it off the DSP lock and swaps it in under the lock. The work
shares the resampled impulses rather than copying them, since a reverb IR runs to seconds and the
work is taken under the lock. There is no crossfade: the new convolvers start with no history, so
the tail of what was playing stops at the swap. A change from the node panel is built before its
message returns, and one from automation at the next idle tick. `GetParam` reports the requested
value straight away.

The reverb has no latency: it runs the same zero-latency convolver as the IR Cabinet. Until
2026-10-16 its Low Latency switch, on by default, reported 64-256 samples, though its dry signal
passed undelayed, so a host compensating for that latency pulled the dry guitar 1.3-5.3 ms early.
The wet signal also trailed the dry one by that much, so below 100% Mix the two combed against
each other wherever the IR has early energy. Now they line up, the same at every buffer size. On the factory pack's
two bundled reverb IRs (Mix 39% and 70%) it moved the third-octave response by up to 1.25 dB
against the old 64-sample offset, and against the old 256-sample one by 2.3-3.4 dB at 63 Hz and
0.6 dB at most above it.

Low Latency off reported its partition, 256 to 2048 samples from the largest block the host
promised, and from later on 2026-10-16 delayed the dry by the same amount (`DryPathDelay`), so the
switch changed when the reverb sounded, not how. Reporting no latency and calling the wet's trail a
pre-delay would have kept the old sound exactly, but that trail moved the factory pack's two
reverb nodes 1.5-1.7 dB at 80 Hz between a 64- and a 1024-sample host block. On a 3 s IR off also
cost 1.8-3.7x the CPU of the zero-latency convolver at every block up to 512, so on 2026-10-17 it
was retired with the IR Cabinet's. A preset or MIDI binding that still sets `lowLatency` is
ignored.

**Even cost per block.** An IR over 32,768 samples (0.68 s at 48 kHz) runs its FFT stages spread:
each stage computes a block's output over the samples of the next block, instead of doing both
FFTs in the callback where its partition closes. Until 2026-10-19 every stage closed together every
4096 samples, and that callback cost 5-10x the median; on a Galaxy S21's big cores the reverb was
the main source of the app's DSP load peaks. Spread, a 3 s IR at 96-sample blocks has a p99 within 1.2x
of its median (it was 40x), at the same average CPU or less, with the same output to float
rounding (docs/plans/zero-latency-ir-cab.md, *Per-callback cost of a long IR*). The cab's IRs are
shorter than that and keep the layout they had.

**Real-input FFTs.** Every FFT stage, cab or reverb, transforms its real samples with a real-input
FFT (`RealFFT.h`), half the work of the complex FFT it replaced, and keeps half the spectrum, and
the FFT and multiply-accumulate have NEON paths for arm64 as well as SSE. Since 2026-10-19 the
convolver costs about half what it did on x64 and on a Galaxy S21's big cores, from cab IRs to 10 s
reverbs, with the same output to float rounding (docs/plans/zero-latency-ir-cab.md, *Real-input
FFTs and NEON*).

### Algorithmic Reverbs
Room, chamber, and advanced reverb share a common algorithmic engine. Spring and ambient use dedicated processors because their topology and voicing diverge more strongly from the shared room/chamber design:

- `reverb_room`
- `reverb_chamber`
- `reverb_spring`
- `reverb_advanced`
- `reverb_ambient`
- `reverb_verbal` (twelve machines on its own engine, below)

Each effect exposes the controls most relevant to that style rather than sharing one universal surface.

**Shared blocks.** `core/src/dsp/reverb/ReverbBlocks.h` holds what the reverbs share: the
division-free delay reads (`ReadDelay`, `ReadDelayLinear`), the rotating-phasor LFO, the RT60 to
loop-gain law, the spring tanks' in-loop `ChirpDispersion`, a fractional Schroeder allpass, the
three-band loop damping and the 16-point Hadamard mix. Room, Chamber, Advanced, Spring and Ambient
read their delays through it (Spring and Ambient used to wrap with `%`) and Spring disperses through
it, bit for bit as before (checked under strict floating point). All five scrub a non-finite input
sample to silence, so one bad sample from upstream no longer latches the tank silent until a
reload, and clear the tank if something non-finite reaches it anyway.

**Wet level.** Each carries a fixed makeup gain on its wet path: Room +10.3 dB, Chamber +14.3,
Advanced +13.5 (whatever its Character), Spring +13.5, and Ambient +12.4 on the late reverb alone,
where its diffusers sit, so the early reflections keep their balance against it. Until 1.6.0 the
diffusers were resonant combs rather than allpasses, and their gain, which also rose with
Diffusion, set the wet level every Mix default and saved preset was balanced against. Fixing them
took 10-15 dB out of the wet path at the default settings. The makeup puts each type back where it
was at those settings, measured on guitar clips raw and through a cab, and it follows no control, so
Diffusion stays out of the level. Ambient is matched at 1.5.0's default Decay (0.7). At 100% wet a
guitar comes out between -2 and +10 dB of where it went in, depending on the type, close to the
+1 to +11 dB the Convolution Reverb's unit-energy normalisation gives with the factory IRs.

**Keeping the tail from ringing.** A turned-up Mix exposes any tone the tail holds, and three
things used to give it one. They are measured on the impulse response's late tail: the whitened
autocorrelation, where a peak at lag τ is a pitch or flutter at 1/τ, and the spectral peaks above a
third-octave average. Real and high-end reverbs (Lexicon 480L, Bricasti and EMT 140 captures)
measure 0.07-0.27 at 22-57 ms and peaks of +15-21 dB.
- *Output diffusers* are 2-7 ms with gain up to about 0.65. At 12-18 ms and up to 0.92 they held
  their resonances (multiples of 1/M) for up to M(1+g)/(1-g), and the tail rang at 60-80 Hz:
  Room 0.37 at 12 ms, Chamber 0.60 at 17 ms.
- *Early taps* feed each comb line with its own signs (columns of a Hadamard matrix). Fed alike,
  every line carried the same comb, the first tap ~5 ms behind the direct sound, and so did the
  whole tail.
- *Spring* disperses on every pass of each tank loop (16 first-order allpasses stretched over
  K = 4 samples at 48 kHz, scaled with the rate), not once at the input, where each echo came back
  the same shape and the tail fluttered at the loop times (0.51 at 53.5 ms; real spring captures
  measure about 0.1).

Now: Room 0.16, Chamber 0.17, Advanced 0.23 and Spring 0.18, all at 54-69 ms, with peaks of
+15-21 dB.

**Factory presets** (`ReverbPresets.h`, listed under each reverb below) set every control their
reverb declares, `mix` included, but not Ambient's `outputGain`: the level the player has set stays
theirs. Each list opens with its default, which is the parameter defaults themselves, so a node added
fresh sounds as it did before there were presets. `ReverbPresetTests` holds them to that, and renders
each to check it stays finite, dies away and sits near its default's level. On guitar through a cab
every preset's output is within -1.2 to +2.9 dB of its default's. The long Advanced halls lean on
Mod Depth: a decay that long resolves the comb modes, and modulation is what smears them.

#### Room Reverb (`reverb_room`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.46 | — |
| `size` | 0.0–1.0 | 0.42 | — |
| `damping` | 0.0–1.0 | 0.56 | — |
| `preDelay` | 0–220 | 8.0 | ms |
| `mix` | 0.0–1.0 | 0.22 | — |

Factory presets: Studio Room (default), Small Room, Bright Room, Live Room, Large Room, Warm Room.

#### Chamber Reverb (`reverb_chamber`)

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.60 | — |
| `size` | 0.0–1.0 | 0.56 | — |
| `tone` | 0.0–1.0 | 0.52 | — |
| `preDelay` | 0–220 | 15.0 | ms |
| `mix` | 0.0–1.0 | 0.24 | — |

Factory presets: Echo Chamber (default), Small Chamber, Bright Chamber, Dark Chamber, Large Chamber.

#### Spring Reverb (`reverb_spring`)

Dedicated spring-tank model with short dispersive delays, resonant drip emphasis, and nonlinear tank drive.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.42 | — |
| `tone` | 0.0–1.0 | 0.52 | — |
| `drive` | 0.0–1.0 | 0.18 | — |
| `mix` | 0.0–1.0 | 0.18 | — |

Factory presets: Amp Spring (default), Surf Spring, Short Spring, Dark Spring, Bright Spring, Long Spring.

#### Advanced Reverb (`reverb_advanced`)

Common controls:

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `character` | 0–2 | 0 | Room, Chamber, Spring |
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
| `lowCut` | 20–1200 | 140 | Hz, log taper |
| `highCut` | 1000–20000 | 12000 | Hz, log taper |
| `modRate` | 0.02–8.0 | 0.45 | Hz |
| `modDepth` | 0.0–1.0 | 0.18 | — |
| `ducking` | 0.0–1.0 | 0.08 | — |
| `drive` | 0.0–1.0 | 0.00 | — |

Factory presets: Wide Room (default), Concert Hall, Bright Plate, Ducked Lead, Lush Hall, Gritty Spring.

#### Ambient Reverb (`reverb_ambient`)

Long, diffuse late reverb with soft early reflections, slow modulation, and a wide stereo bloom.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `decay` | 0.0–1.0 | 0.50 | — |
| `space` | 0.0–1.0 | 0.72 | — |
| `diffusion` | 0.0–1.0 | 0.40 | — |
| `preDelay` | 0–200 | 26.0 | ms |
| `tone` | 0.0–1.0 | 0.42 | — |
| `width` | 0.0–1.25 | 1.08 | — |
| `modRate` | 0.02–2.0 | 0.18 | Hz |
| `modDepth` | 0.0–1.0 | 0.38 | — |
| `mix` | 0.0–1.0 | 0.28 | — |
| `outputGain` | -18..+12 | 0.0 | dB |
| `shimmer` | 0.0–1.0 | 0.0 | — |
| `shimmerPitch` | Octave Up / Fifth Up / Octave + Fifth / Octave Down | Octave Up | — |
| `freeze` | Off / On | Off | — |

**Shimmer** feeds the late reverb back into the combs through a pitch shifter
(`GrainPitchShifter.h`: two sin²-windowed taps sweeping a 70 ms line, cheap and with no latency
of its own, and smooth enough on a reverb's wash), so each pass round the tank comes back
`shimmerPitch` higher and the tail climbs into a halo. The return is band-limited (120 Hz to
7 kHz) and soft-limited, and each interval's loop gain at full Shimmer is the most that still
lets the tail die away with every other control at its maximum: a fifth climbs out of the damped
band more slowly than an octave, so it gets two-thirds of the octave's. With Shimmer at full and
everything else up, the tail is still 17 dB down 18 s later. At the default Mix, Shimmer adds
under 1 dB to the level on a guitar.

**Freeze** holds the tail: the combs' feedback goes to one and their damping off, and the tank
stops taking input (the shimmer return and the early reflections too), over 60 ms. What was
ringing rings on unchanged under whatever is played next; turning it off lets the tail decay
from there at the Decay setting. Map it to a footswitch.

Factory presets: Wide Bloom (default), Tight Ambience, Lead Halo, Dark Swell, Cloud, Infinite Wash,
Shimmer (an octave up), Fifth Halo and Deep Shimmer (an octave down, an organ-like swell).

#### Verbal (`reverb_verbal`)

Twelve reverb machines behind one set of controls: **Room** (Studio or Club), **Hall** (Concert or
Arena, with a Mid EQ), **Plate** (Small or Large), **Spring** (1-3 springs, Dwell), **Swell**,
**Bloom**, **Cloud**, **Choir**, **Shimmer**, **Tape Echo**, **Nonlinear** and **Reflections**.
The code is in `core/src/dsp/reverb/`: `VerbalEffect.h` (the effect), `VerbalEngine.h` (one
machine running), `FdnTank.h` (the shared late reverb), `VerbalStages.h` (diffusers, early
reflections, the room model, formants, the swell envelope, the shaped generator),
`VerbalSpring.h`, `VerbalParams.h` (every range and label) and `VerbalPresets.h`.

Common controls:

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `machine` | Room … Reflections (a picker) | Hall | — |
| `decay` | 0.2–60, log taper | 3.0 | s |
| `preDelay` | 1–1500, log taper | 20 | ms |
| `tone` | 0.0–1.0 | 0.6 | — |
| `mod` | 0.0–1.0 | 0.4 | — |
| `lowEnd` | 0.0–1.0 (0.5 neutral) | 0.5 | — |
| `mix` | 0.0–1.0 | 0.35 | — |
| `hold` | Off / On | Off | — |
| `holdMode` | Infinite / Freeze | Infinite | — |
| `outputGain` | -12..+12 (advanced) | 0 | dB |

Each machine's own controls sit in the Machine group and show only under that machine
(`visibleWhen`, see Conditional controls): Room `roomSize`, `diffusion`; Hall `hallSize`, `midEq`;
Plate `plateSize`; Spring `dwell`, `springs`; Swell `riseTime` (0.08-4 s), `swellMode` (Wet/Dry);
Bloom `bloomLength`, `bloomFeedback`; Cloud `diffusion`; Choir `vowel`, `resonance`; Shimmer
`shimmerMode` (Input/Regen/Input + Regen), `shift1` (28 intervals, -Oct to +2 Oct), `shift2` (Off
or the same), `shimmerAmount`; Tape Echo `echoTime` (200-1500 ms), `echoFeedback`, `heads` (3/4/6),
`spacing` (Even/Uneven), `diffusion`; Nonlinear `shapeTime` (50-2000 ms), `shape` (Swoosh,
Reverse, Ramp, Gate, Gauss, Bounce), `shapeFeedback`, `lateLevel`, `modSpeed`, `diffusion`;
Reflections `size` (100-1000 sq ft), `roomShape` (Square/Rectangle/Oblong), `listenerX`,
`listenerY`. A machine's settings are the node's: changing machine keeps every value.

**The tank.** Every machine but Spring runs through `FdnTank`: sixteen modulated delay lines mixed
by an orthonormal Hadamard matrix, their lengths spread per machine and kept coprime. Each line's
loss is solved from the decay asked for in three bands, so **Decay is the measured RT60**: Hall,
Room, Plate, Bloom and Swell measure within 3% of the knob from 0.6 to 16 s (VerbalTests holds Hall
and Room to 12% from 1 to 8 s). Low End sets the low band's decay (0.55x to 1.25x the mid's) and a
shelf on the output; Tone the high band's decay and an output low-pass whose range is the machine's.
The two outputs are orthogonal sums of the lines, so the tail is uncorrelated left to right even
from a mono input (|r| under 0.1), and the late-tail ringing metric (reverb-level-and-ringing in
the dev notes) reads 0.02-0.06 on the tank machines. The Cloud runs about twice its Decay, by
design: its eight-stage input diffuser cascade is an early reverb of its own, and at Diffusion 0
those stages are plain delays, so the attack is grainy.

**Mix** adds the reverb to an untouched dry: the dry stays at unity to 0.9 and fades out over the
last tenth, so the top of the knob is fully wet. The wet is -24 dB at 0.1, -10 dB at 0.5 and unity
from 0.9. Each machine's wet level is set so that at full Mix a guitar comes out within about 2 dB
of where the same kind of reverb sits in a dedicated multi-machine pedal reverb, -11 dB for
Reflections to +4 dB for Cloud.

**Hold.** Infinite pulls every band's loop gain to one over about 30 ms and keeps the input open,
so what is played joins the held tail; a soft ceiling in the tank (RailClip at 0.9 to 1.4) bounds
it. Freeze does the same and also closes the input over 20 ms, so the held tail sits under what is
played next. Held, the modulation drops to 30% of its depth so the tail does not smear away.
Releasing either lets the tail decay from where it is.

**Changing machine** hands the tail to the effect's second engine, which rings out at the old
machine's decay while the first starts the new one: no cut, no click. A change made while an earlier
tail is still ringing is applied in place instead, since clearing that engine mid-tail would click.
The incoming engine's buffers are cleared on the audio thread, which is the one costly step and
happens only on a deliberate machine change. Size changes glide the tank's lengths over about
120 ms rather than flushing it.

**Machines.**
- *Spring*: one to three loops a side, each dispersing on every pass through the shared
  `ChirpDispersion` the Spring Reverb uses. Dwell is a tank preamp from clean to a hard rail: at
  Overdrive a -36 dBFS pick comes out about 14 dB hotter than at Clean, and everything from
  -24 dBFS up lands within a couple of dB. Low End's first half is the steep low cut of a real
  tank's drive circuit.
- *Swell*: a gain that restarts at each picked note and rises over Rise Time. Wet swells the
  reverb in behind the dry; Dry swells everything, like a volume pedal ahead of the reverb.
- *Bloom*: eight long allpasses ahead of the tank, with feedback round them. Length moves the
  envelope's peak from about 230 to 790 ms; Feedback stretches the tail up to about 4x.
- *Choir*: the tail through a vowel's first three formants and the singer's formant (2.8 kHz), in
  parallel state-variable band-passes; the double vowels glide between their two, Random wanders
  over five, Resonance is the Q.
- *Shimmer*: two pitch voices (`GrainPitchShifter`, 60 ms) on the tank input, in its loop, or both,
  band-limited and soft-limited in the loop so the climb settles. Intervals land within about
  ±0.3 semitones.
- *Tape Echo*: one tape with 3, 4 or 6 heads (Even at k/N of Echo Time, Uneven scattered), wow and
  flutter from `TapeTransport` at Mod's depth, Low End and Tone inside the feedback loop with
  asymmetric tape saturation, and the heads diffused into the tank by Diffusion. Feedback 0.83
  rings for about 15 s at 400 ms. Decay sets the wash.
- *Nonlinear*: 48 taps over Time, their levels following the Shape, feedback round the generator
  so the shape repeats, and a late tank at Late Level with Decay as its RT60.
- *Reflections*: an image-source model of a 9 ft high room, up to fourth order on the floor plan,
  the strongest 64 images kept, each arriving at each ear with its own delay and level. X pans the
  dry and moves the reflections; Y further back makes the reflections louder against a quieter,
  more central dry. Damping (Tone) is the walls' absorption and darkens the later orders. Mod
  drifts the whole pattern against the dry.

**Cost.** 20-70 µs per 128-sample block at 48 kHz on x64 for every machine except Reflections
(about 110), measured in the Release harness. The tank machines allocate about 3.5 MB per engine,
two engines per node.

Factory presets: Concert Hall (default), Studio Room, Club Room, Arena, Studio Plate, Small Plate,
Surf Spring, Combo Spring, Violin Swell, Slow Bloom, Cloud Nine, Choir, Octave Shimmer, Fifth
Shimmer, Tape Echo Wash, Reverse Reverb, Gated and Small Room Reflections.

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
Speaker Drive, being level-dependent, is not captured) and **Match an IR…** (`matchSimpleCabToIr`:
tries every cabinet and mic type and searches the tone controls for the closest 1/6-octave
shape to a library IR, keeping the node's Output, Auto Level and Speaker Drive). The IR is
picked in the resource browser, opened without a node so a click selects rather than
auditions; its Folder and Tone3000 tabs import the pick into the library first.

Twenty-seven factory presets ship with it; the first, **Closed 4x12**, is the defaults and starts new
nodes. Presets set every control except Output. Besides the seven generic voicings there are twenty
popular cabinets: Deluxe 1x12, AC30 2x12 Blue, Twin 2x12, JC-120 2x12, DC30 2x12, Vintage 15-inch,
Closed 2x12 V30, Closed 2x12 EVM12L, Super Reverb 4x10, Recto 4x12 V30, 1960 4x12 V30, 1960 4x12
Greenback, 1960 4x12 G12T-75, PPC412 V30, XXL 4x12 V30, Fane 4x12, Legacy 4x12 V30, Alnico Cream
4x12, Boutique 4x12 and SVT 8x10 Bass. Each was fitted to reference IRs of that cabinet with the
IR match, its cabinet type held to the real one, and they have Auto Level on so stepping through
them holds the level. `TestFactoryPresetsAreDistinct` keeps every pair of presets at least 1.3 dB
apart in response shape (RMS, 60 Hz to 10 kHz, level removed); a pair that differs in Speaker
Drive or Stereo Spread is exempt, since that is what tells them apart.

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
  LPB-1 and the Rangemaster are the exceptions: they are boosts, and Drive is how much.
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
| Rangemaster | Germanium treble booster: up to +26 dB of the upper mids and treble, added to the clean signal, so the bass passes at its own level while the top end is pushed into the amp and into soft, lopsided germanium saturation. Unity at Drive 0, about +9 dB at half. Bass moves the treble corner two octaves either way about 1.8 kHz, the "range" modification |

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

### Multi-band Distortion (`multiband_distortion`)
A distortion in four frequency bands, each driven into a clipper of its own with its own Drive,
Character and Level. One clipper fed a whole chord intermodulates every note with every other,
and the low strings push it hardest, so chords smear and the lows flub; split first and each
band only intermodulates with itself, so the lows can stay tight while the mids scream.
Engine: `MultibandDistortionEffect.h`, with the parameter table, curves and level-match table in
`MultibandDistortionSupport.h`. Plan: docs/plans/multiband-distortion.md. Category `drive`.

- **Bands** are the Multi-band Compressor's: the same three movable crossovers, ranges and
  defaults (200, 800, 3000 Hz), the same LR4 split with all-pass compensation
  (`dsp/LinkwitzRileyCrossover.h`, which also holds the ordering both use). With every band
  clean they add up to a flat all-pass.
- **Oversampled once, as a whole.** The input goes up through the drive pedals' minimum-phase
  half-bands (`dsp/HalfBandIir.h`), is split, clipped and summed near 192 kHz (4× at
  44.1/48 kHz, 2× at 88.2/96, 1× above) and comes down once, so every band and its clean copy
  share one phase. Each clipper is antialiased (`drive::ClipCurve::Antialiased`). A 5 kHz tone
  hard-clipped at full drive leaves its fold-backs 65 dB under it. The antialiasing averages
  successive oversampled samples, which takes 0.3 dB off 16 kHz even when clean.
- **Drive** is 0..48 dB into a knee 12 dB over the nominal operating level (`LevelTargets.h`),
  so at 0 a normal note passes all but clean. It is **level-matched**: a table per Character,
  built once, of how much RMS the curve loses on a nominal sine at each dB of Drive, relative
  to Drive 0, scales the clipper's output back up. Turning Drive up changes how dirty a band
  is, not how loud a nominal note is; quieter playing comes up relative to it, as through any
  clipper. At high Drive whatever leaks into a band from its neighbours is driven up too, so
  the bands tend to even out: that is what a multi-band clipper does.
- **Character** per band: Tape (atan, the softest), Tube (soft knee, negative side at 0.7: even
  harmonics), Diode (soft knee), Hard (flat limit) and Fuzz (flat limit, negative side at 0.5).
  A change crossfades the band from the old curve to the new over 10 ms.
- **Mix** blends each band's driven signal with the same band clean, in phase; **Level** trims
  the band after it; **Output** the sum. Drive, Level, Mix and Output glide over 20 ms, worked
  out every 16 samples and ramped per sample. A 2 Hz DC blocker follows the sum (the lopsided
  curves make DC), then the drive pedals' soft ceiling near 0 dBFS.
- **No latency** reported, as for the drive pedals. About 24 µs per stereo 64-sample block in
  Release (1.8% of the 48 kHz deadline), a little under the 8× Distortion pedal.

| Param | Range | Default | Notes |
|---|---|---|---|
| `crossover1`..`crossover3` | 40..1000, 150..5000, 600..12000 Hz | 200, 800, 3000 Hz | Group Crossovers, log taper |
| `drive1`..`drive4` | 0..48 dB | 18, 28, 36, 30 dB | Groups Low, Low Mid, High Mid, High |
| `character1`..`character4` | Tape, Tube, Diode, Hard, Fuzz | Tube | Enum |
| `level1`..`level4` | -24..+12 dB | 0 | After the mix |
| `mix` | 0..1 | 1 | Per band, in phase |
| `output` | -18..+18 dB | 0 | |

**The node panel** is the Multi-band Compressor's (`multibandPlot.ts`,
`signalPath/paramsPanel/multibandPanel.ts`): input spectrum shaded, output as a line (the gap
up top is the harmonics each band adds), the crossovers draggable. What hangs from the top of
each band, in red, is how far its peaks were driven past the knee since the last frame; the
strip under the plot is each band's peak level against the input level where its clipper
starts to bend. The engine sends that level as each band meter's `thresholdDb`, since it
depends on the nominal level the UI does not know.

**Factory presets:** Standard (default), Tight Crunch (low band near clean from 160 Hz), Chord
Clarity (every band alike, Tape at the ends), Fat Lows (Tape low band at 24 dB from 250 Hz),
Mid Scream (High Mid Diode at 34 dB), Fizz-free Lead (top band nearly clean, -6 dB), Parallel
Grit (Hard at Mix 0.4) and Splatter (Fuzz up top).

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
| `softClip` | 0.0–1.0 | 0.0 | — (advanced) |
| `stereoLink` | Independent / Linked | Linked | — (advanced) |

A peak detector feeds a soft-knee gain computer, and Attack and Release smooth the gain
reduction (in dB) rising and falling. On a steady sine the smoothing leaves it a little under
the ideal peak curve: 9.4 dB of reduction where 4:1 on a peak 14 dB over would take 10.5.
Fast settings cost some low-frequency distortion, as on any fast compressor: 0.6% THD on a low E
at 10/100 ms, 1.5% at 1/50 ms.

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
| `softClip` | 0.0–1.0 | 0.0 | — (advanced) |
| `stereoLink` | Independent / Linked | Linked | — (advanced) |

An RMS detector (5 ms) feeds a hard-knee gain computer, and lands on the ideal curve. The cell
releases more slowly the more gain reduction it holds, a little like a photocell.

**Both compressors**

- **Stereo Link**, as on the gate. Linked (the default), one detector on the louder channel sets
  one gain for both, so a stereo image after a chorus or a ping-pong delay holds still.
  Independent, each channel is compressed on its own level, as both always were before the
  switch; `StereoProcessingTests` holds that mode to it. A mono signal is compressed the same
  either way. Presets leave the switch alone.
- **Makeup and Mix glide** over 20 ms, so turning them, automating them or choosing a preset
  never steps the level mid-note.
- **A NaN or an infinity in the input** is scrubbed to silence before the detector sees it.
  Before, one infinity left either compressor putting out NaN for good, and one NaN left the
  Opto never compressing again (`CompressorEffectTests`).
- **Threshold is in dBFS**, not relative to the nominal operating level, so a hotter rig
  compresses harder.

**Factory presets** (`DynamicsPresets.h`) set every control. They are voiced on the demo DI (median
peaks near -7 dBFS), and Makeup brings each to within 1 dB of the level it came in at, so
switching one on changes the dynamics, not the volume; the default, being the parameter
defaults, has no makeup and sits 1.5-3 dB lower.

| VCA | Settings |
|---|---|
| Classic VCA (default) | The defaults: -20 dB, 4:1, 10/100 ms, 6 dB knee |
| Chicken Pickin' | -32 dB, 8:1, 4/150 ms: snap on the pick, then squash |
| Funk Clean | -28 dB, 4:1, 15/80 ms: lets the pick through for percussive cleans |
| Transparent Leveler | -24 dB, 2:1, 20/250 ms, 12 dB knee: evens out, barely heard |
| Parallel Squash | -38 dB, 12:1, 1/80 ms under the dry signal at 40% |
| Lead Sustain | -36 dB, 10:1, 5/400 ms, a little soft clip |
| Peak Catcher | -10 dB, 20:1, 0.5/60 ms, hard knee: only the loudest hits |

| Opto | Settings |
|---|---|
| Smooth Opto (default) | The defaults: -20 dB, 3:1, 20/300 ms |
| Gentle Leveler | -26 dB, 2:1, 30/600 ms |
| Studio Leveling | -30 dB, 4:1, 10/500 ms |
| Clean Sustain | -34 dB, 6:1, 15/1200 ms |
| Slow Bloom | -32 dB, 4:1, 120/1500 ms: the pick through, then the swell held |
| Parallel Bloom | -36 dB, 8:1, 40/800 ms under the dry signal at 50% |
| Opto Limit | -20 dB, 20:1, 5/250 ms, a little soft clip |

### Chorus (`chorus`)
A modulated delay: a sine LFO swings the delay either side of Delay, the right channel 90°
ahead of the left. So a mono input comes out stereo while Depth and Mix are both above zero,
and the nodes after it keep both sides; an amp or drive pedal there runs its stereo path (a NAM
model at about twice the CPU) until one of them is turned to zero.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `rate` | 0.1–10 | 0.5 | Hz |
| `syncMode` / `syncDivision` | Free / Tempo, 1/1 … 1/32T | Free, 1/4 | — |
| `depth` | 0–20 | 2.0 | ms either way |
| `delay` | 1–30 | 15 | ms |
| `feedback` | 0.0–0.95 | 0.1 | — (undamped: keep it low) |
| `mix` | 0.0–1.0 | 0.3 | — |

Pitch wobble is about 10.9 × Rate × Depth cents. **Factory presets** set every control but
Division: Studio Chorus (default), CE-2, Small Clone, Lush Chorus, Double Track, Vibrato (fully
wet, so pitch alone, as a CE-1's vibrato) and Warble. Within 0.8 dB of the default on the demo
DI, but for Vibrato, which sits at the bypass level, 2.2 dB over the default's mix.

### Flanger (`flanger`)
A short delay swept from Delay up to Delay + Depth, the right channel 90° ahead, with feedback
through a 6 kHz low-pass and a soft limit. Like the chorus, it makes a mono input stereo while
Depth and Mix are both above zero.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `rate` | 0.05–5 | 0.25 | Hz |
| `syncMode` / `syncDivision` | Free / Tempo, 1/1 … 1/32T | Free, 1/4 | — |
| `depth` | 0–5 | 2.0 | ms |
| `delay` | 0.1–5 | 1.0 | ms (the first notch sits at 1 / (2 × delay)) |
| `feedback` | 0.0–0.85 | 0.2 | — |
| `mix` | 0.0–1.0 | 0.5 | — (0.5 gives the deepest notches) |

**Factory presets** set every control, leaving out Division but for Tempo Sweep (one sweep a
bar): Classic Flanger (default), MXR 117 (a slow, wide jet), BF-2, Electric Mistress, Metallic
Comb (a near-static ring), Fast Swirl and Tempo Sweep. Within 2.1 dB of the default on the demo
DI; the high-feedback ones run a little hotter when played softly.

### Tremolo (`tremolo`)
The level moved by an LFO, four ways, chosen by **Mode**. Classic is the tremolo it always was.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mode` | Classic / Harmonic / Pan / Slicer | Classic | — |
| `rate` | 0.1–12 | 4.0 | Hz |
| `syncMode` / `syncDivision` | Free / Tempo, 1/1 … 1/32T | Free, 1/4 | — |
| `depth` | 0.0–1.0 | 0.7 | — |
| `shape` | 0.0–1.0 | 0.0 | — (sine to near-square; a Slicer step's edge) |
| `mix` | 0.0–1.0 | 1.0 | — |
| `pattern` | Pulse, Gallop, Reverse Gallop, Offbeat, Tresillo, Syncopated, Half Time, Build | Pulse | — (Slicer) |
| `crossover` | 200–2000, log | 650 | Hz (Harmonic; advanced) |

- **Harmonic** splits the signal at Crossover (a Butterworth low-pass, the highs being what it
  leaves, so the two always add back exactly) and moves the lows and the highs in opposite
  phase, as the early-60s brown amps did. The level barely moves (3 dB where Classic swings
  60 at full Depth); the tone sweeps instead.
- **Pan** moves the signal between the sides at constant power: centred, both at unity; hard
  over, one at +3 dB and the other silent. It is the one mode that makes a mono input stereo, so
  the tremolo is declared able to widen and the nodes after it run in stereo whatever the mode,
  which lets switching to Pan be heard at once (docs/signal-chain.md, Channel Layout).
- **Slicer** steps through a sixteen-step pattern, one step per LFO cycle, so with Sync on,
  Division is the step length (1/16 for sixteenths). Played steps pass at unity and cut ones
  drop by Depth; Shape softens the edges from 1.5 ms up to a third of the step. A synced step
  may run up to 40 Hz (a 1/32 at 300 bpm); the other modes stay under 12. The pattern starts
  when the effect does: there is no song position to align it to.

**Factory presets** (`ModulationPresets.h`) set every control but Division unless tempo-synced:
Classic Tremolo (default), Slow Throb, Surf (fast and near-square), Brown Harmonic, Deep
Harmonic, Auto-Pan, Tempo Pan (a pan each beat), and three sixteenth-note Slicers: Gallop
Slicer, Half-Time Chop and Tresillo Pulse.

### Rotary (`rotary`)
A rotating speaker cabinet (`RotaryEffect.h`): a horn above, a drum below, two mics.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `speed` | Slow / Fast / Brake | Slow | — |
| `slowRate` | 0.3–2.0 | 0.8 | Hz (the horn; the drum turns at 0.85 of it) |
| `fastRate` | 4–9 | 6.7 | Hz (the drum at 0.88 of it) |
| `ramp` | 0.0–1.0 | 0.5 | — (how long the rotors take to change speed) |
| `drive` | 0.0–1.0 | 0.2 | — (the cabinet's tube amp) |
| `balance` | -1..+1 | 0 | — (Horn/Drum: turns one down, never up) |
| `spread` | 0.0–1.0 | 0.8 | — (Mic Spread: 0 one mic, 1 opposite sides) |
| `depth` | 0.0–1.0 | 0.7 | — (how close the mics are) |
| `level` | -12..+12 | 0 | dB |
| `mix` | 0.0–1.0 | 1.0 | — |

The input is summed to mono, through the amp's soft saturation, and split at 800 Hz by a
Linkwitz-Riley crossover, so horn and drum add back flat. Each rotor reaches each mic along a
path that lengthens and shortens as it turns: a delay swung by the rotor's radius over the speed
of sound (0.15 m for the horn, 0.05 m for the drum's baffle) is the Doppler, and a gain that
follows where it points is the tremolo. The horn also darkens pointing away, and a reflection
off the back of the cabinet fills its troughs. Horn and drum turn in opposite directions.

**Speed never changes at once.** The horn takes about 0.8 s to spin up and 1.2 s to slow down,
the drum 3.5 and 4.5 s (95% of the way, at Ramp's noon; Ramp scales them from a fifth to 1.8
times). That lag, the two rotors drifting apart and back, is the sound of a rotary switching.
A fresh node starts at its speed rather than spinning up from rest, and Brake brings both to a
stop. Map Speed to a footswitch.

At the defaults the horn's pitch swings about 70 cents peak to peak at Fast and 9 at Slow, and
its level 10 dB; the mics hear it at different points of its turn, so a mono input comes out
stereo whenever Spread, Depth and Mix are all above zero, and the effect says so. Level is
calibrated: Drive's saturation and Depth's tremolo each take level away (up to 4.9 and 2.6 dB),
and a makeup that follows both keeps the cabinet within 0.2 dB of bypass on the demo riffs as
Drive, Depth, Spread or Speed move. Horn/Drum only ever turns a rotor down, so it is quieter
away from the middle, the drum carrying most of a guitar. Mix below one combs the dry against
the wet's crossover and delay, a hollow sound, so the presets stay fully wet. About 4 µs per
64-sample block at 48 kHz.

**Factory presets** set every control: Cabinet 122 (default), Chorale (Slow, deep), Tremolo
Rotor (Fast), Bright Horn (the drum down), Dirty Cabinet (the amp driven), Slow Ramp (long
spin-ups between speeds) and Guitar Rotor (lighter, slower and narrower).

### Vibe (`vibe`)
A photocell vibe in the Uni-Vibe style (`VibeEffect.h`): four phase-shift stages swept by a lamp.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mode` | Chorus / Vibrato | Chorus | — |
| `rate` | 0.3–12, log | 2.0 | Hz (Speed) |
| `syncMode` / `syncDivision` | Free / Tempo, 1/1 … 1/32T | Free, 1/4 | — |
| `intensity` | 0.0–1.0 | 0.75 | — |
| `throb` | 0.0–1.0 | 0.5 | — (advanced) |
| `level` | -12..+12 | 0 | dB |

An LFO lights a lamp; four light-dependent resistors facing it each set the corner of one
first-order all-pass stage. Two things make it a vibe and not a phaser:

- **The capacitors are wildly unequal**: 15 nF, 220 nF, 470 pF and 4.7 nF, so each stage sweeps
  its own part of the spectrum (the 220 nF one in the bass, the 470 pF one high up) and the
  notches move unevenly. The cells run from 4 kΩ in full light up as light^-0.7.
- **The lamp and the cells lag.** The filament heats in 6 ms and cools in 18; the cells brighten
  in 3 ms and darken in 10–90 ms (Throb). So the sweep is lopsided: the light rises for about a
  third of each cycle and falls for the rest, the pulse the pedal is known for.

Intensity is how far the lamp swings above its glow; at 0 the stages stand still. **Chorus**
mixes the stages with the dry signal, so the notches move; where they cancel the level drops,
by up to 6.6 dB at full Intensity, and a makeup that follows Intensity puts it back (Speed and
Throb still move it a couple of dB). **Vibrato** is the stages alone: the moving phase is a
pitch wobble, about 16 cents at full Intensity on a 1 kHz tone, at constant level. It moves
both channels together, keeps a mono input mono and runs on one channel in a mono chain. About
1.3 µs per 64-sample block at 48 kHz.

**Factory presets** set every control but Division unless tempo-synced: Classic Vibe (default),
Slow Throb, Fast Chorus, Vibrato, Seasick (slow and deep vibrato), Subtle Shimmer and Tempo
Vibe (a sweep a beat).

### Doubler (`delay_doubler`)
Creates stereo width by mixing a delayed copy of the signal in: added on the left, subtracted
on the right. A mono input comes out stereo whenever Mix is above zero; at a Time of 0 it
passes the input through.

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

A wah: a resonant bandpass swept by a pedal or by the playing level. With **Control** at
Pedal it follows **Pedal Position**; map that parameter to an expression pedal, a MIDI CC or
host automation (right-click the control for MIDI Learn). At Auto Wah it follows how hard you
play, as an envelope filter does. The voicing parameters describe the pedal either way, so a
factory preset is a *model* of a wah rather than a setting of one. The former Auto-Wah effect
runs as this effect's Auto Wah control; see *Retired Auto-Wah* below.

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
| Auto-Engage | For controllers without a toe switch. The wah fades in over 25 ms as soon as the pedal leaves the heel, and fades back to the dry signal once the pedal has rested at or below 4% travel for 400 ms, so a quick heel-toe rock never cuts out. A wah loaded with its pedal parked at the heel starts off. Pedal mode only: the envelope parks the filter at the heel between notes, where this would switch the wah off at every rest. |
| Auto Wah control | A peak follower with Attack and Release time constants reads the louder of the two channels (one detector, as a stereo compressor links its sides, so a stereo image never wobbles between them). Sensitivity is the gain the level is read with, 0 dB at 0 to 40 dB at 1; the default, +10 dB, puts a nominal guitar's accents at the toe and the rest of its playing part way. The result, capped at 1, is the pedal's travel above Pedal Position, which is the rest the filter opens from. The opening sets the filter directly every sample: Attack and Release are the smoothing here, and Response plays no part. |

The trapezoidal (topology-preserving) state-variable filter stays stable and free of
artefacts while its coefficients change every sample, which a direct-form biquad does not.
Centre frequencies are clamped to 0.45 × the sample rate. Latency is zero.

**Parameters**

| Parameter | Range | Default | Unit | Group |
|-----------|-------|---------|------|-------|
| `control` | Pedal / Auto Wah | Pedal | enum | Pedal |
| `position` | 0–1 | 0.5 | heel → toe (the rest position in Auto Wah mode) | Pedal |
| `response` | 1–150 | 12 | ms | Pedal (advanced) |
| `autoEngage` | 0/1 | 0 | toggle | Pedal |
| `sensitivity` | 0–1 | 0.25 | 0–40 dB of detector gain | Auto Envelope (advanced) |
| `attack` | 1–100 | 5 | ms, log taper | Auto Envelope (advanced) |
| `release` | 10–1000 | 80 | ms, log taper | Auto Envelope (advanced) |
| `heelFreq` | 100–1000 | 440 | Hz | Voicing |
| `toeFreq` | 400–5000 | 2000 | Hz | Voicing |
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
with. Read-only feedback: `currentFrequency`, `currentQ`, `engaged`, `envelope` (the detector's
opening, 0–1).

**Factory presets** set every voicing parameter plus `mix`, and deliberately leave out
`control`, `position`, `autoEngage` and the envelope settings (`wah::IsPerformanceParam`).
Those belong to the player's controller and to how the wah is driven, so loading a voicing
never moves the pedal, switches the wah off underfoot, or takes the sweep away from the
envelope. The presets are voiced
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

**Retired Auto-Wah.** The `auto_wah` effect (`kAutoWah`) was folded into this one: its type is an
alias of `wah`, so a stored auto-wah node runs as a wah, and on load `WahLegacyMigration.h` gives
it the wah's parameters on the Auto Wah control. The mapping follows what the old filter did
rather than what its knobs said. Its state-variable filter resonated at half the frequency set,
so Min and Max Freq become Heel and Toe Freq at half their values. Its damping was 1/Q plus the
integrator gain tan(πf/fs), so its Q fell short of the knob (Q 10 gave 8.4 at 300 Hz and 2.3 at
5 kHz) and its bandpass peaked at that Q where the wah's peaks at 2·√Q, so Q, Toe Q Scale, Level
and Toe Gain are set to the Q and peak it had at each end of the sweep, at 48 kHz. It swept
linearly in Hz, so Taper is set for the two sweeps to agree at mid-travel, which keeps them
within about 10% from a quarter open to the toe; nearer the heel the migrated wah sits higher on
a faint signal. Its envelope followed each channel on its own, where the wah's one detector
follows the louder. Its Sensitivity read the level 1 + 9 × Sensitivity times hotter, so the
setting is mapped to the same gain on the wah's 0–40 dB scale (its default 0.6 becomes 0.40). Its
fixed 5 ms attack and 80 ms release are set explicitly, Pedal Position 0
is the rest the envelope opens from, and Low End, Treble and Saturation are off. A node whose
type a loader has already resolved to `wah` is still recognised by the old keys (`minFreq`,
`maxFreq`, `resonance`). `WahLegacyMigrationTests` holds the mapping to the old effect, kept verbatim in
`core/tests/helpers/LegacyAutoWah.h`.

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
| Stereo Spread | Runs the right channel's LFO up to half a cycle ahead of the left's, and its carrier up to a quarter of a cycle ahead, so a mono input comes out stereo at about the same level on each side. When Spread returns to zero, the right carrier is phase-locked back onto the left over about a second, as a brief detune of at most 10 Hz rather than a jump. Since Spread can widen, a ring modulator always runs both channels and the nodes after it run in stereo. |

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

### Dispersal (`dispersal`)

A long chain of second-order all-passes, all centred on one frequency, which holds that band
of the spectrum back behind the rest. Nothing is boosted or cut: the magnitude response is
flat and only timing changes. A pick attack holds every frequency at once, so it comes out as
a sweep: a "pew", a laser zap, a dub drop. A steady note only changes phase and sounds nearly
dry. Mix below 1 combs the chain against the dry signal, a very deep phaser.
`DispersalEffect.h` has the processing, `DispersalSupport.h` the parameter table, the stage
design and the presets. The plan, with what is still to come (an LFO and an envelope on
Frequency, a group-delay plot), is docs/plans/dispersal.md.

```
in ─┬─ AP ─ AP ─ … ─ AP ─┬─ tap at Stages (crossfaded on a change) ─ mix ─ out
    │  ←── Stages ──→    │                                          │
    └────────────────────┴─────────────────── dry ──────────────────┘
```

| Behaviour | How |
|---|---|
| Stages | Trapezoidal state-variable filters, all-pass output x − 2k·band, which stay bounded while their coefficients move. One stage delays its centre by 2Q(1 + g²)/(g·fs), about 2Q/(π·f₀) well below Nyquist; at Focus 1 it delays DC by half that, so a low Focus holds the lows back too and a high one only a narrow band. The chain delays by Stages times one stage. `dispersal::StageGroupDelaySeconds` gives the delay at any frequency; tone bursts through the effect arrive within 0.05 ms of it at the centre, two octaves above and two below. |
| Delay at the defaults | 64 stages at 800 Hz and Focus 1 hold 800 Hz back 51 ms, 200 Hz 29 ms and 3.2 kHz 1.8 ms. `GetParam("peakDelayMs")` answers the delay at the centre for the current settings. Long settings ring for a long time: 256 stages at 100 Hz and Focus 6 hold the centre back about 10 s. |
| All-pass | A noise burst comes out with its energy (within 0.0001 dB, 1–256 stages) and a steady sine at its level, 50 Hz to 12 kHz. |
| Stages changes | Whole stages, crossfaded over 30 ms from the old count's output to the new one's while the chain runs to the larger count. Half-way between counts one stage apart there is a notch at the centre, and growing the count leaves a gap at the centre until the signal arrives through the new stages, so Stages is a control to set rather than sweep: automating it steps. Stages past the count are not run, and are cleared when they drop out. A sine through 64, 256, 32 and 1 stages never steps by more than 1.25 times its steady slope. |
| Smoothing | Frequency and Focus glide in the log domain over 20 ms, Stereo in semitones over 20 ms, worked out every 16 samples with the coefficients ramped in between; Mix glides per sample over 20 ms. Values set before the first block apply from sample 0. Sweeping Frequency end to end twice a second at 256 stages and Focus 6 gains no level (0.2 dB over 2 s of noise). |
| Stereo | Moves the right side's centre up to an octave either way, so the zap reaches each side at a different moment. The sides share settings and nothing else (`KeepsChannelsSeparate`). With Stereo at 0 and a mono input the two chains converge, and once their states agree within 1e-7 only the left is run and copied to the right. |
| Safety | A non-finite stage state clears that side's chain; the centre is held below 0.45 × the sample rate; state under 1e-12 is flushed at the end of each 64-sample chunk. No latency. |
| Cost | Release, per 64-sample stereo block at 48 kHz: 18 µs at 64 stages with a mono input, 21 µs with a stereo one (1.3–1.6% of the 1333 µs deadline); 71 µs at 256 stages either way (5.3%). Each stage's recursion is serial, so one chain is latency-bound and a second runs in its shadow: linking the sides saves little. |

**Factory presets**

| Preset | Stages | Frequency | Focus | Other | Delay at the centre |
|---|---|---|---|---|---|
| Zap (default) | 64 | 800 Hz | 1.0 | | 51 ms |
| Laser | 64 | 1.2 kHz | 2.0 | | 68 ms, narrower |
| Dub Drop | 96 | 250 Hz | 2.0 | | 0.49 s |
| Soft Attack | 24 | 2 kHz | 0.5 | | 2 ms: rounds the pick attack without a zap |
| Wide Zap | 96 | 900 Hz | 1.5 | Stereo +7 | 102 ms left, 68 ms right |
| Comb Phase | 8 | 600 Hz | 0.7 | Mix 0.5 | 8 notches against the dry |

### Synth Voice (`synth_saw`)
Tracks the pitch of a single note and plays it on two oscillator voices, shaped by an envelope
follower on the input.

| Parameter | Range | Default | Notes |
|---|---|---|---|
| `mix` | 0–1 | 1.0 | Dry guitar to synth |
| `attack` / `release` | 0.1–100 / 10–1000 ms | 5 / 100 | The envelope follower |
| `detune` | ±100 cents | 0 | Both voices |
| `octaveShift` | -2..+2 | 0 | Both voices |
| `glide` | 0–500 ms | 10 | |
| `outputGain` | -24..+12 dB | 0 | |
| `gate` | -80..0 dB | -60 | Silent below it |
| `waveShape`, `pulseWidth` | Saw / Square / Triangle / Sine, 0.1–0.9 | Saw, 0.5 | Voice 1. The Square is zero-mean at any width, from -2 × width to 2 - 2 × width |
| `voice2Semitones`, `voice2Mix` | ±24 st, 0–1 | 0, 0 | Voice 2's interval and share |
| `voice2WaveShape`, `voice2PulseWidth` | as voice 1 | Saw, 0.5 | |

**Factory presets** (`PitchPresets.h`) leave out Output, the player's level, and Gate, which is
set against their own noise floor. Classic Saw (default), Synth Bass (square an octave down),
Square Lead (with a triangle an octave up, and glide), Fifth Stack, Sine Flute, Saw Pad (slow
attack, long release) and Sub Octave Blend (the guitar with a square an octave below). Within
about 1 dB of the default on the demo DI. Every pulse width stays at 0.5. Any other width used to
add DC, a thump on every note; the Square is zero-mean now, though a narrower pulse measures
quieter (1.9 dB at 0.2).

### Auto Arpeggiator (`arp_auto`)
Pitch-shifts what you play through a pattern of intervals, one step per beat division at the
song's tempo.

| Parameter | Range | Default | Notes |
|---|---|---|---|
| `stepRate` | 1/4 … 1/32T | 1/8 | |
| `pattern` | Major Triad, Minor Triad, Power Chord, Octaves, Custom, Random | Major Triad | |
| `direction` | Up, Down, Up-Down | Up | |
| `numSteps` | 2–8 | 4 | Custom and Random only |
| `gate` / `attack` / `release` | fractions of a step | 0.8 / 0.05 / 0.08 | |
| `step0`…`step7` | ±24 st | 0, 4, 7, 12, 0… | Custom's intervals (advanced) |
| `pitchMode` / `pitchThreshold` | Always, Above, Below / 50–2000 Hz | Always / 330 | When it plays |
| `mix` | 0–1 | 0.8 | |

Steps at 0 st play the input itself. The others go through `TimeDomainPitchShifter`, a time-domain
shifter (Pitch Shift's Low Latency engine until it moved to `SpliceTransposer`). Everything runs
per sample, so a step starts on its own sample whatever the block size. From one shifted step to the next the shifter only changes speed,
so the new pitch is heard on the step's first sample, without a click; a 0 st step and a shifted
one cross-fade over 5 ms. The shifter's history is written on every sample, so a shifted step
after a 0 st one plays what is coming in now. A shifted step runs 2–25 ms behind its input, and
the node reports the nominal 10 ms. The dry mix is not delayed.

Until 2026-10 the arp used Signalsmith Stretch. Stretch was not fed during 0 st steps, so the next
shifted step replayed the audio from before them. After a chord change that was the old chord,
at about full level. Measured at 48 kHz in 64-sample blocks against Stretch with that fixed:

| | Signalsmith Stretch | Time-domain shifter |
|---|---|---|
| New pitch after a shifted step | 40–51 ms | 0.1 ms |
| CPU per block: mean / p99 / max | 11 / 217 / 396 µs | 2.7 / 20 / 40 µs |
| Pitch error on held notes, E2–E4 shifted −12 to +12 st | up to 30 cents | within 1.5 cents |
| Latency | 80 ms | 2–25 ms (10 nominal) |

The gate is judged in double precision. In float, a phase just under the end of a step rounded to
1.0 and read as past a 100% gate, which dropped the last sample of every step to silence.
`AutoArpEffectTests` covers all of this.

`numSteps` is an enum from 2, so both UIs count its labels from the parameter's minimum
(`enumLabel` in `paramLabels.ts`, `EnumLabelIndex` in `uiclient/ParamFormat.h`). Indexed by the
value, 4 showed "6", and the JUCE Nano's menu set 2 for any of "2", "3" or "4".

**Factory presets** (`PitchPresets.h`) leave out Pitch Trigger and its Pitch, which decide when it
plays, and set all eight custom steps, mirroring a built-in pattern where they use one. Major Arp
(default), Minor Up-Down (1/8 triplets), Power Fifths, Octave Bounce (-12, 0, +12, 0), Sus4 Arp,
Minor 7th (eight steps, up and back), Fifths Ladder and Octave Swells (quarter notes over the dry
note). Their rates go no faster than 1/8 triplets. They were chosen when a new pitch arrived about
35 ms into its step; it now arrives on the step's first sample.

### Pitch tracking in Synth Voice, the Auto Arpeggiator and the tuner

The same `dsp/PitchTracker.h` (see *Pitch tracker* under Ring Modulator) follows the note for
three more features, each of which used to run a brute-force YIN of its own. The Pitch Tracking
EQ (`eq_pitch_tracking`) uses it too, to place its bands.

| Feature | How it uses the tracker |
|---|---|
| Synth Voice (`synth_saw`) | The mono input feeds the tracker, and its accepted pitch is what the oscillator glides to over Glide. After an onset (the input envelope rising by 15% in a sample) the glide runs four times faster until two confident detections, about 10 ms. While the tracker finds no pitch (silence, noise, a note dying away) the oscillator goes, over Glide, to the pitch the tracker holds: the note's own from before it began to stop. It used to keep the last estimate it was given, which may already have heard the start of the silence, and a low note's tail could settle over 30 cents out (13 cents at the default Glide); wherever the stop falls it now settles within 1.5 cents. It sounds only while the envelope is above Gate. Notes from 45 Hz to 2 kHz. At 48 kHz in 64-sample blocks of the DI demo, built for Release, it costs 2.2 µs per block on average and 8.3 µs at the 99th percentile, 1.7 and 7.7 µs of that the tracker's; with voice 2 on, 2.4 and 8.4 µs, where working out the frequency ratios with `std::pow` on every sample made it 3.7 and 10. Built without fast math (`/fp:fast`), the tracker's sums go unvectorised and everything costs about five times as much. A note change reaches the oscillator in 13–60 ms. The old detector cost 50 and 130 µs (2.2 ms at 192 kHz, against that block's 333 µs deadline), took up to 83 ms, and stopped at 50 Hz. |
| Auto Arpeggiator pitch trigger (`arp_auto`) | The left input feeds the tracker. Every 2048 samples at 48 kHz (43 ms at any rate) the frame's pitch is smoothed (0.6 of the new, 0.4 of the old; ×0.8 for a frame without one) and compared with Pitch: two frames past it turn the arp on, restarting the pattern on the beat, and five short of it turn it off. A frame's pitch comes from its latest detection that found one, and none when the frame is quieter than RMS 0.003. The old detector took the first period under its threshold rather than the bottom of the dip, in whole samples, so it read up to 190 cents sharp and a note a semitone below the threshold tripped it. It found no pitch at all at 96 kHz and above, and cost a burst of about 340 µs in one block of every 32. On the DI demo the on/off state matches the old one in 98–99.9% of blocks in Above mode. In Below mode the arp now drops out in long gaps between notes, as a full-rate YIN reference does, where the old one mostly stayed on. |
| Tuner (`TunerEngine`) | Runs the tracker on the audio thread (about 2.4 µs per 64-sample block) and averages its detections over each reading, 2048 samples at 48 kHz (43 ms at any rate). A worker thread names the note against the reference pitch and reports it, as before. A new note restarts the average. Averaged, the 5 ms detections read a held note with a third to a tenth of the scatter of the single 85 ms window the tuner used to analyse. So the tuner needs no longer window of its own, and the UI still averages six readings. With white noise 20 dB below an A2, readings scatter by 0.3 cents, where they scattered by 2.6 and sat 1.7 cents sharp. On the DI demo's held notes, 99% of readings are within 10 cents of full-rate YIN, against 75% before, and none is more than 50 cents out, against 24% before. Notes above about 1.1 kHz no longer read an octave low, and 176.4 and 192 kHz work. The worker no longer spends 2.3 ms on YIN every 43 ms. The tuner takes the tracker's range down to 16 Hz (`kBassLowestHz`, a semitone under an extended-range bass's low C#), where the effects stop at 45 Hz. It sets it before `Prepare()`, which sizes the buffers for it; a tracker prepared at the default keeps the guitar's buffers and goes no lower, so the effects' buffers, window, cost and output are exactly as they were. A note under 45 Hz showed nothing in 1.6.0. The old detector stopped at 50 Hz and fell back to its best lag, which read a low E or B as its octave on a minority of readings, and only when the fundamental was much weaker than the second harmonic; otherwise nothing. The longer window (two of the longest period, 125 ms against 44) reads held notes the same, from C0 up and through a 20 Hz interface roll-off, with the first reading about 40 ms later on the lowest notes; it costs 7–9 µs per block instead of 1.7 while the tuner is on. It follows fast playing worse (on the first demo riff, 32 of 46 readings right against all 46 with the 45 Hz window), which a tuner does not need; the effects keep the 45 Hz window for that. |

### Guitar to MIDI (`guitar_to_midi`)
Turns single notes into MIDI for a virtual instrument in a Plugin Host downstream of it in the
same chain. It makes no sound of its own: Guitar Thru passes the guitar on or mutes it. Nano Q
does not offer it, since it cannot add the Plugin Host it needs.

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

**On real playing** (the demo clips in `core/ui/demo`, scored against each pick's note read
offline, 2026-10-07): the C#3 to F#3 notes of Guitar Riff 03 start 30 ms after the pick at the
median, most within 17–36 ms and a few at 50–65 ms; a hard-picked one can read sharp and start a
semitone high for 25 ms in Notes mode, which Notes + Bend plays as a bend. The low riffs of
Guitar Riff 01 and 02 (Eb2 to C3, picked hard) start about 55 ms after the pick at the median and
86 ms at worst, every note found. That is the pitch tracker's limit, not the note logic's: for
the first 50 ms after those picks no lag dips under YIN's 0.2 threshold (the minimum sits at
0.3–0.6), and the note starts one detection, 5 ms, after the tracker first reads it. Until then
the previous note carries on. `OnsetPitchEstimator`, which the harmonizer uses, finds no more of
these notes early. Power chords, most of Guitar Riff 03, have no single period in the tracker's
range: a note often stops 100 ms into the chord and returns as its root a few hundred
milliseconds later, if at all. `NoteTrackerTests` holds the single-note riffs to this.

**Routing**: the notes reach every Plugin Host downstream, however many nodes lie between, and
none on a parallel branch that does not pass through this node; they stop at a composite's
edge. Several Guitar to MIDI nodes can feed one instrument, each on its own channel. The Plugin
Host's note player (`NotePlayer` in `dsp/NoteEvents.h`) reconciles what it has sent with the
note each source holds at the end of every block, so nothing hangs when a source is bypassed or
removed, a block is dropped while the plugin is busy, or the plugin is reset or replaced; a
source brought back from bypass starts afresh rather than replaying the note it had.

It is monophonic: a chord comes out as one of its notes, not always the same one, and often late
or not at all (above). MIDI goes only to hosted plugins: it is not sent out of the app or to a
DAW.

### Pitch Shift (`pitch_shift`)
Pitch shift, free or snapped to whole semitones, within a range an expression pedal sweeps, on one of two engines.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -12..+12 | 0.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |
| `engine` (Engine) | 0 High Quality / 1 Low Latency | 0 | enum |
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

**Engines.** The two trade sound against how quickly a shift is heard:

| Engine | Latency while shifting | A new shift is heard after | Character |
|--------|------------------------|----------------------------|-----------|
| High Quality (default) | 80 ms | ~40 ms | Signalsmith Stretch, a phase vocoder: smooth, but smears attacks and low notes |
| Low Latency | 16 ms reported; picks 3-6 ms late shifting down | under 3 ms | Time domain: low notes in tune, chords clean, picks on time |

High Quality is Signalsmith Stretch (`SignalsmithSupport.h`). It takes a new shift at its next
analysis frame and fades it in over its synthesis window, so the delay before a pedal move is
heard is its output latency, half the 80 ms total. Shortening the analysis interval does not
change that (only its jitter), and every smaller or asymmetric window measured traded tone
for it. It stays the default so presets saved before the switch sound as they did.

Low Latency is `SpliceTransposer` (`core/src/dsp/`), the engine Transpose and the global transpose
run (`docs/transpose-engine.md`). A tap reads a delay line at the pitch ratio, so a new shift
changes the pitch on the next sample, and the tap jumps when it drifts to the end of its 30 ms
window. Each jump lands where the waveform matches, scored as mismatch per sample of run and
refined to a fraction of a sample, so on a single note it joins whole periods; a poor match, such
as a chord's, fades for longer; and a pick moves the tap to the newest audio. Pitch is within a
cent from -12 to +12 st, low bass included, and it costs 1.6-3.4 µs per 64-sample block. With Snap
off it follows the target with a 4 ms glide, which smooths a 7-bit controller's steps; with Snap on
each semitone lands at once. The latency it reports is a fixed 16 ms whatever the shift, so a
sweep does not keep changing the host's delay compensation. Until 2026-10 Low Latency was
`TimeDomainPitchShifter` (10 ms reported, a flutter on chords and low notes out of tune); presets
saved on it now report 6 ms more latency and sound cleaner.

**Path changes crossfade.** Entering and leaving the 0 st bypass, and switching engine, fade
over 10 ms, with both engines running until the fade ends. The engines' latencies differ from the
bypass's, so a hard switch used to jump the audio in time (80 ms on High Quality) with a click;
now it is a short blend. Both engines record their input history on every sample, so either
starts on current audio. Tests: `core/tests/PitchShiftEngineTests.cpp`.

**Factory presets** (`PitchPresets.h`) set everything, Semitones included: with no pedal mapped it is
the interval itself, so a pedal preset sets it to the end the pedal rests at, inside its own
Range. Full Range (default), Octave Down, Octave Up Blend (High Quality, for chords, at 40%),
Fifth Harmony (at 50%), Whammy Up (heel dry, toe an octave up, gliding), Dive Bomb (rests at the
toe, rock back to dive) and Step Whammy (in semitones). Everything a pedal moves uses Low Latency.
The two blends sit 3 dB under the default, as Mix is a linear crossfade.

### Harmonizer (`harmonizer`)
Up to four pitch-shifted voices played alongside the guitar: intelligent harmony in a key and
scale, or fixed intervals that work on chords. `core/src/dsp/effects/HarmonizerEffect.h`, with
its parameter table and presets in `HarmonizerSupport.h`.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `mode` (Mode) | 0 Scale / 1 Fixed | 0 | enum |
| `key` (Key) | 0 C .. 11 B | 0 (C) | enum |
| `scale` (Scale) | Major, Minor, Harmonic Minor, Melodic Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Phrygian Dominant | 0 (Major) | enum |
| `tracking` (Tracking) | 0 Clean / 1 Fast | 0 | enum |
| `glide` (Glide, advanced) | 0–500 | 0 | ms |
| `lowestNote` (Lowest Note, advanced) | E2 / D2 / B1 / F#1 | 1 (D2) | enum |
| `voiceNOn` (N = 1–4) | 0/1 toggle | voice 1 on | — |
| `voiceNInterval` (Interval) | -14..+14 scale steps (2 Octaves Down .. 2 Octaves Up) | 3rd Up, 5th Up, Octave Down, Octave Up | enum |
| `voiceNSemitones` (Semitones) | -24..+24 | +4, +7, -12, +12 | st |
| `voiceNLevel` (Level) | -40..+6 | 0 | dB |
| `voiceNPan` (Pan) | -1..+1 | 0 | — |
| `voiceNDetune` (Detune, advanced) | -50..+50 | 0 | cents |
| `voiceNDelay` (Delay, advanced) | 0–100 | 0 | ms |
| `dry` (Dry) | 0.0–1.0 | 1.0 | — |
| `harmonyLevel` (Harmony) | -24..+6 | 0 | dB |
| `mix` (Mix) | 0.0 guitar only – 1.0 guitar and harmony in equal parts; scales the harmony only | 1.0 | — |
| `highCut` (High Cut, log taper) | 1000–20000; 19.5 kHz and up is off | 20000 | Hz |
| `humanize` (Humanize, advanced) | 0.0–1.0 | 0.0 | — |

Each voice is its own `SpliceTransposer`, the Low Latency engine of Pitch Shift and Transpose
(`docs/transpose-engine.md`): a new interval is heard on the next sample, bass notes stay in
tune and chords shift cleanly. Its window is 40 ms rather than Transpose's 30, so a Clean voice
can start from a pick that far back, and the voices play about 21 ms behind the guitar on
average, as a second player would. The dry signal is never delayed and the effect reports no latency, so a harmony
never makes the guitar itself late or moves the host's delay compensation.

**Modes.**
- **Scale**: each voice is a number of scale steps from the note being played, in Key and Scale.
  `core/src/dsp/MusicalScale.h` does the arithmetic: a 3rd up is two steps, so in C major it is a
  major third above C, F and G and a minor third above D, E, A and B. A note outside the scale
  moves as the scale note nearest it (the lower one on a tie), so a chromatic run is harmonised
  in parallel rather than stalling. Only seven-note scales are offered, since a 3rd or a 6th is
  defined by counting a seven-note scale's notes. The note comes from `ScaleNoteFollower`
  (below). Scale mode follows single-note lines. A chord or a palm-muted chug often has no pitch
  to find, and keeps the interval the last note had. Until a first note has been found the voices
  are silent.
- **Fixed**: each voice moves by its own Semitones whatever is played, chords included. Interval
  is ignored.

**Following the notes** (`core/src/dsp/ScaleNoteFollower.h`). The first version used
`NoteTracker`, and on real playing it sounded discordant and warbling. Its pitch comes from
`PitchTracker`'s fixed 28 ms window, which after a pick still holds the note before. On the demo
riffs a new note was confirmed 50-75 ms after its pick, and 8 of 18 picks were never confirmed.
Clean then waited out a 50 ms timeout and came back without the pick, a stutter several times a
second, and Fast played most of each note at the old interval. Notes 35-60 cents sharp (low
strings after a hard pick) were rounded to the wrong semitone and flipped. The follower now:
- finds picks with `PickAttackDetector`, as each voice's SpliceTransposer does;
- after each pick, estimates the new note from the audio since the pick alone
  (`core/src/dsp/OnsetPitchEstimator.h`, YIN over what has arrived, a period found once two have):
  confirmed by two estimates in a row on the same scale note, in 10-13 ms from E4 up, 18 ms on an
  E3 and 30 ms on a low E2. Until an estimate has searched far enough to rule out a lower
  fundamental, it only counts within 19 semitones of the note before: right after a low note's
  pick a strong fifth harmonic reads two octaves and more too high;
- between picks, follows legato notes, slides and bends with `PitchTracker`, once its window lies
  wholly after the pick, on two readings in a row;
- snaps every pitch to the nearest note of the scale (`music::NearestScaleNote`), with 0.3
  semitones of hysteresis (`music::FollowScaleNote`), not to the nearest semitone. Scale notes are
  one or two semitones apart, so a sharp note is still clearly nearest its own.

**Tracking** (Scale mode). Until a pick's note is known, a voice would play the new note at the
old note's interval, a short wrong harmony at every note that changes it.
- **Clean** (default) ducks the voices under each pick (a 2 ms fade) and, once the note is known,
  starts them again from 3 ms before the pick (`SpliceTransposer::EngageAt`). The harmony enters
  with its own pick, at the level Fast plays it, 10-30 ms behind the guitar's (lower notes later),
  and never at a wrong interval. A pick whose note is not found within 35 ms (a strum, a muted
  chug) starts the voices from the pick at the intervals they had, so the harmony is never more
  than that late and never cut short.
- **Fast** never ducks. The voices play the pick at the old interval and move when the note is
  known.

On the demo riffs (low, palm-muted single notes and power chords, which few picks of have a pitch
to find), in their own keys, voice 1 a 3rd up: Clean plays a wrong interval 3.5% of the time on
the single-note riffs, and is late 35 ms on most picks; Fast is on time and wrong 7% of the time.
Power chords are the hard case for either: about 20%.

In both, a legato note (a hammer-on or slide, with no pick) moves the voices when it is found. A
bend moves the harmony once it is nearer the next scale note than its own by 0.3 semitones, so
vibrato does not flicker between two harmony notes. Glide
slides a voice to its new interval rather than jumping, which suits slow legato lines. Key and
Scale can be changed while playing, or mapped to MIDI for a song's key changes; the voices then
glide to their new intervals.

**Per voice.** Level and Pan (the voice's mid at equal power, centre at unity on both sides; any
side the input already had narrows as it moves off centre). Detune adds cents to the interval,
and Delay plays the voice up to 100 ms late. A unison voice detuned a few cents and 20-30 ms late
is a double-tracked part. A voice can be panned, so the harmonizer is declared able to widen and
a mono rig carries the spread on downstream (docs/signal-chain.md, Channel Layout). Humanize gives every
voice a slow random drift of its own, up to 8 cents in pitch and 3 ms in timing at 1, the timing
rate-limited so it never bends the pitch by more than 7 cents. High Cut is a 12 dB/octave
low-pass on the harmony only, to soften the voices against the guitar.

Every voice's input history is written on every sample, on or off, so a voice that is switched
on plays what is coming in now; switching, ducking and the first note fade over 5 ms. SetParam
only stores the value, and Process takes it up at the start of the next block, so it is safe on
the audio thread (no allocation, no locks). The output is bit-identical in any block size.
CPU at 48 kHz in 64-sample blocks, Release, on a picked line from E2 up: one voice 8 µs mean and
30 µs p99, four voices 20 and 52 µs (the same in Fixed mode; Humanize adds about 2 µs), against a
1333 µs deadline. Each voice is a whole SpliceTransposer with its own copy of the input history,
written on every sample; one history shared by the four would save three of those writes.

**Factory presets** (`HarmonizerSupport.h`) leave Key alone, as it is the song's, and Lowest Note,
the guitar's. 3rd Up (default), Twin Leads (a 3rd up in minor, right of centre and 12 ms late),
Thirds and Fifths, Neo-Classical (harmonic minor), Country Sixths (a 6th below, gliding), Choir
(four voices, humanised and darkened), and, in Fixed mode, Octave Stack, Power Fifths and Double
Tracked (two detuned, delayed unison voices panned wide). Tests: `core/tests/HarmonizerEffectTests.cpp`.

### Transpose (`transpose`)
Shifts the whole input by whole semitones, for playing in another tuning. Two engines:

- **High Quality** (0, the default, so presets saved before the second engine sound as they did):
  Signalsmith Stretch, 80 ms of latency.
- **Low Latency** (1): `SpliceTransposer`, a time-domain shifter that moves its read tap to every
  pick attack, so attacks arrive 3-6 ms late shifting down, and that reports 16 ms. It holds a
  low E1 in tune, and on the bench it scores better than High Quality on every measure
  (`docs/transpose-engine.md`). The global transpose always runs this one.

At 0 st the node passes its input through and reports no latency. Entering or leaving 0 st, and
switching engine, crossfade over 10 ms, and both engines keep recording their input while idle, so
a shift never starts on stale audio.

| Parameter | Range | Default | Unit |
|-----------|-------|---------|------|
| `semitones` | -36..+12 | 0.0 | st |
| `mix` | 0.0–1.0 | 1.0 | — |
| `engine` | 0/1 (`High Quality`/`Low Latency`) | 0.0 | enum |

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
Creates parallel paths by inserting a splitter and an auto-join mixer. **Mode** sets what each
branch gets. Branches are numbered as the chain draws them, top to bottom. Engine:
`core/src/dsp/effects/SplitterEffect.h` (+ `SplitterSupport.h`).

| Parameter | Range | Default | Unit | Shown in |
|-----------|-------|---------|------|----------|
| `mode` | Copy, Blend, Switch, Dynamics, Pitch, Stereo | Copy | enum | always |
| `position` | 0..1 | 0 | amount | Blend |
| `curve` | Equal power, Linear | Equal power | enum | Blend |
| `branch` | Branch 1..4 | Branch 1 | enum | Switch |
| `detect` | Pick, Level | Pick | enum | Dynamics |
| `threshold` | -60..0 | -18 | dB | Dynamics |
| `width` | 0..24 | 6 | dB | Dynamics |
| `release` | 10..2000 (log) | 300 | ms | Dynamics |
| `splitNote1`..`splitNote3` | E1..E6 (index 0..60) | C4, C5, C6 | enum | Pitch |
| `lowestNote` | E2, D2, B1, F#1 | E2 | enum (advanced) | Pitch |
| `unpitched` | All branches, Last branch, Branch 1..4 | All branches | enum | Pitch |
| `crossfade` | 1..200 (log) | 10 | ms | Blend, Switch, Dynamics, Pitch |

**Modes**:
- **Copy**: every branch gets the whole signal, as a split always did. The branches read the
  splitter's own buffer, so a Copy split runs exactly as it did before modes existed.
- **Blend**: Position moves the signal from branch 1 (0) to the last branch (1), crossfading each
  pair of neighbours. It's built for an expression pedal. Equal power keeps the loudness steady
  between rigs that sound different; Linear suits branches that sound alike.
- **Switch**: only the chosen branch plays, like an A/B/Y box. Every branch keeps running, so a
  delay or reverb on the branch you left rings out.
- **Dynamics**: soft playing goes to branch 1, hard to branch 2, crossfaded across Width (dB)
  centred on Threshold. **Pick** judges each note by its pick's peak (`PickAttackDetector`) and
  holds that until the next pick, so a hard note stays on branch 2 while it rings. For the first
  5 ms of a pick the previous note's branch holds unless the new pick is already over the
  threshold. **Level** follows a peak envelope with Release, so a note moves back to branch 1 as it
  decays, and so does turning the guitar's volume down. Branches after the second get
  everything.
- **Pitch**: notes go to branches by range. Each split note is the lowest note of the branch
  above it, and a split uses one fewer split note than it has branches (the panel hides the
  rest). `NoteTracker` decides once per note: a bend or vibrato across a split note never moves
  the note, but a legato move to a new note does. A pick's pitch is known about 55 ms after it
  (84 ms worst, on the demo riffs). Until then, and for chords, which a mono tracker can't
  pitch, the signal goes where **Unpitched** says: every branch at equal power (-3 dB each with
  two), the branch the last note went to, or a fixed branch. Put the split before any drive:
  distortion makes tracking worse.
- **Stereo**: the left channel goes to branch 1 and the right to branch 2, each mono on both
  sides; later branches get both channels as they came. The layout gives those two branches a
  mono connection, so they run mono, and a mode change moves the layout at the next block
  (docs/signal-chain.md). With a mono input, both branches get the same signal.

**Notes**:
- The `splitter` effect is user-addable.
- A split has 2 to 4 branches (one per mixer input). It starts with two; the splitter's **Branches** control adds empty ones or removes empty ones (`setSplitBranchCount`). The count is the splitter's outgoing edges, so there is no parameter to keep in step with the graph.
- Every change of branch ramps each branch's power over Crossfade, so a switch between two rigs neither dips nor bumps. A preset that loads in a routing mode starts on its branches without a ramp.
- Detection runs on the mid of the two sides, and only in the mode that needs it. In dual mono, both sides are routed by that one decision.
- A bypassed splitter copies. The editors don't offer bypass for a splitter; only automation can bypass one.
- Under Branches, the panel says what the branches get in the current mode (`describeSplitRouting` in `splitBranches.ts`, `uiclient/SplitRouting.h` for Nano Q).
- The `mixer` node is inserted automatically to rejoin branches and is not shown in the FX list.

### Signal Analyzer (`input_analyzer`)
Pass-through utility node that renders live diagnostics for the signal entering the node.

| Output | Description |
|--------|-------------|
| dBFS | Live peak/RMS in dBFS |
| Converted units | Peak/RMS as %FS plus RMS dBu/dBV/Vrms |
| LUFS | Live momentary, short-term, and integrated loudness per BS.1770-4 K-weighting |
| Channels | The layout the engine runs the node in, Mono or Stereo, which is configuration (docs/signal-chain.md, Channel Layout). On a stereo layout the measurement adds a hint: "one side silent" when only one channel carries signal, "sides identical" when both carry the same, so a player can switch the input to mono |
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
9. Give it an icon (and, if it wants one, stock artwork) under `effects` in
   `core/ui/data/effect-presentation.json`, keyed by its `EffectGuids` constant with its guid,
   then run `node tools/gen-effect-presentation.mjs`. Both UIs read that file; without an entry
   the effect shows its category's icon. See [user-interface.md](user-interface.md#effect-presentation-coreuidataeffect-presentationjson).

Framework-specific effects, such as the JUCE plugin host, may live in the adapter layer instead. They should still use a stable UUID from `EffectGuids.h` and register with `EffectRegistry` before presets or the effect catalog are loaded.

> **Renaming an existing effect?** The UUID stays the same — just update `info.displayName`. Add the old alias string to `info.aliases` if it was previously used in preset JSON.

## See Also
- [Signal Chain](signal-chain.md) — How effects execute in the graph
- [Data Models](data-models.md) — ResourceRef and preset schema
- [User Interface](user-interface.md) — Effect browser UI
- [Composite Effects](composite-effects.md) — Bundling effects into reusable composites
