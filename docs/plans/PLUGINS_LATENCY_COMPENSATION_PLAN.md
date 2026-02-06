# Plugin Latency Compensation Implementation Plan

## Overview
This document outlines the implementation plan for adding plugin latency compensation to address DAW timing issues, with priority support for VST3 and AU formats using an incremental approach.

## Current State Analysis
- Plugin currently reports `PLUG_LATENCY 0` to all hosts
- Individual effects have internal latency (IR convolution, pitch shifting) but it's not accumulated
- No automatic delay compensation system in signal chain
- Host receives no latency information, causing timing issues

## Implementation Phases

### Phase 1: Foundation (Low Risk)
**Timeline: 1-2 weeks**

1. **Add latency reporting interface to EffectProcessor base class:**
   ```cpp
   class EffectProcessor {
   public:
       virtual int GetLatencySamples() const { return 0; }  // Override in latency-causing effects
       virtual void OnLatencyChanged() {}  // Callback when latency changes
   };
   ```

2. **Update latency-causing effects:**
   - `IRCabEffect`: Return `mConvolverL.GetLatency()` (256-512 samples)
   - `PitchShiftEffect`: Return `static_cast<int>(mMinLatency * mSampleRate / 1000.0)`
   - Other effects: Return 0 (no processing latency)

3. **Create latency calculation utilities:**
   - Sample rate aware conversions (ms <-> samples)
   - Common latency calculation helpers

**Files to modify:**
- `src/EffectProcessor.h/cpp`
- `src/IRCabEffect.cpp`
- `src/PitchShiftEffect.cpp`
- `src/LatencyUtils.h/cpp` (new)

### Phase 2: Chain Integration (Medium Risk)
**Timeline: 2-3 weeks**

1. **Modify SignalGraphExecutor for latency accumulation:**
   ```cpp
   class SignalGraphExecutor {
   private:
       int mTotalLatency = 0;
       void CalculateChainLatency();
       int GetPathLatency(const std::string& nodeId) const;
       void ApplyDelayCompensation(std::vector<float>& buffer, int delaySamples);
   };
   ```

2. **Handle parallel paths:**
   - Use maximum latency across parallel branches
   - Add delay compensation buffers for faster paths

3. **Update MultiPresetMixer:**
   ```cpp
   class MultiPresetMixer {
   private:
       int mMaxLatencyAcrossPresets = 0;
       void SynchronizePresetLatencies();
   };
   ```

**Files to modify:**
- `src/SignalGraphExecutor.h/cpp`
- `src/MultiPresetMixer.h/cpp`

### Phase 3: Host Integration (Higher Risk)
**Timeline: 2-4 weeks**

1. **VST3 Implementation:**
   - Remove `PLUG_LATENCY 0` from `GuitarFXConfig.h`
   - Implement dynamic latency reporting via `IAudioProcessor::setLatencySamples()`
   - Handle host notifications on latency changes

2. **AU Implementation:**
   - Implement `kAudioUnitProperty_Latency` property
   - Ensure proper property listener notifications

3. **Update plugin configuration:**
   ```cpp
   // GuitarFXConfig.h - Remove static latency
   // #define PLUG_LATENCY 0  // DELETE THIS
   
   #define PLUG_SUPPORTS_DYNAMIC_LATENCY 1
   ```

**Files to modify:**
- `src/GuitarFXConfig.h`
- `src/GuitarFXPlugin.h/cpp`
- VST3-specific interface files
- AU-specific interface files

### Phase 4: Dynamic Updates (Polish)
**Timeline: 1-2 weeks**

1. **Handle sample rate changes** in `OnReset()`
2. **Update latency on parameter changes**:
   - IR loading/unloading
   - Pitch shift amount changes
   - Effect bypass/enable states

3. **Thread-safe latency updates:**
   ```cpp
   std::atomic<int> mReportedLatency{0};
   void UpdateLatency() {
       int newLatency = CalculateTotalLatency();
       if (mReportedLatency.exchange(newLatency) != newLatency) {
           ReportLatencyToHost(newLatency);
       }
   }
   ```

4. **Add user feedback:**
   - Latency indicator in UI (optional)
   - Debug logging for latency changes

## Key Implementation Considerations

### Audio Thread Safety
- Use `std::atomic<int>` for reported latency
- Buffer updates outside audio callback when possible
- Minimize locking in processing path
- Prefer lock-free data structures for delay buffers

### Incremental Compatibility
- New methods have default implementations (return 0)
- Existing presets continue to work unchanged
- Latency compensation activates gradually as effects are updated
- Feature flagging for gradual rollout if needed

### Buffer Management
- Pre-allocate delay compensation buffers
- Use efficient circular buffer implementation
- Maximum latency buffer sizing based on worst-case scenarios
- Memory alignment for SIMD operations

### Performance Optimization
- Minimize per-sample overhead in audio path
- Cache latency calculations until parameters change
- Batch latency updates when possible
- Profile audio thread to ensure real-time performance

## Testing Strategy

### Unit Tests
- Individual effect latency calculations
- Sample rate conversion accuracy
- Latency accumulation logic
- Buffer management operations

### Integration Tests
- Chain latency accumulation verification
- Multi-preset latency synchronization
- Host interface communication
- Dynamic latency change handling

### DAW-Specific Testing
- **VST3:** VST3 validator, Cubase, Studio One, Bitwig
- **AU:** AU Lab, Logic Pro, GarageBand
- Real-world timing verification with known latency sources
- Plugin format compatibility validation

### Performance Tests
- Audio thread timing measurements
- CPU usage impact assessment
- Memory usage analysis
- Buffer size and sample rate scaling

## Risk Assessment & Mitigation

### High Risks
- **Host compatibility issues:** Mitigate with thorough DAW testing
- **Audio thread blocking:** Use lock-free programming and careful synchronization
- **Preset compatibility breaking:** Maintain backward compatibility with incremental approach

### Medium Risks
- **Performance degradation:** Profile and optimize critical paths
- **Complex timing edge cases:** Comprehensive testing across various scenarios

### Low Risks
- **Interface changes:** Well-defined API contracts
- **Calculation errors:** Unit tests and code reviews

## Success Criteria

1. **Functionality:**
   - Accurate latency reporting to VST3 and AU hosts
   - Proper timing alignment across parallel processing paths
   - Dynamic latency updates on parameter changes

2. **Compatibility:**
   - Existing presets continue to work unchanged
   - No regressions in audio quality
   - Support across target DAWs and plugin formats

3. **Performance:**
   - Minimal impact on CPU usage (< 5% overhead)
   - No audio thread blocking or dropouts
   - Memory usage within acceptable limits

## Implementation Priority

1. **Phase 1:** Foundation - Essential for subsequent phases
2. **Phase 2:** Chain Integration - Core latency compensation logic
3. **Phase 3:** Host Integration - Critical for DAW timing issues
4. **Phase 4:** Dynamic Updates - Polishing and user experience

## Dependencies

### Internal Dependencies
- Effect processor architecture
- Signal graph execution framework
- Plugin format interface implementations

### External Dependencies
- VST3 SDK for host communication
- AU SDK for Audio Units integration
- iPlug2 framework for plugin infrastructure

## Rollback Strategy

Each phase can be rolled back independently:
- Phase 1: New methods have safe defaults
- Phase 2: Latency calculations can be disabled
- Phase 3: Can revert to static latency reporting
- Phase 4: Optional features can be disabled

This plan ensures robust, incremental implementation of plugin latency compensation while maintaining system stability and backward compatibility.