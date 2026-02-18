# Riff Library Plan

## Status
- Draft
- Last updated: 2026-02-18

## Assumptions
- Riffs are captured as dry DI audio and monitored through the active signal chain for player feedback.
- Drum patterns are sourced from MIDI assets in a local library and rendered by an internal pattern player.
- Library storage is local-first, user-configurable, and file-based.
- Existing UI shell/components and messaging patterns will be reused.

## Problem Statement
Users need a fast way to capture riff ideas at a known musical grid (tempo + time signature), keep them organized, and reuse them later for composition and tone auditioning.

## Goals
- Enable quick riff capture against click or MIDI drum patterns.
- Save clean, measure-aligned WAV clips for easy DAW copy/paste.
- Persist playback and tone context metadata (tempo, time signature, pattern, last-used preset/signal chain).
- Provide local library organization with categories/tags, search, favorites, and optional usage tracking.
- Reuse recording UI/infrastructure for a future looper feature.

## Non-Goals (Initial Release)
- Multi-user/cloud sync.
- Full DAW timeline editor.
- Real-time quantization/time-warp of recorded audio.
- MIDI riff recording/export (audio-first scope).

## User Stories
1. As a guitarist, I can choose click or a MIDI drum pattern, set tempo/time signature/bars, and record a riff with count-in.
2. As a guitarist, I receive a WAV that is clipped to exact measure boundaries so I can drag it into a DAW quickly.
3. As a guitarist, I can keep multiple takes under one riff idea and choose a preferred take.
4. As a guitarist, I can save riffs in a configurable local library path and find them later by search and categories/tags.
5. As a songwriter, I can mark riffs as used and optionally note the song title.
6. As a tone designer, I can favorite riffs and use favorites in Demo Audio to audition presets/signal chains consistently.
7. As a future looper user, the same transport/recording components can be reused to capture and loop short sections.

## Functional Requirements

### FR-RL-01 Recording Setup
- User can select:
  - Transport mode: click or MIDI drum pattern.
  - Tempo (BPM), time signature, bar length, and count-in.
  - Input source (default guitar DI).
- UI validates required fields before arming record.

### FR-RL-02 MIDI Drum Pattern Playback
- Pattern browser lists available MIDI drum patterns from library.
- Pattern preview/playback follows transport tempo and time signature rules.
- If a pattern is incompatible with selected signature, UI prevents invalid selection or clearly explains conversion behavior.

### FR-RL-03 Measure-Aligned Audio Capture
- Recording starts/stops on bar boundaries when quantized mode is enabled.
- Output is saved as WAV, clipped to configured measure range.
- Apply short edge fades to prevent boundary clicks.

### FR-RL-04 Metadata Persistence
- Save per-take metadata:
  - tempo, time signature, bar count, sample rate/bit depth
  - transport source type (click/drum), pattern ID/version
  - last-used preset ID/name and signal-chain snapshot reference/hash
  - timestamps and user notes

### FR-RL-05 Takes
- A riff entry can contain multiple takes.
- User can audition takes, rename, delete, and mark one as preferred.
- Preferred take is the default for Demo Audio and exports.

### FR-RL-06 Library Management
- User can set and change the Riff Library path.
- Riffs can be categorized, tagged, searched, and sorted.
- Missing/moved files surface non-blocking warnings with repair options.

### FR-RL-07 Usage Tracking
- User can mark/unmark riff as used.
- Optional song title and usage date are stored.

### FR-RL-08 Favorites + Demo Audio
- User can favorite riffs/takes.
- Demo Audio feature can filter/select favorite riffs as source material.

### FR-RL-09 Looper-Reuse Compatibility
- Recording/transport engine exposes reusable interfaces for:
  - grid clock (tempo/time signature/bar position)
  - bounded capture (N bars)
  - loopable playback segment definition
- Riff Library and future Looper share core primitives, differing mainly in UX and persistence behavior.

## Data Model (Proposed)

### Riff
- id
- title
- categories[]
- tags[]
- notes
- favorite (bool)
- used (bool)
- usedSongTitle (optional)
- usedAt (optional)
- preferredTakeId
- createdAt / updatedAt

### RiffTake
- id
- riffId
- audioFilePath
- durationSec
- bars
- tempoBpm
- timeSigNum / timeSigDen
- transportType (click | drum)
- drumPatternId (optional)
- drumPatternVersion (optional)
- presetId / presetName (optional)
- chainSnapshotHash (optional)
- sampleRate / bitDepth
- inputLatencyCompSamples
- createdAt

### DrumPattern
- id
- name
- midiSourceRef
- supportedTimeSignatures[]
- defaultTempoRange
- tags[]

## UX Flow (MVP)
1. Open Riff Library panel.
2. Select click or drum pattern.
3. Set tempo/time signature/bars/count-in.
4. Arm + record.
5. Review take, name riff, assign categories/tags, save.
6. Optionally mark as favorite or used with song title.

## Architecture Notes

### Shared Recording Core (Riff + Future Looper)
- `GridClockService`: canonical transport clock/state.
- `PatternPlaybackService`: click + MIDI pattern scheduling.
- `BoundedRecorderService`: deterministic bar-bounded capture.
- `ClipExportService`: edge-faded WAV export + metadata write.

### Library Layer
- `RiffLibraryStore`: index and query riffs/takes metadata.
- `RiffFileRepository`: handles file layout and path migration.
- `RiffSearchIndex`: text + structured filters (tempo, signature, tags, favorite, used).

### UI Layer
- Reuse existing list/detail controls and recording widgets.
- Add takes strip/list in riff details.
- Add Demo Audio picker integration path for favorites.

## Acceptance Criteria
- Recording against click or selected MIDI drum pattern succeeds without audio-thread glitches under normal load.
- Saved WAV length matches configured bars at selected tempo/time signature within tolerance.
- Metadata includes tempo, signature, transport source, pattern reference, and last-used preset context.
- Multiple takes can be created and managed under a single riff entry.
- Users can set a library path, search/filter riffs, and mark favorite/used with optional song title.
- Demo Audio can consume favorite riffs as audition source.
- Shared recording services are structured so looper feature can consume them without duplicating transport/capture logic.

## Risks and Mitigations
- Latency misalignment: include input-latency compensation and verification tests.
- Pattern-signature mismatch: explicit compatibility rules and UI guards.
- File churn/broken refs: stable IDs + repair workflow for moved assets.
- Scope creep: keep MVP audio-first and local-first.

## Delivery Phases

### Phase 1 (MVP)
- Click + MIDI drum pattern selection.
- Bar-bounded recording and clipped WAV export.
- Basic metadata persistence.
- Library path config + create/list/search by text.

### Phase 2
- Takes management (preferred take, rename/delete/audition).
- Categories/tags/favorite/used workflows.
- Demo Audio favorites integration.

### Phase 3
- Robust indexing/repair tools for moved files.
- Advanced filters (tempo range, time signature, pattern).
- Looper spike reusing shared recording core APIs.

## Implementation Task Breakdown

### Core/DSP
- Define `GridClockService` and transport state model.
- Implement MIDI pattern scheduling with click fallback.
- Implement bar-bounded DI capture with latency compensation.
- Add deterministic clip/export path and boundary fade handling.

### Data/Storage
- Define riff and take schemas (JSON sidecar or indexed store + sidecar refs).
- Implement library-path configuration and migration-safe file layout.
- Implement metadata index + query API.

### UI
- Build recording setup panel (tempo/signature/bars/source/count-in).
- Add pattern browser and compatibility messaging.
- Add riff list/detail with takes management, favorite, used marker, song title note.
- Add Demo Audio selection using favorite riffs.

### Integration
- Add UI ↔ plugin messages for transport/record/save/manage actions.
- Keep message payloads backward compatible and guard missing fields.
- Add feature flags for incremental rollout if needed.

### Test Plan
- Unit: bar math, clip boundaries, metadata serialization, take preference rules.
- Integration: record with click/pattern, save/load metadata, library indexing and search.
- Regression: Demo Audio source selection from favorites.
- Performance: verify no RT allocations/locks in audio path and no dropouts at target settings.

## Open Decisions
- Metadata persistence format: sidecar JSON only vs lightweight DB index.
- Pattern tempo handling: strict compatibility vs controlled stretching rules.
- Save policy for canceled/partial captures.
- Whether looper initial release should read from Riff Library entries directly.
