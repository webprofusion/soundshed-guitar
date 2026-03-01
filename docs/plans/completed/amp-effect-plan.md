# Built-in Amp Effect Plan

> **Status: IMPLEMENTED** — Both Phase 1 (original spec) and Phase 2 ("Heavy American v2") are fully implemented in `core/src/dsp/effects/BuiltinAmpEffect.h`.


## Goal
Create a built-in amp effect that covers clean, driving rock, and heavy modern metal tones. The amp should provide a smooth Clean/Drive voice blend and include controls for Gain, Bass, Middle, Treble, Contour, and Presence.

## DSP Approach
- Implement a new `EffectProcessor` with a dual-voice preamp:
  - **Clean voice**: mild saturation and lower pre-gain.
  - **Drive voice**: stronger saturation and higher pre-gain.
- Blend voices with a **smoothed** `voice` parameter (0..1) using a simple one-pole smoother.
- Apply a shared tone stack after the voice blend:
  - Low shelf (Bass)
  - Mid peaking EQ (Middle)
  - Contour peaking EQ (negative gain scoop)
  - High shelf (Treble)
  - Presence peaking EQ (upper-mid presence)
- Keep processing real-time safe (no allocations in audio callback).

## Parameters
| Key | Purpose | Range | Default | Unit |
| --- | --- | --- | --- | --- |
| `voice` | Clean/Drive blend | 0..1 | 0.0 | toggle (smoothed internally) |
| `gain` | Preamp gain | 0..1 | 0.45 | amount |
| `bass` | Low shelf | 0..1 | 0.5 | amount |
| `middle` | Mid peak | 0..1 | 0.5 | amount |
| `treble` | High shelf | 0..1 | 0.5 | amount |
| `contour` | Mid scoop | 0..1 | 0.2 | amount |
| `presence` | Upper-mid boost | 0..1 | 0.5 | amount |

## File Touchpoints
- DSP effect: `src/src/dsp/effects/BuiltinAmpEffect.h`
- Registry: `src/src/dsp/effects/BuiltinEffects.h`
- UI effect registry: `core/ui/ts/presetV2.ts`
- FX icon mapping: `core/ui/ts/iconAssets.ts`

## Notes
- The voice control is implemented as a smooth blend (not a hard switch).
- The effect does not require external resources.
- Preset defaults should align between C++ and UI.


----------------------------------
Heavy American v2 Spec (Implemented)

Goals

Amp realism that reduces dependence on NAM for core tones.
Maintain low CPU and real‑time safety (no allocations, no locks in audio thread).
Preserve existing presets (default behavior unchanged when new params are at defaults).
Signal Flow (per channel)

Input conditioning
HPF (existing), optional bright switch (new)
Preamp gain stages
Drive stage with unified gain and stageGain (existing), soft clip
Pre‑tone shaping
Optional pre‑emphasis (new)
Tone stack
Bass/Middle/Treble/Contour (existing)
Power amp (new)
Presence + Depth (separate from preamp presence)
Power‑amp drive, sag, and bias/asymmetry
Speaker interaction (new, lightweight)
Resonance + damping (impedance curve approximation)
Output trim (existing)
New Parameters (suggested)

bright (toggle, default 0): boost upper mids pre‑gain for clarity at low gain.
preEmphasis (amount 0–1, default 0): tilt EQ before gain to shape saturation.
powerDrive (amount 0–1, default 0): drive the power amp stage.
sag (amount 0–1, default 0): supply droop, slower recovery.
bias (amount -1 to 1, default 0): asymmetry for harmonics and feel.
depth (amount 0–1, default 0.4): low‑end resonance.
resonance (amount 0–1, default 0.4): speaker bump intensity.
damping (amount 0–1, default 0.5): speaker tightness (inversely affects resonance width).
Parameter Mapping / Behavior

Defaults keep current sound (all new params at neutral values).
gain remains the main saturation amount; stageGain remains dB trim for overall drive structure.
powerDrive and sag should be subtle at mid values to avoid big loudness jumps.
bias should not change overall RMS too dramatically (compensate if needed).
DSP Details (lightweight)

Bright: 1‑pole high‑shelf or simple high‑pass blend pre‑gain.
Pre‑emphasis: tilt EQ (low‑cut + high‑shelf) scaled by param.
Power amp stage: extra soft‑clip with separate drive and a mild low‑freq rolloff.
Sag: envelope follower on preamp output controlling gain reduction; attack ~5–20 ms, release ~100–300 ms.
Bias: offset before soft‑clip, then remove DC offset (light HPF).
Speaker interaction: a resonant low‑shelf bump and high‑end rolloff using existing biquad helpers.
UI/UX

Group new params under “Power Amp” and “Speaker” sections in the node panel.
Keep the main panel simple: Bright (toggle), Power Drive, Sag, Presence, Depth.
Advanced panel for Bias/Resonance/Damping/Pre‑emphasis.
Preset Compatibility

Existing presets: use defaults for new params.
Do not change existing parameter keys/meaning.
If a preset contains unknown fields, ignore them gracefully.
Testing / Verification

A/B compare old vs new with new params at default: should match within small tolerance.
CPU impact measured on 48 kHz with max 4 stages.
Ensure no denormals or NaNs (add small noise floor or flush‑to‑zero if needed).
