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