# D3 — Signal Path Test

## Intent
Provide a built-in diagnostic that verifies the engine’s processing path is producing measurable output, and reports numeric results to the UI.

## Actors
- End user (runs test)
- UI client (initiates test, displays results)
- Engine (injects stimulus and measures response)

## Inputs
- Command to start a test
- Optional parameters:
  - Test frequency (Hz)
  - Duration (seconds)

## Outputs
- A test result event containing:
  - Sample rate
  - Frequency
  - Duration
  - Elapsed wall-clock time
  - Input RMS
  - Output RMS (per channel)
  - Pass/fail boolean

## Behavioral Requirements
### R1 — Stimulus injection
- When the test is active, the engine must inject a deterministic stimulus into the signal path.
- Default stimulus: sine wave at 440 Hz for 1 second.

### R2 — Measurement
- The engine must accumulate input and output RMS over the test duration.
- Results must be computed deterministically from the accumulated sums.

### R3 — Result emission
- On completion, a single structured result event must be sent to the UI.

### R4 — Pass criteria
- Pass/fail must be derived from measurable output exceeding a defined threshold.
- Threshold must be documented and stable.

### R5 — Exclusivity with preview playback
- If demo/preview audio playback exists, the system must define interaction rules.
- At minimum: preview must not start while signal path test is active (or vice versa), and the user must receive a clear error.

### R6 — Real-time safety
- The test must not block or allocate unbounded memory in the audio callback.

## Error Handling
- If sample rate is invalid, test must fail to start cleanly and report an error.

## Test Strategy
- Deterministic stimulus tests
- Result payload shape tests
- Exclusivity tests with preview feature

## Traceability (Non-Normative)
- core/src/PluginController.cpp (stimulus injection + RMS accumulation + result message)
- docs/user-interface.md (signalPathTestResult message)
- core/src/controller/DemoPreviewService.cpp (preview/test exclusivity)
