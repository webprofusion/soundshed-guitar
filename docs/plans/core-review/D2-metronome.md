# D2 — Metronome

## Intent
Provide a metronome click generator for standalone use (and optionally tempo-following behavior when hosted in a DAW).

## Actors
- End user (enables metronome, sets BPM/volume)
- UI client (controls and displays state)
- Host environment (provides sample rate; optionally tempo)

## Inputs
- User commands to set BPM, enable/disable, and click style parameters
- Sample rate (required)
- Host tempo (optional, when available)

## Outputs
- Click audio mixed into the final stereo output
- Metronome state updates to UI

## Behavioral Requirements
### R1 — Availability
- In standalone mode, metronome must be available.
- In plugin/DAW mode, metronome may be unavailable or may follow host tempo; behavior must be explicit.

### R2 — State controls
Metronome must support:
- `enabled` boolean
- `bpm` within a bounded range
- Output volume control
- Stereo pan control

### R3 — Tempo source
- If a host tempo is available and metronome is allowed in that context, metronome should be able to follow host tempo.
- If not, it uses a user-provided BPM.

### R4 — Click generation
- Clicks must be scheduled in sample-accurate time based on BPM and sample rate.
- Click waveform may be synthesized or sample-based.
- Downbeat accent (e.g., first beat of bar) should be supported where feasible.

### R5 — Persistence
- Metronome settings should persist across sessions in user settings.

### R6 — Real-time safety
- Click mixing must be real-time safe.
- Any sample loading/resampling must be off the audio thread.

## Error Handling
- If sample-based click assets are unavailable, the system must fall back to synthesized click.

## Test Strategy
- BPM range clamping tests
- Beat scheduling accuracy tests
- Fallback behavior when click samples missing

## Traceability (Non-Normative)
- core/src/PluginController.cpp (click generation + settings)
- docs/user-interface.md (metronome messages)
