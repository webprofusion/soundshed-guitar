# Audio Engine Specification

## Overview

The Audio Engine is responsible for all real-time digital signal processing in NeuronGuitar. It processes audio through a configurable signal graph of effect nodes, including neural amp models, impulse response convolution, and traditional DSP effects.

## Design Goals

1. **Low Latency**: Sub-10ms end-to-end processing at standard sample rates
2. **Deterministic Performance**: Consistent CPU usage regardless of signal content
3. **No Allocations**: Zero heap allocations in the audio callback
4. **Thread Safety**: Safe parameter updates from non-audio threads
5. **Flexibility**: Support arbitrary effect ordering and parallel paths

## Components

### Signal Graph Executor

The Signal Graph Executor routes audio through the configured effect chain.

**Responsibilities:**
- Parse and validate signal graph topology
- Execute nodes in correct dependency order
- Manage audio buffer allocation and routing
- Handle bypass states per node
- Support parallel paths with mixing

**Graph Execution Algorithm:**
1. Topologically sort nodes based on edges
2. Allocate intermediate buffers for parallel paths
3. Process nodes in sorted order
4. Apply edge gains during mixing
5. Copy final output to destination buffer

### NAM DSP Manager

Manages Neural Amp Modeler instances for amp simulation.

**Responsibilities:**
- Load and cache NAM model files
- Pre-warm models to avoid initial latency spike
- Process audio through neural network inference
- Handle model switching without audio interruption

**Model Loading Protocol:**
1. Load model file in background thread
2. Initialize neural network weights
3. Pre-warm with silent audio blocks
4. Atomically swap active model pointer

### IR Manager

Handles impulse response loading and convolution processing.

**Responsibilities:**
- Load IR files (WAV, AIFF formats)
- Resample IRs to match session sample rate
- Configure convolution engine with loaded IR
- Support stereo and mono configurations

**Convolution Implementation:**
- Partitioned convolution for low latency
- FFT-based processing for efficiency
- Configurable partition sizes

### Effect Processors

Individual DSP effect implementations.

**Built-in Effect Categories:**
- **Dynamics**: Noise gate, compressor, limiter
- **Distortion**: Drive, saturation, fuzz
- **Equalization**: Parametric EQ, tilt EQ, graphic EQ
- **Modulation**: Chorus, flanger, phaser, tremolo
- **Time-based**: Delay (digital, tape, analog), reverb
- **Utility**: Gain, splitter, mixer

**Effect Interface:**
```
interface EffectProcessor:
    method configure(sample_rate, max_block_size)
    method reset()
    method process(input_buffer, output_buffer, num_samples)
    method set_parameter(param_id, value)
    method get_parameter(param_id) -> value
    method get_latency_samples() -> int
```

### Effect Registry

Runtime registration of available effect types.

**Responsibilities:**
- Register effect type metadata
- Create effect instances by type ID
- Provide parameter definitions for UI
- Support versioned effect types

## Audio Processing Pipeline

### Block Processing

```
For each audio block:
    1. Read input from host buffer
    2. Apply input trim gain
    3. Execute signal graph:
       a. Find graph entry node
       b. For each node in topological order:
          - If enabled: process through effect
          - If bypassed: pass-through
       c. Mix parallel paths at mixer nodes
    4. Apply output trim gain
    5. Write to host output buffer
```

### Parameter Updates

Parameters are updated using a lock-free mechanism:

1. UI thread writes to parameter staging buffer
2. Audio thread atomically reads staged values
3. Parameters smoothed over configurable time
4. Avoids clicks/pops during adjustment

### Model/IR Switching

Resource switching is designed to be glitch-free:

1. New resource loaded in background thread
2. Crossfade buffer prepared
3. Atomic pointer swap on audio thread
4. Brief crossfade masks discontinuity

## Performance Considerations

### CPU Optimization

- SIMD instructions for buffer operations
- Cache-friendly memory layout
- Minimal branching in hot path
- Pre-computed lookup tables

### Memory Management

- Pre-allocated processing buffers
- Object pools for temporary allocations
- Fixed-size intermediate buffers

### Latency Reporting

Total latency = sum of node latencies

Each node reports its latency in samples for:
- Look-ahead processing (dynamics)
- Convolution latency (IR processing)
- Internal buffering requirements

## Sample Rate Handling

The engine supports common sample rates:
- 44,100 Hz
- 48,000 Hz
- 88,200 Hz
- 96,000 Hz

On sample rate change:
1. Notify all effect processors
2. Reload/resample IRs if needed
3. Reconfigure internal buffers
4. Report new latency to host

## Error Recovery

### Processing Errors
- Catch exceptions (should not occur)
- Output silence or pass-through
- Set error flag for UI notification
- Log error for diagnostics

### Resource Errors
- Missing model/IR: Use bypass
- Corrupted file: Report and skip
- Memory exhaustion: Graceful degradation

## Configuration Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| Input Trim | -40 to +20 dB | 0 dB | Input gain adjustment |
| Output Trim | -40 to +20 dB | 0 dB | Master output level |
| Max Block Size | 32-8192 | 2048 | Maximum samples per block |
| Crossfade Time | 5-100 ms | 20 ms | Model switch crossfade |

## Related Documents

- [NAM DSP Manager](./nam-dsp.md)
- [IR Manager](./ir-manager.md)
- [Effect Registry](./effect-registry.md)
- [Signal Graph Model](./signal-graph-model.md)
