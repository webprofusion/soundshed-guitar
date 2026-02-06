# Skill

## Assumptions
- This document targets audio DSP export quality for the GuitarFX project.
- “Export” refers to rendering or saving audio output (offline or real-time capture) from the DSP chain.

## Key Files
- docs/signal-chain.md
- docs/fx-library.md
- docs/data-models.md
- docs/user-interface.md

## Overview
This document defines core concepts and quality expectations for audio DSP export with two primary goals:
- Low audible noise across the full export path.
- Avoidance of digital clipping while preserving intended loudness.

It applies to offline renders and real-time capture and should be used as a checklist when implementing or reviewing export paths.

## Core Concepts
- Gain staging: Maintain consistent signal levels between nodes to prevent cumulative overload or noise amplification.
- Headroom: Reserve peak margin (e.g., several dB below full scale) for transients and inter-sample peaks.
- Noise floor: Minimize quantization, rounding, and denormal issues; avoid unnecessary gain boosts.
- Dynamic range: Balance loudness with transient integrity; avoid aggressive limiting unless explicitly desired.
- Sample rate and bit depth: Use suitable precision for internal processing; prefer higher precision internally and apply final conversion at export.

## Export Quality Guidelines
- Normalize only when explicitly requested; do not normalize by default if it risks transient damage.
- Use floating-point processing internally and dither only at the final integer bit‑depth conversion stage.
- Apply a true‑peak aware limiter when required to prevent inter‑sample clipping.
- Avoid clipping by design: keep average and peak levels within safe bounds throughout the chain.
- Validate that export does not exceed $0\,\text{dBFS}$, including inter-sample peaks where applicable.
- Ensure silence remains silent: no DC offset, no denormal noise, and no unintended residual output.

## Pitfalls and Mitigations
| Pitfall | Impact | Mitigation | Notes/Detection |
|---------|--------|------------|----------------|
| Over‑hot gain staging | Hard clipping, harsh distortion | Track per‑node levels and apply trims | Meter per node; check for peaks near $0\,\text{dBFS}$ |
| Missing headroom | Transient clipping | Keep peaks below full scale; reserve margin | True‑peak meter or oversampled peak scan |
| Uncontrolled normalization | Transient flattening, pumping | Normalize only with clear user intent | Compare crest factor pre/post |
| No dither on final bit‑depth | Low‑level quantization artifacts | Dither once at final integer export | Listen to fades; spectral analysis |
| Excessive limiting | Loss of punch, smeared transients | Use minimal limiting; prefer gentle compression | Compare dynamics and transient sharpness |
| DC offset | Reduced headroom, LF bias | High‑pass or DC blocker if needed | Measure mean offset of waveform |
| Denormal noise | CPU spikes, noise in silence | Add tiny noise or use denormal handling | Observe CPU spikes during silence |
| Sample‑rate mismatch | Aliasing, tonal shift | Resample with high‑quality SRC | Sweep test tones; alias check |
| Excessive makeup gain | Raised noise floor | Apply gain only after assessing noise | Compare noise floor before/after |

## Review Checklist
- All stages remain within safe peak limits with headroom.
- Export path preserves dynamics without unintended clipping.
- Final conversion uses correct bit‑depth and dither strategy.
- Silence and low‑level passages remain clean.

## See Also
- docs/signal-chain.md
- docs/fx-library.md
- docs/data-models.md
- docs/user-interface.md
