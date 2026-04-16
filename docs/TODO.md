# GuitarFX TODO

This document tracks outstanding tasks and improvements for the GuitarFX project.

## DSP (Digital Signal Processing)

### Correctness & Stability
- [ ] Fix NoiseGateEffect hold implementation: Currently sets hold parameter but doesn't use it in Process()—gate is instantaneous without hold time. Implement proper hold logic to maintain gate state for specified duration after signal drops below threshold.

### Testing
- [ ] Add unit tests for edge cases:
  - Extreme parameter values (e.g., max/min gains, Q factors, feedback levels)
  - Various buffer sizes (32, 64, 128, 256, 512, 1024, 2048, 4096 samples)
  - Sample rate transitions (44.1kHz, 48kHz, 88.2kHz, 96kHz)
  - Signal integrity checks (no NaN/Inf, silence detection, frequency response validation)

### Performance Optimization
- [ ] Implement SIMD optimizations in hot paths:
  - Vectorize ParametricEQ biquad processing loops using AVX intrinsics
  - Optimize buffer clearing/copying in SignalGraphExecutor with SIMD
  - Expand FFT SIMD usage beyond RealtimeConvolver to other frequency-domain effects
- [ ] Add performance benchmarking:
  - Profile CPU usage against <30% target for typical presets
  - Measure latency for convolution effects (limit IR length to maintain <10ms total latency)
  - Implement basic profiling infrastructure (e.g., chrono timers) for ongoing monitoring

### Other
- [ ] Review convolution resampling: Current linear interpolation may introduce aliasing—consider cubic interpolation for better quality at large sample rate mismatches.

---

## Planned Features

### Multi-Model NAM Blend Effect (`amp_nam_blend`)

**Goal**: Map arbitrary physical parameters to multiple captured NAM models, enabling smooth transitions between models captured at different amp/pedal settings. The blend control represents any parameter that varies across captures—Gain is the most common, but could also be Warp, Tone, Drive, or any unique pedal/amp control.

**Use Cases**:
- **Amp Gain**: Models captured at gain=1, 5, 10 → blend control sweeps gain range
- **Pedal Effect Parameter**: Models captured with "Warp" at 0%, 50%, 100% → blend recreates the warp sweep
- **Tone Stack**: Models captured with Treble at 0, 5, 10 → blend morphs between EQ settings
- **Multi-dimensional** (future): Map 2+ parameters (e.g., Gain × Tone grid) to 2D blend surface

**Technical Approach**: Audio mixing (primary) + weight interpolation (experimental)
- Hold 2+ NAM model instances simultaneously
- Process each active model in parallel
- Crossfade outputs based on blend parameter position
- Use piecewise linear interpolation between adjacent models
- Blend parameter is abstract—maps to whatever physical parameter was varied during capture

**Experimental Path**: Weight interpolation
- Keep as optional R&D path behind a feature flag
- Only valid when model architectures match exactly
- Not real-time safe for live modulation; precompute offline if used

**Why Audio Mixing over Weight Interpolation** (for initial implementation):
- Weight interpolation requires exact architecture match between models
- Non-linear activations make interpolated weights produce unpredictable audio
- Weight updates are not real-time safe (memory allocation, copying)
- Audio mixing works with any model architectures and uses proven patterns

**Implementation Steps**:
1. Extend `ResourceRef` in [PresetTypes.h](../core/src/presets/PresetTypes.h) to support multiple resources per node
2. Create `MultiModelNAMAmpEffect` in [dsp/effects/](../core/src/dsp/effects/) holding multiple `nam::DSP` instances (use optimized NAM processing where possible)
3. Implement parameter-mapped blending: map blend control to model indices with piecewise interpolation
4. Register effect in [BuiltinEffects.h](../core/src/dsp/effects/BuiltinEffects.h) as `amp_nam_blend`
5. Add UI support in [presetV2.ts](../core/ui/ts/presetV2.ts) and [fxSelector.ts](../core/ui/ts/fxSelector.ts)
6. Optimize: skip inactive model when blend is at 0.0 or 1.0

**Parameters**:
- `blend` (0.0–1.0): Controls crossfade position between models; represents the mapped physical parameter
- `blendLabel` (string, optional): Display name for the blend control (e.g., "Gain", "Warp", "Drive")
- `inputGain` (-24 to +24 dB): Input level before processing
- `outputGain` (-24 to +24 dB): Output level after processing

**Model Metadata** (per-resource):
- `parameterValue`: The physical parameter value this model was captured at (e.g., 5.0 for gain=5)
- `parameterId`: Identifier for the mapped parameter (e.g., "gain", "warp", "drive")—used to group compatible models
- Models with matching `parameterId` are ordered by `parameterValue` to enable correct interpolation

**Considerations**:
- CPU cost: Running 2 models doubles CPU usage during blend; use optimized NAM internally (~2x speedup)
- Model count: Start with 2-model blending, extensible to N models for finer parameter resolution
- Latency: Models should ideally be from the same architecture family for consistent latency
- Parameter semantics: Blend is unit-less (0–1); UI can optionally display mapped parameter range (e.g., "Gain: 1–10")


# Facotry Presets general notes
- Preset archive export from a presets folder should include information on the preset subfolder structure, import should (optionally) recreate that subfolder structure. Factory preset import should create/update the folder structure

- Hash the factory preset archive to decide if it has changed on startup, if it has changed re-import it. New factory presets should be added, existing presets should be updated

- Factory preset import should change the preset category to Factory in import

- Factory preset import should persits the imported persets and folder structure in the users preset structure, under a top level Factory Pesets folder

- Factory presets should Import to a Factory Presets folder with a folder structure, it should also import to our resource library with resource item metadata. Like a partial resource library export/import.