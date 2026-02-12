# D1 — Tuner

## Intent
Provide a real-time tuner utility that estimates pitch from the input signal and reports musical note + deviation to the UI.

## Actors
- End user (starts/stops tuner)
- UI client (displays tuning info)
- Engine (runs pitch detection)

## Inputs
- Incoming audio signal
- User commands: start/stop, reference frequency, live mode

## Outputs
- Periodic tuner updates: note name, octave, frequency, cents deviation, confidence
- Start/stop acknowledgements

## Behavioral Requirements
### R1 — Start/stop
- The tuner must support explicit start and stop commands.
- Starting must emit a “started” event containing the active reference frequency and live-mode state.
- Stopping must emit a “stopped” event.

### R2 — Reference frequency
- The tuner must support setting the reference frequency (A4), defaulting to 440 Hz.
- Changing the reference must emit an acknowledgement event.

### R3 — Live mode
- Live mode controls whether normal audio continues to pass through while tuning.
- Changing live mode must emit an acknowledgement event.

### R4 — Update cadence
- While active, the tuner must publish updates frequently enough for an interactive UI.
- Updates must be rate-limited to avoid UI overload; the rate must be stable and documented.

### R5 — Real-time safety
- Pitch detection must not block the audio thread.
- If pitch detection is too expensive for per-block processing, it must be throttled and/or moved off the audio callback.

### R6 — Robustness
- Under silence/noise, the tuner must produce:
  - `detected = false`
  - Confidence near zero
  - No NaN/Inf outputs

## Test Strategy
- Synthetic sine wave tests (known frequencies)
- Noise/silence tests
- Reference frequency shifts

## Traceability (Non-Normative)
- core/src/dsp/MultiPresetMixer.h (tuner result, enable, reference)
- core/src/PluginController.cpp (tuner message handling + update publish)
- docs/user-interface.md (tuner messages)
