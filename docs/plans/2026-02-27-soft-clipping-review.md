# Soft Clipping Review

**Date:** 2026-02-27

## Summary

Review of where soft clipping is appropriate across existing effects and what the implementation should be.

---

## What Already Has It (No Changes Needed)

| Effect | Existing Approach |
|--------|------------------|
| `OverdriveEffect` | `tanh(x * driveGain)` — it IS a soft clipper |
| `DistortionEffect` | Hard clip by design — keep it |
| `FuzzEffect` | `1 − e^(−|x|)` — already smooth |
| `BuiltinAmpEffect` | `SoftClip(x) = tanh(x)` at every stage |

---

## Candidates — Prioritized

### A. New `SoftClipperEffect` (drive category) — Highest Value

Fills the gap between warm tanh (Overdrive) and hard-clip (Distortion). Uses the **cubic polynomial**:

```
y = 1.5x - 0.5x³  for |x| ≤ 1
y = ±1             for |x| > 1
```

- Produces only odd harmonics; computationally free (no transcendental functions unlike `tanh`/`atan`)
- Linear at small amplitudes (no coloring when clean), smooth saturation knee, hard bounded at ±1 for safety
- Parameters: `drive` [0..1], `level` [−12..12 dB], `mix` [0..1], `tone` [0..1]
- Follows exact pattern of `DistortionEffect` / `FuzzEffect`
- Referenced in `docs/plans/core-feature-plan.md`: "Clipper-based saturation: Various clipping algorithms"

**New file:** `core/src/dsp/effects/SoftClipperEffect.h`  
**New GUID:** add `kSoftClipper` to `core/src/dsp/EffectGuids.h`  
**Alias:** `"soft_clip"`

---

### B. `LimiterEffect` — Add `mode` Parameter (hard / soft) — Moderate Value

The final `std::clamp(outL, -ceiling, ceiling)` safety catch is abrasive near the ceiling. Soft mode replaces it with:

```
out = ceiling * tanh(out / ceiling)
```

This is mathematically bounded (never exceeds ceiling), rounds off peaks musically, and models an analog-style soft limiter. The 2026-02-25 FX review flagged "No saturation output protection" — this addresses it cleanly.

- Add `mode` parameter: `0` = hard (current behavior, default), `1` = soft
- Default `mode=0` preserves backward compatibility for existing presets

**Modify:** `core/src/dsp/effects/LimiterEffect.h`

---

### C. `GainEffect` — Add Optional `clip` Parameter — Moderate Value

GainEffect is often used as a preamp boost in a chain (e.g., before an IR cab). At +24 dB it can push signal to ~15× amplitude, causing hard downstream clipping. Adding `clip` [0=off, 1=on] applies `tanh(output)` post-gain, making it a tube-style preamp stage.

- `tanh` is near-linear for small signals — no coloring when clean
- Only engages when signal is driven hard
- Default `clip=0` preserves existing behavior

**Modify:** `core/src/dsp/effects/GainEffect.h`

> **Note:** If GainEffect should stay a pure utility, skip this and rely on chaining `GainEffect → SoftClipperEffect` in presets instead.

---

### D. `CompressorEffect` — Optional Output Saturation — Lower Value

Makeup gain can push output beyond 0 dBFS (flagged in the 2026-02-25 FX review). Adding a `saturate` boolean applies normalized soft saturation on the compressor output:

```
y = tanh(1.5x) / tanh(1.5)
```

This is unity-gain for small signals, adds 3rd-harmonic coloring when driven, and models the harmonic character of hardware compressors (e.g. SSL G-Bus). Opt-in, default off.

**Modify:** `core/src/dsp/effects/CompressorEffect.h`

---

## Files

| Action | File |
|--------|------|
| Create | `core/src/dsp/effects/SoftClipperEffect.h` |
| Modify | `core/src/dsp/EffectGuids.h` — add `kSoftClipper` UUID |
| Modify | `docs/fx-library.md` — document new effect |
| Modify | `core/src/dsp/effects/LimiterEffect.h` — add `mode` param |
| Modify | `core/src/dsp/effects/GainEffect.h` — add `clip` param |
| Modify | `core/src/dsp/effects/CompressorEffect.h` — add `saturate` param |

---

## Verification

1. Build debug, run `ctest --build-config Debug --output-on-failure` from `core/build`
2. Add test cases to `tests/EffectProcessorTests.cpp`:
   - `SoftClipperEffect` at drive=1.0 produces no hard discontinuities (smooth 3rd-harmonic content)
   - `LimiterEffect` soft mode: output never exceeds ceiling even at hard transients
   - `GainEffect` with `clip=1` at +24 dB: output bounded to ±1.0
3. Manually verify `SoftClipperEffect` produces audibly smoother breakup than `DistortionEffect` at equivalent drive

---

## Further Considerations

1. **SoftClipperEffect character parameter**: A `character` param [0..1] could morph between cubic polynomial (0) and `atan`-based (1). `atan`: `y = (2/π) * atan(x * driveGain)` — infinitely smooth, no hard boundary. Recommend starting with cubic only and adding the morph in a follow-up.

2. **GainEffect scope creep**: Adding `clip` nudges GainEffect toward being a dedicated saturator. If purity is preferred, chain `GainEffect → SoftClipperEffect` in presets instead.

3. **LimiterEffect soft mode default**: Changing the default to soft would affect existing presets that rely on brickwall behavior. Keep `mode=0` (hard) as default; consider exposing the soft limiter as a separately named effect (`"Soft Limiter"`) if the distinction matters to users.
