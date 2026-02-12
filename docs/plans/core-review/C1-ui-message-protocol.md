# C1 — UI ↔ Engine Message Protocol

## Intent
Enable a UI client (web-based or native) to control and observe Core via a **bidirectional message protocol**.

The protocol must be:
- Stable and version-tolerant
- Structured (e.g., JSON or equivalent)
- Safe to process with input validation

## Actors
- UI client
- Engine/controller
- Host adapter (bridges UI runtime to Core)

## Message Envelope (Abstract)
Each message must include:
- `type`: message type identifier
- `payload`: structured object (may be empty)
- Optional metadata: timestamp, correlation ID, request ID

## Behavioral Requirements
### R1 — Startup sync
- UI must be able to request a full state snapshot.
- Engine must respond with a complete snapshot sufficient to render the UI.

### R2 — Engine authoritative state
- If UI and engine disagree, engine state is authoritative.
- UI must converge to engine state after receiving updates.

### R3 — Command handling
The engine must handle commands for:
- Parameter changes
- Preset load/save/list/delete
- Resource browsing and assignment
- Graph editing (add/remove/replace/reorder, bypass, param update)
- Mixer controls (add/remove active preset, mix/pan/mute/solo)
- Utility features (tuner, metronome, signal test)

### R4 — Event notifications
The engine must publish events for:
- State snapshot updates
- Preset loaded/saved
- Errors
- Diagnostics updates (levels, tuner updates, performance stats)

### R5 — Validation and hardening
- Messages must be validated:
  - Unknown message types must be safely ignored (and optionally logged).
  - Missing required fields must produce a structured error response/event.
  - Payload size and nesting depth must be bounded.

### R6 — Threading constraints
- Message dispatch must occur on a non-audio thread.
- UI-originating commands that trigger heavy work must run asynchronously.

## Error Handling
- Engine must send structured error events with at least:
  - `message` (user-facing)
  - `detail` (optional diagnostic)

## Compatibility
- Message types and payload fields should be additive.
- Removing fields/types requires a deprecation strategy.

## Test Strategy
- Contract tests for message parsing/validation
- Golden tests for state snapshots
- Fuzz tests for malformed input

## Traceability (Non-Normative)
- docs/user-interface.md
- core/src/MessageDispatcher.h
- core/src/PluginController.h
