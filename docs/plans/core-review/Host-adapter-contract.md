# Host — Plugin Framework Adapter Contract

## Intent
Define the **minimum host capabilities** required to embed and run Core inside a plugin framework (DAW plugin) or standalone app.

This is the abstraction boundary that makes Core portable across frameworks.

## Actors
- Host adapter (framework-specific glue)
- Core controller/engine
- Host environment (DAW or OS)

## Requirements
### H1 — Audio callback integration
- Host must provide block-based processing callbacks.
- Host must provide current sample rate and max block size.
- Host must supply audio buffers in a consistent channel ordering.

### H2 — Lifecycle
Host must call into Core for:
- Initialize (once per instance)
- Prepare (on sample rate / block size change)
- Reset (on transport resets)
- Idle tick (periodic, non-audio thread)

### H3 — Thread dispatch
- Host must provide a way for Core to schedule work on the main/UI thread safely from other threads.

### H4 — UI embedding (optional but standard)
If the UI is embedded:
- Host must provide a sandboxed UI runtime container.
- Host must implement a bidirectional message bridge:
  - Core → UI: send string messages
  - UI → Core: deliver messages to Core dispatcher
- Host must notify Core when UI content is loaded/ready.

If UI is not embedded:
- Host must still implement equivalent command/state flows (C1) using an alternative UI technology.

### H5 — File dialogs and filesystem access
- Host must provide asynchronous open/save dialogs by file category.
- Completion callbacks must execute on the main/UI thread.

### H6 — Storage paths
Host must provide:
- A user data root for Core-managed storage (presets, libraries, cache, settings)
- A bundled assets root for factory/read-only content

### H7 — Parameter & automation bridge
- Host must expose a stable parameter set for DAW automation.
- Host must notify Core on parameter changes.
- Host should support “state dirty” notification.

### H8 — State save/restore
- Host must store a Core-provided serialized state blob.
- Host must restore it reliably and deterministically.

### H9 — Host playback context (optional)
If available, host should provide:
- Transport state (playing)
- Tempo (BPM)

### H10 — Latency reporting (if applicable)
If Core introduces latency:
- Host should support reporting it to the environment for compensation.

### H11 — Observability
- Host should provide a logging sink.
- Host must surface Core errors to the UI in a user-visible way.

## Failure Modes
- If the host cannot provide a capability (e.g., tempo), Core must fall back to safe defaults.
- If message bridge fails, Core must remain stable and not block audio.

## Traceability (Non-Normative)
- core/src/IPluginHost.h
- core/src/PluginController.h
- docs/user-interface.md
