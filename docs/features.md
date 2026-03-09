# Implemented Features

This document is the authoritative reference for every implemented feature in Soundshed Guitar, grouped by functional system. It is intended for agents and developers who need to understand what exists before making changes.

---

## 1. Signal Chain Engine

**Key files:** `core/src/dsp/SignalGraphExecutor.h/.cpp`, `core/src/presets/PresetTypes.h`

The core DSP engine routes audio through a directed acyclic graph (DAG) of effect nodes rather than a fixed linear chain.

- **Graph model**: `SignalGraph` holds `GraphNode[]` (processing units) and `GraphEdge[]` (connections).
- **Node fields**: `id`, `type`, `enabled` (bypass), `params` (float map), `config` (string map), `resource` (ResourceRef).
- **Edge fields**: `from`, `to`, `fromPort`, `toPort`, `gain` (edge-level gain multiplier).
- **Special node types**: `input`, `output`, `splitter` (fan-out), `mixer` (weighted sum of N inputs).
- **Execution order**: Kahn's topological sort; acyclicity is validated at load time.
- **Bypass semantics**: Disabled nodes pass audio through without processing; the graph stays connected.
- **Buffer management**: Per-node stereo buffers allocated at `Prepare()` — zero heap allocation in audio callback.
- **Implicit I/O**: If edges reference `__input__` or `__output__` but nodes are absent, the executor inserts them.
- **Parallel paths**: Created by inserting a `splitter` node; a `mixer` node is auto-inserted to rejoin branches. Example use: dual-cab routing or wet/dry parallel delay.
- **Global processing loop**:
  1. Input trim + mono/auto-level
  2. Global pre-chain (shared across all presets)
  3. Per-preset signal graphs (mixed in parallel)
  4. Global post-chain (shared EQ, doubler)
  5. Master gain + output limiter

**Global pre/post chains** (`GlobalSignalChainConfig`): Independent `SignalGraph` instances that wrap around all presets. Defaults: pre = gate; post = EQ + doubler. Edited via the signal path UI.

**Global parameters** (outside the graph):

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `inputTrim` | -40..+20 dB | 0.0 | Gain before graph |
| `outputTrim` | -40..+20 dB | 0.0 | Gain after graph |

---

## 2. Effect Types (FX Library)

**Key files:** `core/src/dsp/EffectRegistry.h/.cpp`, `core/src/dsp/effects/`

All effects register via `EffectRegistry` (singleton factory). Effect type IDs, parameter definitions, and factory callbacks are declared at startup via `RegisterAllEffects()` in `BuiltinEffects.h`. The UI queries the registry for the effect catalog dynamically.

### 2.1 Amplifier & Cabinet

| Type ID | Name | Resource | Key Parameters |
|---------|------|----------|----------------|
| `amp_nam` | NAM Amp | `.nam` model file | `inputGain` (-24..+24 dB), `outputGain` (-24..+24 dB) |
| `cab_ir` | IR Cabinet | `.wav` IR file | `mix` (0–1), `outputGain` (-24..+24 dB), `quality` (0–3: Economy/Standard/High/Full) |
| `cab_simple` | Simple Cabinet | none | `bass`, `presence`, `brightness`, `mix` (all 0–1) |

### 2.2 Dynamics

| Type ID | Name | Key Parameters |
|---------|------|----------------|
| `dynamics_gate` | Noise Gate | `thresholdDb` (-80..0), `attackMs` (0.1–50), `releaseMs` (1–500) |
| `compressor_vca` | VCA Compressor | `threshold` (-60..0 dB), `ratio` (1–20:1), `attack` (0.1–500 ms), `release` (10–2000 ms), `knee` (0–24 dB), `makeup` (0–24 dB), `mix` |
| `compressor_opto` | Opto Compressor | `threshold`, `ratio` (1–20:1), `attack` (5–200 ms), `release` (50–3000 ms), `makeup`, `mix` |

### 2.3 Distortion / Saturation

| Type ID | Description |
|---------|-------------|
| `distortion` | Hard clip distortion |
| `overdrive` | Soft saturation overdrive |
| `fuzz` | Fuzz effect |

### 2.4 Equalization

| Type ID | Name | Key Parameters |
|---------|------|----------------|
| `eq_parametric` | 4-Band Parametric EQ | `lowGain/Freq`, `lowMidGain/Freq/Q`, `highMidGain/Freq/Q`, `highGain/Freq` (gains ±12 dB) |

### 2.5 Modulation

| Type ID | Name |
|---------|------|
| `chorus` | Chorus |
| `flanger` | Flanger |
| `phaser` | Phaser |
| `tremolo` | Tremolo |
| `auto_wah` | Auto-Wah |

### 2.6 Pitch

| Type ID | Name | Key Parameters |
|---------|------|----------------|
| `pitch_shift` | Pitch Shift | `semitones` (-1..+1 normalized), `minSemitones`/`maxSemitones` (-12..+12), `mix`, `stepMode` (0/1) — uses Signalsmith Stretch |
| `transpose` | Transpose | `semitones` (-24..+12 st), `mix` — optimized for integer steps, uses Signalsmith Stretch |
| `transpose_stft` | Transpose (STFT) | `semitones` (-12..+12 st), `mix` — STFT phase-vocoder alternative for A/B comparison |
| `octave` | Octave Effect | `octaveUp`, `octaveDown`, `tone`, `mix` — uses Signalsmith Stretch for fixed +/-12 semitone voices |

### 2.7 Delay

| Type ID | Name | Key Parameters |
|---------|------|----------------|
| `delay_digital` | Digital Delay | `timeMs` (1–2000), `feedback` (0–0.95), `mix` (0–1) |
| `delay_doubler` | Stereo Doubler | `time` (0–100 ms), `mix` (0–1) |

### 2.8 Reverb (Algorithmic Family)

All types share `decay`, `mix`, and `preDelay`. Each has tuned internals for its style.

| Type ID | Name | Notable Extra Parameters |
|---------|------|--------------------------|
| `reverb_room` | Room | `size`, `damping` |
| `reverb_hall` | Hall | `size`, `damping`, `width` |
| `reverb_plate` | Plate | `tone`, `damping` |
| `reverb_chamber` | Chamber | `size`, `tone` |
| `reverb_spring` | Spring | `tone`, `drive` |
| `reverb_shimmer` | Shimmer | `size`, `shimmer` |
| `reverb_ambient` | Ambient | `size`, `diffusion`, `width` |
| `reverb_advanced` | Advanced | Full control: `diffusion`, `lowCut`, `highCut`, `modRate`, `modDepth`, `ducking`, `drive`, `shimmer`, `tone`, `width` |

### 2.9 Synthesis

| Type ID | Description |
|---------|-------------|
| `synth_saw` | Sawtooth synth layer |

### 2.10 Utility

| Type ID | Name | Description |
|---------|------|-------------|
| `gain` | Gain | Simple gain stage; `gainDb` (-24..+24 dB) |
| `splitter` | Splitter | Fan-out to N parallel branches; auto-paired with a `mixer` node at rejoin |
| `mixer` | Mixer | Sums N inputs with per-edge gain; auto-inserted, not user-addable directly |

---

## 3. NAM Amp Modeling

**Key files:** `core/src/dsp/effects/NAMAmpEffect.h`, `core/src/dsp/effects/OptimizedNAMAmpEffect.h`, `core/src/dsp/MultiPresetMixer.h`

- Loads `.nam` model files (NAM community format).
- **SIMD-optimized inference**: `simd/OptimizedNAM.h`, `simd/OptimizedLSTM.h`, `simd/OptimizedWaveNet.h`, `simd/OptimizedActivations.h` (~2× speedup over scalar).
- **NAM Calibration**: Offline job (`NAMCalibrationJob`) computes input/output gain calibration per model. Results cached by model SHA-256 hash (`calibration/models/index.json`). Calibration aligns NAM's dBu reference to DAW dBFS peak reference.
- **Auto-level**: Per-preset flags `autoLevelInput`/`autoLevelOutput` apply model calibration gains automatically. Interface calibration reference defaults to +12.0 dBu @ 0 dBFS peak (configurable in Settings).
- **Model hashing**: `ModelHasher` computes SHA-256 for deduplication and calibration cache keys.

---

## 4. IR Convolution (Cabinet Simulation)

**Key files:** `core/src/dsp/RealtimeConvolver.h`, `core/src/dsp/IRWavLoader.h`, `core/src/dsp/effects/IRCabEffect.h`

- Loads `.wav` impulse response files.
- **Algorithm**: Uniformly Partitioned Overlap-Save (UPOLS) — zero heap allocation in audio callback.
- **SIMD FFT**: `simd/SimdFFT.h` for accelerated frequency-domain convolution.
- **Quality presets**: 0=Economy, 1=Standard, 2=High, 3=Full — trades block size and IR length for CPU vs quality.
- **IR resampling**: Handles sample-rate mismatch between IR and session (linear interpolation; cubic is a known improvement in TODO).
- Also used for **convolution reverb** (`IRReverbEffect.h`).

---

## 5. Multi-Preset Mixer

**Key files:** `core/src/dsp/MultiPresetMixer.h/.cpp`

Runs multiple presets simultaneously with independent mix controls — used for A/B comparison and blended tones.

- Each preset runs its own full `SignalGraphExecutor` instance in parallel.
- Per-preset controls: **mix level**, **pan** (equal-power), **mute**, **solo**.
- **Master gain** and **output limiter** applied post-mix.
- **Input trim** and **auto-level** applied globally before per-preset processing.
- **Mono/stereo input mode** selection.
- **Signal diagnostics**: Per-node input/output signal levels, DSP load % and timing — streamed to UI as `dspPerformance` and `signalLevelDiagnostics` messages.
- **Tuner callback**: DSP thread notifies tuner of pitch data.
- **Metronome audio**: Mixed into output stream.

---

## 6. Composite Effects

**Key files:** `core/src/dsp/effects/CompositeEffectProcessor.h/.cpp`, `core/src/presets/PresetTypes.h` (`CompositeEffectDefinition`, `ExposedParameter`)

A composite bundles multiple effects into a single reusable node with a simplified control surface.

- **Inner graph**: A full `SignalGraph` runs inside a single `EffectProcessor` node via a nested `SignalGraphExecutor`.
- **Exposed parameters**: Each maps a user-facing control (`paramId`, `displayName`) to an inner node parameter (`nodeId`, `nodeParamKey`). Supports range override, default override, unit label, and curve (`linear`, `log`, `exp`).
- **Custom layout**: Optional `EffectLayout` defines visual control positions, background, text labels.
- **Registration**: Each definition registers with `EffectRegistry` as `composite:{definitionId}`.
- **Storage**: `data/v1/composites/user/<compositeId>.json` and `factory/` for built-ins.
- **User flow**: Build a signal path → select nodes → bundle into composite → choose exposed params → optionally design layout → saved to composite library → appears in FX browser.
- **Messages**: `compositeLibrary`, `compositeDefinitionAdded`, `compositeDefinitionRemoved` (engine→UI); `saveCompositeDefinition`, `deleteCompositeDefinition`, `addCompositeToGraph` (UI→engine).

---

## 7. Blend Editor (Multi-Model NAM)

**Key files:** `core/ui/ts/blendEditor.ts`, `core/ui/ts/blendManager.ts`, `core/ui/ts/blendUtils.ts`, `core/src/dsp/effects/MultiModelNAMAmpEffect.h`

Blending maps multiple NAM models to a set of physical parameters, enabling smooth transitions between captures taken at different amp/pedal settings.

- **BlendDefinition** fields: `id`, `name`, `category`, `models` (NAM IDs), `blendMode` (`snap` | `interpolate`), `modelMappings` (per-model parameter values), `parameters` (active control list: gain, drive, treble, bass, middle, presence, tone, level, etc.).
- **Blend modes**: `snap` = discrete model switching; `interpolate` = smooth crossfade between adjacent models.
- **Auto-map**: Regex-based extraction of parameter values from model file names.
- **Blend editor UI**: Three tabs — Settings (name, mode, model list, parameter selection), Test (live knob preview), Model Browser (searchable).
- **Safety**: Blends in use by presets are protected from deletion.
- **Storage**: `data/v1/blends/library.json`.
- **DSP**: `MultiModelNAMAmpEffect` processes N models in parallel and crossfades outputs. Uses piecewise linear interpolation between adjacent model positions. See TODO.md for planned `amp_nam_blend` graph node type.

---

## 8. Preset System

**Key files:** `core/src/presets/PresetStorage.h/.cpp`, `core/src/presets/PresetTypes.h`, `core/ui/ts/presets.ts`

### 8.1 Preset Data Model (PresetV2)

| Field | Type | Description |
|-------|------|-------------|
| `id` | UUID string | Unique identifier |
| `name` | string | Display name (max 100 chars) |
| `version` | int | Schema version (must be 2) |
| `author`, `category`, `description`, `tags` | string/array | Metadata |
| `global` | GlobalSettings | Input/output trim, auto-level, transpose |
| `graph` | SignalGraph | Effect signal graph |
| `embeddedResources` | EmbeddedResource[] | Portable base64-encoded resources |

**GlobalSettings**: `inputTrim`, `outputTrim`, `outputVolume`, `autoLevelInput`, `autoLevelOutput`, `transpose` (semitones, -24..+12).

### 8.2 CRUD Operations

| Operation | Description |
|-----------|-------------|
| Load | Resolve resources from library/file/embedded; apply to DSP |
| Save | Write JSON to `data/v1/presets/user/<id>.json` |
| Delete | Remove preset file |
| List | Enumerate factory + user presets |
| Import | Load from file; extract embedded resources to cache |
| Export | Serialize with optional embedded resources |

### 8.3 Versioning & Migration

- V1 used a linear `effects[]` array; V2 uses a `graph` (DAG). V1 presets auto-migrate on load.
- Schema version field enables forward detection of future formats.

### 8.4 Embedded Resources

For portable sharing, resources embed as base64 in the preset JSON (`EmbeddedResource`): `id`, `type`, `name`, `hash` (SHA-256), `data` (base64), `originalPath`. Extracted to `data/v1/cache/resources/<sha256>/` on load.

### 8.5 Preset Organization

- **Folders**: Hierarchical tree; `preset-folders.json` stores structure with `presetIds[]` references (no data duplication). Nested arbitrarily. Case-insensitive name matching for merge.
- **Setlists**: Ordered slot lists (`presetId` references, repeatable). Each setlist may have a `bank` number (unique integer) for future hardware mapping. Stored in `settings/ui/setlists.json`.
- **Favorites**: `settings/ui/preset-favorites.json`.
- **Ratings**: `settings/ui/preset-ratings.json`.
- **Import**: Default folder = `Imported`; full-export imports merge folder trees.

---

## 9. Resource Library

**Key files:** `core/src/resources/ResourceLibrary.h/.cpp`

Central catalog of NAM models and IR files.

- **Storage layout** (`data/v1/resources/`):
  - `indexes/resources-index.json` — canonical index with `resourceId`, `resourceType`, `provider`, `contentHash`, `filePath`, `displayName`, `originalFileName`.
  - `content/<provider>/<hash-named-file>` — actual binaries (hash-addressed for deduplication).
- **ResourceRef resolution priority**: 1) library (resourceType + resourceId), 2) embedded (embeddedId), 3) file path.
- **Content deduplication**: SHA-256 hash prevents duplicate storage.
- **Dynamic loading/unloading**: Resources loaded on demand; catalog supports queries by type, category, tags.

---

## 10. Tone3000 Integration

**Key files:** `core/ui/ts/tone3000.ts`, `core/ui/ts/tone3000Browser.ts`

Integration with the Tone3000 cloud amp model library.

- **Authentication**: API key stored in `settings/app.json` (`tone3000.apiKey`), exchanged for OAuth-style tokens at `https://www.tone3000.com/api/v1/auth/session`. Token refresh handled automatically.
- **Browse categories**: Pedals/FX, Preamps, Amps, Full Rigs, Cabinet IRs.
- **Search**: Query-based tone search with category filtering.
- **Import**: Download individual NAM models or IR files into the local resource library.
- **Blend creation**: Import multiple models from a single tone entry and combine into a blend definition.
- **Content shown**: Title, description, tags, image, gear info, download count.
- **External API**: `https://www.tone3000.com/api/v1`.

---

## 11. Community Preset Sharing

**Key files:** `core/ui/ts/toneSharingPanel.ts`, `core/ui/ts/archiveUtils.ts`

Peer-to-peer community preset sharing via the Soundshed platform.

- **Authentication**: Session-ID based auth via `https://api-guitar.soundshed.com/v1`.
- **Browse modes**: Featured, Items (individual presets), Packs (preset collections), Installed, My Content.
- **Publish**: Users share presets and packs with title, description, tags, thumbnail.
- **Pack creation**: Bundle multiple presets into curated collections.
- **Download**: Full archive format containing presets + referenced resources (NAM models, IRs).
- **Import**: Unpacks archive, registers resources into local library, merges presets and folder structure.
- **Archive format** (`archiveUtils.ts`): Sanitizes filenames, bundles preset JSON with resource binaries, generates unique IDs.
- **External API**: `https://api-guitar.soundshed.com/v1`.

---

## 12. Signal Path Editor (UI)

**Key files:** `core/ui/ts/signalPath.ts`, `core/ui/ts/fxSelector.ts`, `core/ui/ts/controls.ts`, `core/ui/ts/eqCurve.ts`

Visual node graph editor for building and editing effect chains.

- **Graph visualization**: Nodes shown in signal-flow order with connection lines. Selection highlights node details.
- **Add effects**: Drag from the categorized FX browser (`fxSelector.ts`) or insert after a selected node.
- **Remove effects**: Delete node from graph; graph re-connects adjacent nodes.
- **Reorder**: Drag-drop reordering of nodes within the chain.
- **Bypass**: Per-node enable/disable toggle.
- **Parameter editing**: Knobs and sliders update `params` via `updateSignalPathNodeParam` message (debounced 50 ms).
- **Resource assignment**: Per-node resource picker for NAM models and IRs; supports browse-file and library lookup.
- **EQ curve visualization** (`eqCurve.ts`): Interactive frequency-response curve for parametric EQ nodes with per-band drag handles.
- **Parallel paths**: Add a Splitter node to fan out; the Mixer is auto-inserted at the rejoin point.
- **Custom layouts** (`layoutRenderer.ts`, `layoutDesigner.ts`, `layoutManager.ts`): Composite nodes (and some built-in effects) can have custom visual control surfaces stored in `data/v1/layouts/`.
- **FX categories in browser**: Dynamics, Amplifiers, Cabinets, EQ, Modulation, Synth, Delay, Reverb, Utilities.

---

## 13. Preset Browser (UI)

**Key files:** `core/ui/ts/presets.ts`, `core/ui/ts/views.ts`

- List all factory + user presets; filter by folder, search text, category, tags.
- Load preset into DSP with single click.
- Save current state as new preset (name, category, description).
- Rename, delete, duplicate presets.
- Folder tree navigation; create, rename, delete folders; move presets between folders.
- Setlist panel: create/rename/delete setlists, drag-drop slot reordering, assign bank number.
- Mark presets as favorites; star ratings (stored in settings).
- Export individual presets or full folder exports (with optional embedded resources).
- Import presets from file (auto-extracts embedded resources).

---

## 14. Multi-Preset Mixer (UI)

**Key files:** `core/ui/ts/views.ts`, `core/ui/ts/messages.ts`

UI for the multi-preset parallel mixing engine.

- Add/remove presets from the active mix.
- Per-preset mix level fader, pan control, mute button, solo button.
- Master gain fader and limiter toggle.
- DSP load and signal-level meters displayed per preset.
- Messages: `addActivePreset`, `removeActivePreset`, `setPresetMix`, `setPresetPan`, `setPresetMute`, `setPresetSolo`, `setMasterGain`, `setLimiterEnabled`.

---

## 15. Tuner

**Key files:** `core/ui/ts/tuner.ts`

Real-time chromatic guitar tuner driven by DSP pitch detection.

- Displays detected note name, cents deviation, and raw frequency.
- Visual needle/indicator for tuning accuracy.
- Live input waveform visualization.
- Reference frequency adjustment (default A=440 Hz).
- Start/stop via UI button or `tuner` message (`{action: "start"|"stop"}`).
- Engine sends `tunerUpdate` messages with `{note, cents, frequency}` during active tuning.

---

## 16. Metronome

**Key files:** `core/ui/ts/metronome.ts`

Audio metronome mixed directly into the DSP output stream.

- BPM control with freeform entry and ±1 nudge buttons.
- **Tap tempo**: Calculates BPM from successive tap intervals.
- Volume and pan controls for click track in mix.
- Click sound selection.
- Host sync option (locks to DAW transport BPM when available).
- Enable/disable toggle.
- Engine-side state broadcast as `metronomeState` message.

---

## 17. Demo Audio & Riff Library

**Key files:** `core/ui/ts/demoAudio.ts`, `core/ui/ts/riffLibrary.ts`

### Demo Audio

Built-in test signals for non-destructive preset auditioning without live guitar input.

- **Source types**: Built-in guitar riff clips, white noise, frequency sweeps.
- **Favorite riffs**: Mark clips as favorites for quick access.
- **Playback control**: Preview any clip; stops on `stopDemoAudio` message.
- Engine sends `previewStarted`, `previewComplete`, `previewStopped` notifications.

### Riff Library

Record and playback of user guitar takes for offline preset editing.

- **Capture**: Record live guitar input in sync with the metronome.
- **Waveform visualization**: Rendered waveform of captured audio.
- **Trim**: Select playback region.
- **Playback**: Loop captured riff through current preset for editing.
- **Import WAV**: Load external audio files as riff clips.
- **Favorites**: Mark takes for quick recall.
- **Save/load takes**: Persistent across sessions.

---

## 18. DSP Diagnostics

**Key files:** `core/src/dsp/MultiPresetMixer.h`, `core/ui/ts/views.ts`

Real-time performance and signal monitoring.

- **Signal level meters**: Input and output levels per active preset node, streamed as `signalLevelDiagnostics` messages.
- **DSP performance**: CPU load %, per-block timing (µs), node-level timing breakdown — streamed as `dspPerformance` messages.
- **Signal path test**: `runSignalPathTest` message triggers a test tone through the graph and reports frequency, duration, and elapsed time.
- **Diagnostics toggle**: Enabled via `diagnostics.signalLevelsEnabled` in `settings/app.json`.
- **Log panel**: Console log capture displayed in the UI log tab (`core/ui/ts/logging.ts`).

---

## 19. Settings

**Key files:** `core/ui/ts/settings.ts`, `core/src/resources/ResourceLibrary.h`

Application configuration panel.

- **Audio device preferences**: Open native audio device settings dialog (`openAudioPreferences` message).
- **Resource folders**: Configure search paths for NAM models and IR files.
- **NAM interface calibration**: Reference level (dBu @ 0 dBFS) for auto-leveling. Enabled by default at +12.0 dBu. Can be disabled to use model metadata as-is.
- **Theme selection**: Cycle or select UI theme (see §20).
- **Tone3000 API key**: Enter and persist API key for Tone3000 integration.
- **Diagnostics toggle**: Enable/disable real-time signal level and DSP performance streaming.
- **Advanced options toggle**: Reveals additional developer/power-user controls.
- **Update check**: Software version checking and notification (`core/ui/ts/updateCheck.ts`).
- **Window state persistence**: Width, height stored in `settings/ui/window-state.json`.
- **App settings file**: `settings/app.json` (schemaVersion, theme, lastPresetId, feature flags).

---

## 20. Theme System

**Key files:** `core/ui/ts/theme-switcher.ts`, `core/ui/css/variables.css`

CSS variable-based theming with five built-in themes.

| Theme | Class | Description |
|-------|-------|-------------|
| Default | (none) | Medium-contrast cool grays |
| Light | `.theme-light` | High brightness, clean, modern |
| Dark | `.theme-dark` | Low brightness, reduced eye strain |
| Classic 70s | `.theme-classic` | Warm vintage browns/ambers |
| Worn Pedal | `.theme-gritty` | Gritty, worn metal textures |

- All colors use CSS variables (`--color-primary`, `--bg-primary`, `--text-primary`, etc.) — no hardcoded values in components.
- **Persistence**: Selection stored in `localStorage` and `settings/app.json`.
- **Cycle**: Click 🎨 icon in the icon bar or call `themeSwitcher.cycleTheme()`.
- **Variable categories**: Primary colors, backgrounds, text, borders, controls, overlays, shadows.

---

## 21. UI Architecture & Messaging

**Key files:** `core/ui/ts/main.ts`, `core/ui/ts/state.ts`, `core/ui/ts/bridge.ts`, `core/ui/ts/messages.ts`, `core/src/PluginController.h/.cpp`, `core/src/MessageDispatcher.h/.cpp`

### WebView SPA

- Single-page TypeScript application hosted in a native WebView (WebView2 on Windows, WKWebView on macOS).
- **State**: Global singleton in `state.ts` — single source of truth for presets, signal chain, mixer, metronome, riff library, composite edit mode, UI panel state.
- **Navigation**: Tab/panel switching — main panels: visualizer, library, settings, sharing, advanced, mixer; sub-tabs for preset details and logs.
- **Notifications**: Toast notification system (`notifications.ts`).
- **Dialogs**: Modal confirmation dialogs (`dialogs.ts`).

### Message Protocol

Bidirectional JSON messages over the WebView bridge:

```json
{ "type": "messageType", "payload": { ... }, "timestamp": 1704801234567 }
```

**Key engine→UI messages**: `state`, `presetLoaded`, `presetSaved`, `presetList`, `error`, `tunerUpdate`, `dspPerformance`, `signalLevelDiagnostics`, `globalChain`, `effectCatalog`, `compositeLibrary`, `metronomeState`, `previewStarted`, `previewComplete`, `namCalibrationStatus`, `resourceImported`, `layoutLibraryLoaded`.

**Key UI→engine messages**: `uiReady`, `requestState`, `setParameter`, `loadPreset`, `savePreset`, `addSignalPathNode`, `deleteSignalPathNode`, `replaceSignalPathNode`, `reorderSignalPathNode`, `updateSignalPathNodeParam`, `updateSignalPathNodeBypass`, `updateNodeResource`, `addActivePreset`, `setPresetMix`, `setPresetMute`, `setPresetSolo`, `setMasterGain`, `tuner`, `setMetronome`, `runSignalPathTest`, `previewDemoAudio`, `importRemoteResource`, `saveCompositeDefinition`, `getEffectCatalog`, `getPresetList`, `openAudioPreferences`.

### Message Dispatching (C++)

`MessageDispatcher` routes ~60+ message types to `PluginController` handlers, split across seven files by domain: state, parameters, presets/resources, signal path, settings, mixer, library/composite.

### Startup Sequence

1. WebView loads UI SPA
2. UI sends `uiReady`
3. UI sends `requestState`
4. Engine sends full `state` snapshot
5. UI renders initial state

### Conflict Resolution

Engine is authoritative. If a `state` broadcast contains a different value than what the UI sent optimistically, the UI adopts the engine value.

---

## 22. Plugin Formats & Platform

**Key files:** `core/src/IPluginHost.h`, framework wrappers under `iplug2/` and `juce/`

### Supported Formats

| Format | Platforms | Notes |
|--------|-----------|-------|
| VST3 | Windows, macOS | Full automation, host preset management, latency reporting |
| Audio Unit (AU v2 + v3) | macOS | CoreAudio integration |
| AAX | Windows, macOS | Pro Tools; requires PACE signing |
| Standalone App | Windows, macOS | WASAPI/ASIO (Windows), CoreAudio (macOS) |

### IPluginHost Interface

`IPluginHost` abstracts all framework differences: WebView messaging, file dialogs, threading dispatch (`RunOnMainThread`), state persistence hooks, parameter automation.

### Threading Model

| Thread | Priority | Role |
|--------|----------|------|
| Audio | Realtime | DSP processing — no allocations, no locks |
| UI | Normal | WebView rendering, user interaction |
| Background | Below Normal | Model loading, file I/O, network |

DSP ↔ UI uses `try_lock` on `mDSPMutex`; UI dispatches to audio thread via `RunOnMainThread()`.

### State Serialization

Plugin state: version header + normalized parameter values + compressed preset JSON blob.

### Audio Specifications

- **Buffer sizes**: 32–2048 samples (256 recommended)
- **Sample rates**: 44.1, 48, 88.2, 96 kHz (full); 176.4, 192 kHz (best-effort)
- **I/O**: Stereo (1 stereo bus)

---

## 23. Data Storage Layout

**Key file:** `docs/settings-storage-v1.md`

All persistent data lives under `<ROOT>/data/v1/` where:
- Windows: `%APPDATA%/Soundshed Guitar/data/v1/`
- macOS: `~/Library/Soundshed Guitar/data/v1/`

```
data/v1/
  settings/
    app.json                      # Global settings, theme, API keys, feature flags
    ui/
      preset-favorites.json
      preset-ratings.json
      setlists.json
      window-state.json
  presets/
    preset-folders.json           # Folder tree (references only, no preset data)
    user/<presetId>.json          # User preset files
  resources/
    content/<provider>/<hash>     # Binary NAM/.wav files, hash-addressed
    indexes/resources-index.json  # Resource catalog
  blends/
    library.json                  # Blend definitions
  composites/
    user/<compositeId>.json       # User composite definitions
  layouts/
    content/<layoutId>.layout.json
    indexes/effect-layouts.json
    images/
  calibration/
    models/index.json             # Per-model NAM calibration cache
  logs/
    session-log.txt
```

---

## See Also

- [architecture-overview.md](architecture-overview.md) — System layers, threading, security
- [signal-chain.md](signal-chain.md) — Graph execution model and JSON schema
- [fx-library.md](fx-library.md) — Full effect parameter tables
- [data-models.md](data-models.md) — PresetV2, SignalGraph, ResourceRef JSON schemas
- [composite-effects.md](composite-effects.md) — Composite effect data model and lifecycle
- [user-interface.md](user-interface.md) — Full message catalog and UI architecture
- [network-api.md](network-api.md) — REST API spec and plugin format details
- [preset-library-organization.md](preset-library-organization.md) — Folder/setlist organization rules
- [settings-storage-v1.md](settings-storage-v1.md) — Storage schema and JSON contracts
- [theme-system.md](theme-system.md) — CSS variable theming and adding themes
