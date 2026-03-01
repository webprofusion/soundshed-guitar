# Core Review Plan (Implementation-Agnostic)

> **Status: IMPLEMENTED.** All 13 spec files have been created under `docs/plans/core-review/`: `00-index.md`, `A2` through `D3`, and `Host-adapter-contract.md`. The feature catalog and per-feature requirements are complete.

## Goal
Document the **high-level features** of “Core” and the **requirements per feature** in a way that is:
- **Independent of specific frameworks/libraries/languages**
- Specific enough that the system could be **recreated in the future** with the same externally observable behavior
- Traceable back to today’s implementation (as **non-normative** references)

This plan is about *what the system must do* (capabilities + constraints), not *how it does it*.

## Scope
**In scope**
- Audio engine behavior (signal processing graph, effect model, resource-backed effects)
- Presets, libraries, and storage model
- UI ↔ engine protocol and state synchronization (abstract contract)
- Diagnostics, metering, and utility features (tuner, metronome, signal tests)
- Extensibility mechanisms (new effects, composite effects, layouts)
- Remote services integration (optional; feature-gated)
- Requirements a plugin framework/host must satisfy to utilize Core

**Out of scope**
- Specific framework APIs, GUI toolkit details, build systems
- UI visual design and exact layout/controls (except where it affects behavior)
- Vendor- or platform-specific packaging and signing requirements

## Deliverables
1. **Feature Catalog**: a list of Core features, grouped by domain.
2. **Per-Feature Requirements**: each feature captured using the template below.
3. **Adapter Contract**: what an external “plugin framework host” must provide so Core can run.
4. **Traceability Map** (non-normative): pointers to current docs/code for verification.

## Feature Specs (Normative)
The plan becomes concrete via per-feature spec files in `docs/plans/core-review/`:
- `docs/plans/core-review/00-index.md`
- `docs/plans/core-review/A2-signal-graph-dag.md`
- `docs/plans/core-review/A3-effect-catalog.md`
- `docs/plans/core-review/A4-resource-backed-processing.md`
- `docs/plans/core-review/A5-multi-preset-mixer.md`
- `docs/plans/core-review/B1-preset-data-model.md`
- `docs/plans/core-review/B2-preset-storage.md`
- `docs/plans/core-review/B3-resource-library.md`
- `docs/plans/core-review/C1-ui-message-protocol.md`
- `docs/plans/core-review/D1-tuner.md`
- `docs/plans/core-review/D2-metronome.md`
- `docs/plans/core-review/D3-signal-path-test.md`
- `docs/plans/core-review/Host-adapter-contract.md`

## Process Overview
### Phase 1 — Inventory (What exists)
- Build the feature catalog from:
  - Existing docs (architecture, models, signal chain, UI messaging)
  - Public interfaces (adapter contract) and message types
  - Smoke-testing flows (load preset, change params, run diagnostics, etc.)

### Phase 2 — Normalize (Define stable feature boundaries)
- Merge duplicates (“two names for same capability”).
- Split “mega features” into testable units.
- Define explicit feature boundaries and dependencies.

### Phase 3 — Specify (Per feature requirements)
- For each feature, fill the template:
  - Inputs/outputs, states, error modes
  - Performance/threading constraints
  - Persistence and backward compatibility

### Phase 4 — Validate (Reality check)
- Verify each spec is:
  - Implementable without referencing the current stack
  - Testable via observable behavior
  - Consistent with adjacent features

### Phase 5 — Traceability + Gaps
- Link each requirement set back to current code/docs.
- Record unknowns and gaps as explicit follow-ups.

---

## Feature Catalog (Initial)
This is the starting inventory; refine during Phase 2.

### A. Audio Engine
A1. **Real-time audio processing pipeline**
- Processes blocks of audio deterministically with real-time safety constraints.

A2. **Signal processing graph (DAG)**
- Configurable directed acyclic graph of nodes/edges.
- Supports linear chains and parallel paths (split/mix).

A3. **Effect model and effect catalog**
- Discoverable set of effect types with parameter definitions and categories.
- Supports runtime instantiation by type identifier.

A4. **Resource-backed processing**
- Certain effects depend on external resources (e.g., models/impulses).
- Resources resolved from a library and/or embedded payloads.

A5. **Multi-preset parallel mixing**
- Run multiple preset graphs concurrently and mix outputs with per-preset gain/pan/mute/solo.

A6. **Global processing controls**
- Input/output trims and master gain.
- Optional limiter.
- Optional global pre/post chains that wrap presets.

### B. Presets, Libraries, and Persistence
B1. **Preset data model**
- Versioned preset schema with global settings + signal graph.

B2. **Preset storage**
- Load/save/delete/list.
- Import/export, including portable presets with embedded resources.

B3. **Resource library**
- Catalog of reusable resources (types + metadata).
- Content-addressed deduplication.

B4. **Composite effects library**
- Reusable bundles: a composite acts like a single effect but runs an internal graph.
- Composite definitions stored and discoverable.

B5. **Layout library (optional)**
- Saved layouts for effect/composite control surfaces.

B6. **App settings + last session state**
- Persisted non-audio settings (UI, diagnostics toggles, folders, etc.).

### C. UI / Interaction (Abstract)
C1. **Bidirectional message protocol**
- Message envelope (`type`, `payload`, metadata).
- Engine can publish state snapshots and event notifications.

C2. **State synchronization**
- Initial full sync on startup.
- Incremental updates on changes.
- Conflict resolution rule: engine authoritative.

C3. **Signal graph editing**
- Add/remove/replace/reorder nodes; connect/disconnect/split/collapse.
- Update node params, bypass, and resources.

C4. **Effect catalog browsing**
- UI can request available effect types and metadata.

### D. Diagnostics & Utilities
D1. **Tuner**
- Start/stop; periodic pitch updates (note, cents, confidence).

D2. **Metronome**
- Configurable tempo; can follow host tempo when available.
- Generates click audio mixed into output.

D3. **Signal path test**
- Built-in diagnostic routine producing measurable test results (RMS, pass/fail, timing).

D4. **Signal level meters / diagnostics (optional)**
- Periodic input/output level reporting.

D5. **DSP performance stats (optional)**
- Periodic CPU/latency/load metrics.

D6. **Demo/preview playback (optional)**
- Preview audio assets and notify UI of start/stop/complete.

### E. Remote Services (Optional)
E1. **Remote preset search/download**
- Search query, paging, sorting.

E2. **Remote resource download/import**
- Download binary resources with integrity checks.
- Offline mode behavior.

### F. Cross-Cutting Behaviors
F1. **Threading model**
- Audio thread: real-time safe, no blocking.
- UI/background: async tasks, main-thread callbacks.

F2. **Error handling strategy**
- Audio path: fail-soft (bypass/silence) and signal UI.
- Non-audio: detailed errors and user-visible messages.

F3. **Backward compatibility**
- Schema versioning, migrations, and defaults.

F4. **Security boundaries**
- UI sandbox boundary; messages validated.
- Remote downloads validated (hash/size/type).

---

## Per-Feature Requirements Template
Use this template for each feature (A1, A2, …). Keep requirements **observable** and **testable**.

### Feature: <ID + Name>
**Intent**
- What problem it solves / why it exists.

**Users / Actors**
- End user, host DAW, UI, background worker, remote service.

**Inputs**
- External inputs (audio buffers, UI messages, files, network responses).

**Outputs**
- Audio output, state changes, stored files, UI notifications.

**State & Persistence**
- What state exists, where it lives (conceptually), and when it is saved.

**Public Contract**
- Message types / commands and expected payload shapes (conceptually).
- Data model requirements (fields + constraints) without binding to JSON/C++.

**Behavioral Requirements**
- “Must/Should/May” statements.
- Edge cases and default behaviors.

**Error Handling**
- Error categories and user-visible reporting requirements.
- Recovery behavior and fallback modes.

**Performance & Real-Time Constraints**
- Time/latency, allocations, locking, determinism.

**Threading / Concurrency**
- Which operations are allowed on which threads.

**Compatibility**
- Versioning policy, migrations, tolerance to unknown fields.

**Test Strategy**
- Unit tests / integration tests / golden audio tests / property tests.

**Non-Goals**
- Explicitly list what is not required.

---

## Plugin Framework / Host Requirements (Adapter Contract)
This section describes what any plugin framework (or standalone host) must provide to utilize Core.

### 1) Lifecycle Integration
The host must provide lifecycle calls:
- **Initialize**: after the host is ready (paths, UI container, sample rate known or queryable).
- **Prepare(sampleRate, blockSize)**: on playback start and when sample rate or block size changes.
- **Reset**: on transport reset or playback start.
- **OnIdle / periodic tick**: called regularly on a non-audio thread to process deferred work.

### 2) Audio Processing Contract
The host must:
- Provide **block-based audio processing** callbacks.
- Pass audio as **stereo float buffers** (or an equivalent representation) with deterministic ordering.
- When Core indicates it cannot safely process (e.g., contention), host must have a defined fallback:
  - Output silence, or bypass last known safe output, per the Core contract.

### 3) Parameter & Automation Bridging
To support host automation and UI control:
- Host must expose a set of **automatable parameters** with stable identifiers.
- Host must notify Core on parameter changes (from automation or UI).
- Host must allow Core to query current parameter values for state sync.
- Host must support **state dirty notifications** so DAW projects can be marked modified.

### 4) State Save/Restore
The host must:
- Provide persistence hooks to **serialize Core state** into a host-owned blob.
- Restore Core state deterministically on load.
- Handle versioned state payloads (forward/backward compatibility rules defined by Core specs).

### 5) Embedded UI Hosting + Message Bridge
If using the web-based UI approach (recommended but not required):
- Host must embed an **HTML/JS UI runtime** (or equivalent) and enforce a sandbox boundary.
- Host must provide a **bidirectional message transport**:
  - Core → UI: send string messages.
  - UI → Core: receive messages and forward to Core’s dispatcher.
- Host must notify Core when UI content has loaded so Core can perform initial state sync.

If not using a web UI:
- The host must still implement equivalent command/state flows (same logical protocol).

### 6) File Dialogs & User File Access
The host must provide async file operations:
- Browse/open file (by category: model, impulse, preset, etc.).
- Save file.
- Completion callbacks must execute on the **main/UI thread**.

### 7) Platform Paths & Storage
The host must provide:
- A stable **user data root** for Core-managed storage (presets, libraries, cache, settings).
- A stable **bundled assets root** for read-only factory content.

### 8) Threading & Main-Thread Dispatch
The host must provide:
- A way for Core to schedule work on the main/UI thread safely from other threads.
- A clear rule for what runs on audio vs UI vs background threads.

### 9) Host Environment Info (Optional)
If available, the host should provide:
- Transport state (playing/not).
- Tempo (BPM).
- Standalone-only features (open audio/MIDI preferences).

### 10) Latency Reporting (If applicable)
If Core can introduce latency (e.g., convolution):
- Host should support reporting latency to the plugin environment so delay compensation can work.

### 11) Observability
The host should provide:
- Logging sink or a mechanism for Core to emit user-supportable diagnostic logs.
- A way to surface errors to the UI.

---

## Traceability (Non-Normative)
These pointers are for verification only; do not treat them as requirements.

### Current documentation
- core/README.md
- core/INTEGRATION.md
- docs/architecture-overview.md
- docs/data-models.md
- docs/signal-chain.md
- docs/fx-library.md
- docs/user-interface.md
- docs/network-api.md
- docs/composite-effects.md

### Current interfaces (adapter + controller)
- core/src/IPluginHost.h
- core/src/PluginController.h
- core/src/MessageDispatcher.h
- core/src/UiBridge.h

---

## Next Steps Checklist
- [ ] Confirm feature catalog completeness (add/remove items, refine grouping)
- [ ] Pick the first 5 features to fully specify (suggestion: A2, A3, B1, B2, C1)
- [ ] Write one feature spec per file under docs/plans/core-review/ (optional structure)
- [ ] Add acceptance tests list for each spec (what to measure / what outputs prove compliance)
