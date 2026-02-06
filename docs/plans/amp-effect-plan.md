# Built-in Amp Effect Plan

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
- UI effect registry: `src/resources/ui/ts/presetV2.ts`
- FX icon mapping: `src/resources/ui/ts/iconAssets.ts`

## Notes
- The voice control is implemented as a smooth blend (not a hard switch).
- The effect does not require external resources.
- Preset defaults should align between C++ and UI.
