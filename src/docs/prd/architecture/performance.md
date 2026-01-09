# Performance Requirements

## Overview

This document specifies performance requirements and guidelines for NeuronGuitar, ensuring responsive real-time audio processing and smooth user experience across target platforms.

## Performance Targets

### Audio Processing

| Metric | Target | Measurement |
|--------|--------|-------------|
| Processing Latency | < 10ms @ 44.1kHz | End-to-end at 256 samples |
| CPU Usage (idle) | < 5% single core | No audio input |
| CPU Usage (active) | < 30% single core | Typical preset loaded |
| CPU Usage (peak) | < 50% single core | Complex preset, 96kHz |
| Audio Dropout Rate | 0% | Normal operation |

### Resource Loading

| Metric | Target | Measurement |
|--------|--------|-------------|
| NAM Model Load | < 2s | Cold load from disk |
| NAM Model Hot Swap | < 500ms | Pre-cached model |
| IR Load | < 200ms | Standard length IR |
| Preset Load | < 500ms | Including resources |
| Initial Startup | < 3s | Until UI responsive |

### User Interface

| Metric | Target | Measurement |
|--------|--------|-------------|
| UI Frame Rate | 60fps | During interaction |
| Parameter Response | < 50ms | Knob turn to visual update |
| Preset List Scroll | 60fps | 1000+ presets |
| Search Response | < 200ms | Local search |
| Remote Search | < 2s | Network dependent |

### Memory Usage

| Metric | Target | Measurement |
|--------|--------|-------------|
| Base Memory | < 100MB | Plugin loaded, no preset |
| Typical Usage | < 300MB | Standard preset |
| Maximum Usage | < 500MB | Complex preset |
| WebView Overhead | < 100MB | UI active |

## Buffer Size Support

| Buffer Size | Status | Notes |
|-------------|--------|-------|
| 32 samples | Supported | May need optimization |
| 64 samples | Supported | Low latency target |
| 128 samples | Supported | Standard low latency |
| 256 samples | Recommended | Balance of latency/CPU |
| 512 samples | Supported | Lower CPU usage |
| 1024 samples | Supported | High track count |
| 2048 samples | Supported | Maximum efficiency |

## Sample Rate Support

| Sample Rate | Status | Notes |
|-------------|--------|-------|
| 44,100 Hz | Full Support | CD quality |
| 48,000 Hz | Full Support | Video standard |
| 88,200 Hz | Full Support | High resolution |
| 96,000 Hz | Full Support | High resolution |
| 176,400 Hz | Best Effort | May increase CPU |
| 192,000 Hz | Best Effort | May increase CPU |

## CPU Optimization

### Audio Thread Requirements

```
Audio callback MUST:
- Complete within buffer deadline
- Use no heap allocations
- Avoid system calls
- Use lock-free synchronization
- Have bounded worst-case execution time

Audio callback SHOULD:
- Use SIMD instructions
- Minimize cache misses
- Avoid virtual function calls in hot path
```

### SIMD Utilization

| Operation | SIMD Target |
|-----------|-------------|
| Buffer operations | SSE2/AVX |
| Convolution FFT | AVX/AVX2 |
| Filter processing | SSE2 |
| Gain/Mix | SSE2 |

### Memory Access Patterns

```
Optimization priorities:
1. Sequential access for audio buffers
2. Cache line alignment for DSP data
3. Hot/cold data separation
4. Pre-fetching for predictable access
```

## NAM Model Performance

### Model Complexity Tiers

| Tier | Max Parameters | CPU Target |
|------|----------------|------------|
| Light | < 100K | < 5% |
| Standard | 100K - 500K | < 15% |
| Heavy | 500K - 2M | < 30% |
| Ultra | > 2M | < 50% |

### Model Loading Pipeline

```
1. Background thread reads file from disk
2. Parse model structure
3. Allocate weight buffers
4. Copy weights to aligned memory
5. Pre-warm with silent blocks
6. Atomic swap to active model
```

### Pre-warming

NAM models are pre-warmed to avoid initial latency:

```
Pre-warm process:
- Process 10 blocks of silence
- Allows neural network state to stabilize
- Prevents startup glitches
- Completes before model becomes active
```

## IR Convolution Performance

### Partitioned Convolution

```
Partition sizes by IR length:
- < 512 samples: Direct convolution
- 512 - 2048: 2 partitions
- 2048 - 8192: 4 partitions
- > 8192: 8+ partitions

First partition processed in time domain for lowest latency
Remaining partitions use FFT for efficiency
```

### FFT Size Selection

| IR Length | FFT Size | Partitions |
|-----------|----------|------------|
| 256 | N/A | Direct |
| 512 | 1024 | 1 |
| 1024 | 2048 | 1 |
| 2048 | 2048 | 2 |
| 4096 | 2048 | 4 |
| 8192 | 4096 | 4 |

## Memory Management

### Buffer Allocation Strategy

```
Startup allocation:
- Pre-allocate all processing buffers
- Size for maximum supported block size
- Align to cache line boundaries

Runtime:
- No allocations in audio thread
- Reuse pre-allocated buffers
- Pool allocators for temporary data
```

### Memory Pools

| Pool | Purpose | Size |
|------|---------|------|
| Audio Buffers | Processing | 16MB |
| FFT Workspaces | Convolution | 8MB |
| Message Queue | UI Communication | 1MB |
| Temporary | Non-realtime ops | 4MB |

## Threading Model

### Thread Priorities

| Thread | Priority | Purpose |
|--------|----------|---------|
| Audio | Realtime | Audio processing |
| UI | Normal | User interaction |
| Background | Below Normal | Loading, network |

### Lock-Free Communication

```
Audio thread communication:
- Parameter updates: Atomic load/store
- Model swap: Atomic pointer exchange
- State queries: Lock-free queue

Never in audio thread:
- Mutex acquisition
- Condition variable wait
- Memory allocation
- File I/O
```

## Benchmarking

### Test Scenarios

| Scenario | Description |
|----------|-------------|
| Idle | No audio input, simple preset |
| Typical | -18dB input, standard preset |
| Stress | 0dB input, complex preset, 96kHz |
| Worst Case | Full signal chain, parallel paths |

### Measurement Points

```
Instrumentation locations:
1. Audio callback entry/exit
2. NAM inference duration
3. Convolution processing time
4. Effect chain total time
5. UI render frame time
6. Message round-trip time
```

### Performance Regression Testing

```
CI pipeline includes:
- Automated benchmarks
- Comparison to baseline
- Alert on > 10% regression
- Performance trend tracking
```

## Platform-Specific Considerations

### Windows

```
Optimizations:
- Thread priority boost for audio
- Large page support for buffers
- WASAPI exclusive mode support
- AVX2 when available
```

### macOS

```
Optimizations:
- Real-time thread scheduling
- Accelerate framework for FFT
- Metal compute (future)
- ARM NEON for Apple Silicon
```

## Power Efficiency

### Mobile/Laptop Considerations

```
Power-saving features:
- Reduce polling when idle
- Batch UI updates
- Sleep background threads when inactive
- Lower priority for non-essential tasks
```

### Battery Impact Targets

| State | Target |
|-------|--------|
| Idle | Minimal impact |
| Active | < 10W additional |
| Recording | < 15W additional |

## Related Documents

- [Audio Engine Specification](./audio-engine.md)
- [NAM DSP Manager](./nam-dsp.md)
- [IR Manager](./ir-manager.md)
