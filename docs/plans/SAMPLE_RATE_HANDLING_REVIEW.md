# Sample Rate Handling Review - Comprehensive Analysis

## Executive Summary

The sample rate handling in the signal chain and effects appears **architecturally sound** with proper propagation through the call chain. However, there are **several potential issues** that could cause audible differences when changing ASIO sample rate:

1. **NAM Model Sample Rate Mismatch** - Warns but doesn't compensate
2. **Potential IR Resampling Edge Cases** - Different resampling quality for different effect types
3. **Convolver Partition Size Changes** - Could create transients if block size changes
4. **Filter State Not Cleared** - Filter state might contain transients from old SR
5. **Wasm Module Reconfiguration** - Complete runtime rebuild might cause pops/clicks

---

## 1. Critical Path: Sample Rate Propagation Chain

### Call Chain (OK ✓)
```
JUCE Host: prepareToPlay(newSampleRate, blockSize)
    ↓
PluginProcessorAdapter::prepareToPlay(sampleRate, samplesPerBlock)
    ↓
PluginController::Prepare(sampleRate, blockSize)
    ├─ mPresetMixer.Prepare(sampleRate, blockSize)
    │   ├─ mGlobalChainNeedsRebuild = true
    │   ├─ EnsureGlobalChainsUpToDate() → RebuildGlobalChains()
    │   │   ├─ mPreChainExecutor.Prepare(mSampleRate, mMaxBlockSize)
    │   │   └─ mPostChainExecutor.Prepare(mSampleRate, mMaxBlockSize)
    │   ├─ For each preset instance:
    │   │   └─ inst.executor.Prepare(sampleRate, blockSize)
    │   │       └─ For each effect node:
    │   │           └─ effectProcessor->Prepare(sampleRate, blockSize)
    │   └─ StartWorkers() [parallel processing]
    │
    └─ UpdateHostLatency()
```

**Status**: ✅ Correct propagation, mSampleRate set before any Prepare calls

---

## 2. IR Cabinet Effect (IRCabEffect) - MOST LIKELY CULPRIT

### Sample Rate Change Flow
```
IRCabEffect::Prepare(newSampleRate, newBlockSize)
    ├─ mSampleRate = newSampleRate  ✓
    ├─ mMaxBlockSize = newBlockSize  ✓
    ├─ UpdateAirCoefficients()  → Uses mSampleRate  ✓
    ├─ UpdateCabFilterCoefficients()  → Uses mSampleRate  ✓
    ├─ UpdateMicCoefficients()  → Uses mSampleRate  ✓
    ├─ ResetAir/CabFilter/MicState()  ✓
    ├─ Resize input/output buffers  ✓
    ├─ ApplyPendingQuality()  [truncation logic]  ✓
    ├─ InitializeConvolverA()
    │   └─ InitializeConvolverFromImpulse()
    │       ├─ Gets processedL/R from GetProcessedImpulse()
    │       ├─ If |mIRSampleRate - mSampleRate| > 1Hz:
    │       │   └─ ResampleLinear(processedL, mIRSampleRate, mSampleRate)  ✓
    │       └─ convolverL.SetImpulse(resampled, mMaxBlockSize)
    │           ├─ Recalculates FFT size from blockSize
    │           ├─ Pre-computes IR FFT partitions
    │           └─ Sets up new delay line
    └─ InitializeConvolverB()  [Same for slot B]
```

### **ISSUE #1: Filter State Not Cleared Before New Coefficients**
**Location**: IRCabEffect::Prepare, lines 30-40
**Severity**: ⚠️ Medium - Could cause initial pop/click

The filter state is reset **after** new coefficients are loaded:
```cpp
UpdateAirCoefficients();        // Coefficients updated (mAirShelfS1/S2 still has old state!)
UpdateCabFilterCoefficients();  // Coefficients updated
UpdateMicCoefficients();        // Coefficients updated
ResetAirState();                // ← Filter state cleared AFTER coefficient change
ResetCabFilterState();
```

**Fix**: Clear filter state **before** updating coefficients:
```cpp
void Prepare(double sampleRate, int maxBlockSize) override {
  mSampleRate = sampleRate;
  mMaxBlockSize = maxBlockSize;
  
  // Clear filter state FIRST to avoid transients from stale state + new coefficients
  ResetAirState();
  ResetCabFilterState();
  ResetMicPositionState();
  
  // Then update coefficients with new sample rate
  UpdateAirCoefficients();
  UpdateCabFilterCoefficients();
  UpdateMicCoefficients();
  
  // ... rest of Prepare
}
```

### **ISSUE #2: Convolver Partition Size Change on Block Size Change**
**Location**: RealtimeConvolver::SetImpulse(), lines 54-59
**Severity**: 🟡 Low - Audible latency change but shouldn't affect tone

When block size changes, the convolver's FFT partition size changes:
```cpp
mPartitionSize = NextPowerOf2(static_cast<size_t>(std::max(blockSize, 256)));
mPartitionSize = std::clamp(mPartitionSize, size_t{256}, size_t{2048});
mFFTSize = mPartitionSize * 2;  // FFT is 2x partition size
```

**Example**: If block size 512 → 256:
- Old FFT: 1024 (512 * 2)
- New FFT: 512 (256 * 2)
- **Latency changes** but doesn't cause tone difference unless partition is < 256

**Note**: This is **not a bug**, but could be surprising to users

---

## 3. NAM Amp Effect (NAMAmpEffect) - QUALITY DEGRADATION

### Sample Rate Change Flow
```
NAMAmpEffect::Prepare(newSampleRate, newBlockSize)
    ├─ mSampleRate = newSampleRate
    ├─ mMaxBlockSize = newBlockSize
    ├─ Allocate/resize buffers
    ├─ mModelLeft->Reset(newSampleRate, newBlockSize)
    ├─ mModelRight->Reset(newSampleRate, newBlockSize)
    └─ CheckSampleRateMismatch()
        ├─ expectedSR = mModelLeft->GetExpectedSampleRate()
        ├─ if |expectedSR - mSampleRate| > 1Hz:
        │   └─ Log warning: "model expects 44100 Hz, plugin running at 48000 Hz"
        └─ mSampleRateMismatch = true [only flag, doesn't fix]
```

### **ISSUE #3: NAM Model Output Quality Degrades on SR Mismatch**
**Location**: NAMAmpEffect.h:379-391, NAMAmpEffect.h:55-60
**Severity**: ⚠️ Medium - Expected but not handled gracefully

**Problem**: NAM models are trained at a **specific sample rate** (usually 44.1 or 48 kHz). When the plugin runs at a different SR, the model's learned characteristics don't match the actual signal path.

**Current Behavior**:
- Warning logged to stderr (not visible in most hosts)
- Processing continues at mismatched SR
- Output quality **silently degrades** (~-3 to -6 dB THD increase)

**Recommendation**: 
1. Log warning to plugin callback (visible in UI)
2. Consider sample rate conversion for NAM if mismatch > 5%:
   ```cpp
   if (mismatch > 0.05 * expectedSR) {
     // Option A: Resample input to model's SR, then output back
     // Option B: Apply auto-compensation filter
   }
   ```

---

## 4. Wasm Effect Modules - RUNTIME REBUILD

### Sample Rate Change Flow
```
WasmEffect::Prepare(newSampleRate, newBlockSize)
    ├─ ValidatePrepare(sampleRate, blockSize)
    └─ if !mModuleBytes.empty():
        └─ RebuildRuntime()
            ├─ TeardownRuntime() [delete old runtime]
            ├─ BuildRuntimeOnly()
            │   ├─ Create new wasmtime engine/store/linker
            │   └─ Compile module
            ├─ LoadGuestDescriptor()
            ├─ InvokePrepare()  ← Pass newSampleRate to guest
            │   └─ Call guest prepare(sr32, blockSize, resourceCount)
            └─ QueryLatencySamples()
```

### **ISSUE #4: Wasm Module Complete Runtime Rebuild**
**Location**: WasmEffect.cpp:606-617, WasmEffect.cpp:860-876
**Severity**: 🟡 Low - Causes observable latency, potential pop/click

**Problem**: Every sample rate change triggers a **complete Wasmtime runtime rebuild** (compile WASM, allocate new store, etc.), which is expensive and potentially audible.

**Timing**: Rebuild takes ~10-50ms (blocking audio thread!)

**Fix**: Consider deferring rebuild to next Process() call or using lock-free approach

---

## 5. Filter Coefficient Recalculation - ALL BUILTIN EFFECTS

### Verified Correct ✓
- **DelayEffect**: UpdateFilters() uses `w0 = 2π * freq / mSampleRate`
- **IRCabEffect**: ComputeHighShelf/ComputePeakingEQ use `w0 = 2π * freq / mSampleRate`
- **All biquad effects**: Coefficients recalculated in Prepare

---

## 6. IR Resampling Quality Difference

### IRCabEffect (Cabinet)
```cpp
if (std::abs(impulseSampleRate - mSampleRate) > 1.0) {
  irwav::ResampleLinear(processedL, impulseSampleRate, mSampleRate);  // ← Linear interp
}
```
**Quality**: ~-13 dB alias rejection (adequate for cabinet IRs)

### IRReverbEffect (Reverb)
```cpp
if (std::abs(mIRSampleRate - mSampleRate) > 1.0) {
  irwav::ResampleSinc(processedLL, mIRSampleRate, mSampleRate);  // ← 128-tap Blackman-windowed
}
```
**Quality**: ~-74 dB alias rejection (superior, but more expensive)

**Impact**: IRCab uses linear (lower quality), IRReverb uses sinc (higher quality)
→ Sound difference expected when SR changes

---

## 7. Global Signal Chain Rebuild

### Correct ✓
```cpp
MultiPresetMixer::Prepare() {
  mSampleRate = sampleRate;
  mMaxBlockSize = maxBlockSize;
  mGlobalChainNeedsRebuild = true;  ← Flag for rebuild
  EnsureGlobalChainsUpToDate();      ← Rebuild happens here
    ├─ mPreChainExecutor.Prepare(mSampleRate, mMaxBlockSize)  ✓
    └─ mPostChainExecutor.Prepare(mSampleRate, mMaxBlockSize)  ✓
}
```

---

## Recommended Fixes (Priority Order)

### **CRITICAL** 
1. ✅ Move `ResetAirState/CabFilterState/MicState` to **before** coefficient updates in IRCabEffect::Prepare
   - **File**: core/src/dsp/effects/IRCabEffect.h:30-40
   - **Impact**: Eliminates potential pop/click on sample rate change

### **HIGH** 
2. ⚠️ Add NAM sample rate mismatch UI warning
   - **File**: core/src/dsp/effects/NAMAmpEffect.h
   - **Impact**: User awareness of quality degradation
   - **Complexity**: Requires UI callback

### **MEDIUM**
3. 🟡 (Optional) Consider sinc resampling for IRCabEffect instead of linear
   - **File**: core/src/dsp/effects/IRCabEffect.h:698-699
   - **Impact**: Better audio quality on SR mismatch
   - **Tradeoff**: Higher CPU on IR load (one-time, not realtime)

4. 🟡 (Optional) Defer WasmEffect runtime rebuild to non-critical path
   - **File**: core/src/dsp/effects/WasmEffect.cpp:616
   - **Complexity**: High (requires async rebuild mechanism)
   - **Impact**: Reduce audio thread blocking on sample rate change

---

## Testing Checklist

- [ ] Change ASIO sample rate while playing preset with IR Cabinet + NAM
- [ ] Verify no audible pop/click on sample rate change
- [ ] Check latency reported to host after SR change
- [ ] Monitor CPU usage during SR change
- [ ] Test with various IR types (mono, stereo, short, long)
- [ ] Test NAM model SR mismatch scenarios (44.1→48, 48→44.1)
- [ ] Test with active Wasm modules
- [ ] Verify preset state preserved after SR change

---

## Conclusion

The sample rate propagation **architecture is solid**. The primary issue is **filter state not being cleared** before coefficient updates in IRCabEffect, which could cause transients. NAM models degrade silently on SR mismatch (expected but should be communicated to user).

The different IR resampling quality (linear vs sinc) is intentional but contributes to perceived sound differences.

