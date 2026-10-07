# Signal Level Handling

This document explains the current signal level handling in the simplest possible terms.

## Short Version

Today the app handles level in four main places:

1. A single user-wide input calibration gain can be applied before the chain.
2. Manual gain controls in the preset and effect graph shape level through the chain.
3. NAM effects can apply model-metadata-based dBu calibration to input and output.
4. The mixer can clamp the final output to a configurable ceiling.

There is no hidden global headroom stage that permanently turns everything down.

## The Real Signal Path

For normal playback, level handling happens in this order:

1. Raw stereo input enters the mixer.
2. Raw input diagnostics are measured.
3. The input mode can put one input, or both summed, on both sides (Mono); a DAW's bus decides instead.
4. The active user input calibration profile applies one fixed gain change.
5. Legacy mixer-wide auto input could run here, but the controller currently keeps it off.
6. Input diagnostics are measured again after the active input processing.
7. The global pre-chain runs.
   This is where the shared noise gate and transpose live.
8. Each active preset runs its own signal graph.
9. Preset outputs are mixed together with mix and pan.
10. The global post-chain runs.
    This is where shared post effects such as EQ and doubler live.
11. Master gain is applied.
12. Legacy mixer-wide auto output could run here, but the controller currently keeps it off.
13. Output diagnostics are measured.
14. If the final limiter is enabled, the output is clamped to the configured protection ceiling.

## What Actively Changes Level Today

### 1. User Input Calibration

User input calibration is the main global input-level system.

- It is stored in app settings as named profiles.
- The active profile resolves to one fixed gain value in dB.
- That gain is applied once at the mixer input before the pre-chain and before any preset processing.
- If no profile is active, the gain is `0 dB`.
- While the calibration training flow is open, the applied gain is forced to `0 dB` so the capture is honest.

In simple terms: this is the app's main way to compensate for a guitar or interface being too hot or too quiet.

### 2. Manual Gains In The Chain

After the input calibration stage, normal level changes come from explicit controls:

- global input gain and output gain
- per-preset mix and pan in the mixer
- per-effect gain, drive, level, trim, makeup gain, and similar controls
- NAM input gain and output gain parameters

In simple terms: if the sound gets louder or quieter while you edit the rig, it should usually be because of an explicit control.

### 3. NAM Calibration (Use Calibration)

NAM effects apply a single dBu‑based calibration stage using model metadata.

- **Input calibration**: `gain = calibrationInputLevel_dBu − model.inputLevel_dBu`. Matches the interface reference level to the model's expected input. Active for each NAM node that has calibration enabled and a loaded model.
- **Output calibration**: `gain = model.outputLevel_dBu − calibrationInputLevel_dBu`. Reconstructs the real‑world output level so downstream effects see consistent drive regardless of which model is loaded.
- Both corrections are clamped to ±24 dB and combined with the user's manual input/output gain knobs.
- The **Use Calibration** advanced toggle (on by default) can be disabled to bypass all automatic correction, leaving only the manual gain controls active.
- The interface level comes from **NAM auto input calibration** in Settings. Switching that off withdraws the level from every NAM node, which then applies no correction, just as with Use Calibration off. (The engine sends this as its own `calibrationInputLevelEnabled` flag rather than as a NaN level, which Release builds cannot carry.)

In simple terms: when enabled, the NAM effect reads the model's dBu metadata and the interface calibration level to reconstruct correct gain staging automatically. No loudness normalization or product‑level gain metadata is applied.

### 4. Final Output Protection

The final protection stage is a hard clamp in the mixer.

- The shared protection ceiling is configurable in Settings and defaults to `-1 dBFS`.
- The final limiter uses that ceiling directly.
- The retired mixer-wide auto output stage also reads the same ceiling, but that path is currently forced off in normal product flow.

In simple terms: this is the last safety stop before audio leaves the mixer.

## What Is Still In The Code But Mostly Retired

Some old level systems still exist in the data model or message flow for compatibility, but they are not the main live behavior anymore.

### Mixer-Wide Auto Input And Auto Output

Removed on 19 September 2026. The mixer's peak-based auto input and auto output stages, the `setAutoLevel` message and the preset and global-chain flags are gone. Older presets, saved chains and app settings may still carry `autoLevelInput`/`autoLevelOutput`: they are ignored on load, and stripped from app settings and from NAM node params by the existing migrations. The Output Protection Ceiling now only sets where the output limiter clips.

### Designed Peak Input

Presets can store a `designedPeakInputDbfs` value.

- The UI can capture the current raw input peak and store it in the preset.
- The value is displayed back to the user.
- It is not part of the live DSP gain path.

In simple terms: this is reference information, not an active gain processor.

## Current Defaults

- User input calibration gain: `0 dB` unless an active profile is selected
- NAM Use Calibration: on by default (enables model-metadata-based dBu correction)
- Shared output protection ceiling: `-1 dBFS`
- Mixer-wide auto input: off in current product flow
- Mixer-wide auto output: off in current product flow
- Signal diagnostics: always on

## Settings That Matter Right Now

These settings actively affect signal level behavior:

- `audio.userInputCalibration.profiles`
- `audio.userInputCalibration.activeProfileId`
- `audio.dsp.outputProtectionCeilingDbfs`

These settings are applied at startup and also when changed at runtime.

In simple terms: if you change the protection ceiling in Settings, the running DSP picks it up immediately.

## Diagnostics

The app always streams signal diagnostics.

Three top-level snapshots matter:

- `rawInput`: before mono mode and before user input calibration
- `input`: after the active input processing
- `output`: after master gain and after any retired auto output stage, but before the final hard clamp limiter

The app also records per-node peak, RMS, and clip counts for:

- the global pre-chain
- every active preset graph
- the global post-chain

One important detail: because output diagnostics are measured before the final hard clamp, the diagnostics can show a hotter value than the final delivered sample if the limiter had to catch it.

## The Practical Mental Model

If you want the simplest mental model, use this one:

- Use user input calibration to match your instrument and interface to the app.
- Use normal gain controls to shape tone and balance.
- Leave NAM **Use Calibration** enabled so models automatically correct for their dBu metadata.
- Let the final protection ceiling stop the output from going too high.

That is the current system in practice.

## Main Source Files

If you need to inspect the code behind this behavior, start here:

- `core/src/dsp/MultiPresetMixer.cpp`
- `core/src/dsp/MultiPresetMixer.h`
- `core/src/dsp/LevelTargets.h`
- `core/src/dsp/effects/OptimizedNAMAmpEffect.h`
- `core/src/dsp/effects/MultiModelNAMAmpEffect.h`
- `core/src/PluginController.cpp`
- `core/src/dispatcher/MessageDispatchSettings.cpp`
- `core/ui/ts/settings.ts`
- `core/ui/ts/views.ts`