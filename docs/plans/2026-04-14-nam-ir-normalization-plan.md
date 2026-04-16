# NAM/IR Normalization And Input Calibration Plan

## Overview

This document defines the implementation plan for making model and IR switching more level-consistent without changing a model's intended input-drive behavior.

The core design decision is to separate two concerns that were previously mixed together:

- User-specific input calibration: a fixed pre-model trim that reflects the user's interface and instrument setup.
- Resource-specific normalization: a product-owned post-model or post-IR compensation that makes file switching more predictable.

## Remaining Tasks Only

Use this as the active checklist. The detailed sections below preserve the original design rationale and phased plan, but the items here are what still remain after the runtime DSP, preset/settings migration, IR defaulting, and user input calibration UI work landed.

### Completed Runtime Slice

- Output-only NAM normalization is implemented in the runtime paths.
- Baseline IR auto compensation is enabled.
- The old interface calibration behavior has been replaced by global user input calibration profiles.
- The input level training modal and profile save/select/delete flow are implemented.
- Preset load/save migration no longer revives retired NAM input auto-level calibration behavior.
- Regression coverage exists for the new runtime paths and fast tests are green.

### Remaining Checklist

- [ ] Define and freeze the offline reference DI phrase set used for NAM normalization analysis.
- [ ] Choose and document the final NAM normalization metric and acceptable user calibration operating range.
- [ ] Version the offline analysis method explicitly and define how version changes invalidate old measurements.
- [ ] Implement the resource analysis pipeline that computes product-owned `normalizationGainDb` for NAM resources.
- [ ] Store resource-level analysis metadata for shipped/user resources, including at least `normalizationGainDb`, `analysisVersion`, `analysisSource`, and `resourceHash`.
- [ ] Decide and implement the production storage/update path for that metadata during import, indexing, or library maintenance.
- [ ] Backfill the shipped NAM and IR libraries with measured normalization data before broad rollout.
- [ ] Surface analysis coverage so missing measurements are visible in tooling instead of silently relying on fallbacks.
- [ ] Run corpus-level validation against the Tone3000 NAM and IR libraries using the actual measured-data pipeline.
- [ ] Validate representative user calibration scenarios across different guitars, pickups, interfaces, and interface gain positions.
- [x] Update product/docs language anywhere it still implies legacy interface calibration semantics instead of named user input profiles plus resource normalization.

## Assumptions

- We want model switching to preserve designer-intended response as much as possible.
- We do not want runtime per-model input auto-leveling derived from third-party metadata.
- We do want product-owned output compensation for NAMs so different captures land closer in perceived level.
- We do want IR switching to be level-stable by default.
- We can tolerate an offline analysis step for NAM resources if it produces deterministic, versioned normalization data.
- User-to-user consistency is a separate problem from file-to-file consistency and must be handled with a global input calibration flow.

## Goals

- Remove runtime NAM input normalization tied to model metadata or cached per-model calibration values.
- Introduce output-only NAM normalization based on Soundshed-owned measurements.
- Make IR energy compensation baseline behavior for IR switching.
- Add a user input calibration workflow that improves predictability across interfaces without changing per-model behavior.
- Migrate existing presets and settings without breaking preset loading.

## Non-Goals

- Perfectly identical feel across all users regardless of interface gain staging, pickups, or playing dynamics.
- Loudness normalization based purely on vendor-supplied `output_level_dbu` metadata.
- Reinstating the old preset-scoped `calibrationInputLevel` override path.
- Real-time adaptive auto gain that changes while the user is playing.

## Current State Summary

### NAM Processing

- NAM runtime behavior is now centered on explicit user gain plus output-side normalization.
- Resource-owned `normalizationGainDb` is preferred when present; model loudness metadata is the fallback.
- `autoLevelInput`, `calibrationInputLevel`, and `calibrationOutputLevel` remain in compatibility paths but are sanitized out of normal preset loads.
- NAM level-state reset now forces `autoLevelInput` off and keeps `autoLevelOutput` on.
- User input calibration is applied once globally before the chain rather than as a live NAM-specific interface calibration step.

### IR Processing

- The IR cabinet effect already computes an energy-based auto compensation gain.
- That compensation is baseline behavior for normal IR switching.

### Resource Storage

- ResourceLibrary already supports arbitrary metadata per resource.
- Old model-hash plumbing exists and can be reused to key measured normalization data.

## Design Decisions

### 1. Separate User Calibration From Resource Normalization

User calibration and file normalization solve different problems and should not share the same parameters.

- User calibration:
  - Fixed, global, and stable across model switches.
  - Applied once before the first NAM stage or in a dedicated pre-chain stage.
  - Represents the user's interface and instrument profile.

- Resource normalization:
  - Computed offline by Soundshed.
  - Applied after the model or after IR processing.
  - Represents the file's measured output behavior, not the user's setup.

### 2. Do Not Restore Per-Model Input Calibration

The old per-model calibration concept should not return in its original form.

- `calibrationInputLevel` should not be used as a persistent model-level override.
- Runtime input behavior should be driven only by:
  - user input trim/profile,
  - explicit user controls,
  - any intentional preset gain staging.

### 3. Reinstate Offline NAM Analysis Only For Output Compensation

Offline analysis is still useful, but only for post-model normalization.

- Measure each NAM against a fixed reference DI phrase set.
- Compute a `normalizationGainDb` value owned by Soundshed.
- Apply that gain after the model output, not before the model input.
- Version the analysis so the normalization can be recomputed later if the method changes.

### 4. Make IR Auto Compensation Baseline

IR switching is a linear, lower-risk case.

- Use the existing IR energy compensation path as default behavior.
- Keep any manual output controls for user preference, but do not require users to enable IR leveling themselves.

## Implementation Plan

### Phase 1: Define Reference Targets And Metrics

**Outcome:** one agreed reference for both offline NAM analysis and user input calibration.

1. Define the reference DI phrase set used for offline NAM measurements.
2. Choose the output metric for NAM normalization.
3. Define the acceptable operating range for user input calibration.
4. Version the analysis method so future changes do not silently invalidate old data.

**Recommended approach:**

- Use a short phrase set rather than a single sine wave.
- Include at least:
  - single notes,
  - open chords,
  - palm-muted attacks,
  - a stronger transient segment.
- Compute one robust output normalization value from the rendered output of that phrase set.

### Phase 2: Retire Runtime NAM Input Auto-Leveling

**Outcome:** model switching no longer changes drive based on model metadata or old calibration overrides.

1. Remove or disable metadata-driven NAM input auto gain in:
   - `OptimizedNAMAmpEffect`
   - `NAMAmpEffect`
   - `MultiModelNAMAmpEffect`
2. Stop defaulting `autoLevelInput` to enabled when NAM state is reset.
3. Treat `calibrationInputLevel` as legacy and phase it out from active behavior.
4. Keep backward-compatible preset loading, but ignore legacy input calibration for new behavior.

**Migration note:**

- Old presets may still carry `autoLevelInput`, `calibrationInputLevel`, or `modelHash` remnants.
- These should be sanitized on load and removed from newly saved presets.

### Phase 3: Add Resource-Level Normalization Data

**Outcome:** product-owned normalization data exists independently of presets.

1. Add normalization metadata for NAM and IR resources.
2. Store at least:
   - `normalizationGainDb`
   - `analysisVersion`
   - `analysisSource`
   - `resourceHash`
3. Decide storage location:
   - ResourceLibrary metadata, or
   - a separate analysis cache keyed by resource hash.
4. Reuse model hashing to invalidate stale analysis when the underlying file changes.

**Recommendation:**

- Prefer resource-level storage over preset-level storage.
- Presets should reference resources, not duplicate analysis outcomes.

### Phase 4: Implement Output-Only NAM Compensation

**Outcome:** model switching becomes level-consistent without changing model drive.

1. Add a dedicated post-model normalization stage in NAM effects.
2. Load Soundshed-owned normalization data when a NAM resource is loaded.
3. Apply normalization after the model output and before final user output gain.
4. For multi-model blends, blend normalization values deterministically with the same selection weights used for model blending.
5. Define fallback behavior when normalization data is missing.

**Fallback order recommendation:**

1. Measured `normalizationGainDb`
2. Measured loudness metadata if trusted and present
3. Existing model metadata loudness fallback
4. Unity gain

**Important:**

- Do not use `output_level_dbu` as the primary normalization source.
- It is not reliable enough across the observed Tone3000 NAM corpus.

### Phase 5: Enable Baseline IR Compensation

**Outcome:** IR switching is level-stable by default.

1. Make IR auto compensation effectively always on for resource switching.
2. Keep user-facing output controls independent of the automatic IR normalization.
3. Validate slot A/B blend behavior and polarity handling with the new default.
4. Confirm transitions between IRs remain smooth during resource crossfades.

### Phase 6: Add User Input Calibration Flow

**Outcome:** different users get more predictable NAM behavior without per-model input overrides.

1. Replace the current interface calibration meaning.
2. Add a guided setup flow that measures the user's raw DI level.
3. Store the result as a fixed input profile, not as model-specific data.
4. Support multiple user profiles if needed.
5. Apply the calibrated trim once before the first NAM stage.

**User flow recommendation:**

1. User selects input device/channel.
2. User plays a short guided phrase or repeated strong strums.
3. App measures peak and average input level.
4. App recommends a fixed trim offset.
5. User saves that as a named input profile.

### Phase 7: Preset And Settings Migration

**Outcome:** old data loads safely and new data saves cleanly.

1. Strip or ignore retired NAM calibration fields during preset normalization.
2. Stop reintroducing legacy auto-level fields when presets are loaded or resaved.
3. Update settings semantics so interface calibration means user input profile calibration, not per-model metadata compensation.
4. Ensure old presets still load with predictable behavior.

**Legacy fields to review:**

- `autoLevelInput`
- `calibrationInputLevel`
- `calibrationOutputLevel`
- `modelHash`

### Phase 8: Validation And Rollout

**Outcome:** behavior is verified before wider adoption.

1. Add unit tests for NAM output normalization selection and fallback behavior.
2. Add tests for multi-model blend normalization.
3. Add IR tests covering default auto compensation and blend behavior.
4. Add migration tests for old presets carrying legacy NAM calibration state.
5. Run corpus-level validation against Tone3000 NAM and IR libraries.
6. Validate representative user calibration scenarios with different interfaces and guitars.

## File Areas Expected To Change

### DSP

- `core/src/dsp/effects/OptimizedNAMAmpEffect.h`
- `core/src/dsp/effects/NAMAmpEffect.h`
- `core/src/dsp/effects/MultiModelNAMAmpEffect.h`
- `core/src/dsp/effects/IRCabEffect.h`
- `core/src/dsp/MultiPresetMixer.h`
- `core/src/dsp/MultiPresetMixer.cpp`

### Controller And Preset Migration

- `core/src/PluginController.cpp`
- `core/src/PluginController.h`
- preset normalization / serialization paths

### Resource Metadata

- `core/src/resources/ResourceLibrary.h`
- `core/src/resources/ResourceLibrary.cpp`
- any resource indexing or import pipeline code

### UI And Settings

- `core/ui/ts/settings.ts`
- settings UI markup and labels
- any input calibration wizard UI

### Tests

- `core/tests/PresetDSPLoadingTests.cpp`
- `core/tests/PresetDSPProcessingTests.cpp`
- `core/tests/SignalGraphExecutorTests.cpp`
- `core/tests/IRConvolutionTests.cpp`
- targeted NAM regression coverage

## Risks And Mitigations

### Risk: Output Matching Hides Real Drive Differences

If user input level varies significantly, output-only normalization can make two setups equally loud while they still feel different.

**Mitigation:**

- Add a separate user input calibration workflow.
- Clearly separate input profile calibration from file normalization.

### Risk: Old Presets Reintroduce Retired Gain-Staging Fields

Legacy fields may be restored during load/save flows.

**Mitigation:**

- Centralize preset normalization.
- Add migration tests that verify retired fields stay retired.

### Risk: Offline Analysis Becomes Another Hidden Auto-Level System

If analysis data is stored at the preset level or applied before the model, the old behavior may effectively return under a new name.

**Mitigation:**

- Store normalization at the resource level.
- Apply it post-model only.
- Document the distinction clearly.

### Risk: Missing Analysis Data Produces Inconsistent Fallbacks

Some resources may not have measured normalization data initially.

**Mitigation:**

- Implement a deterministic fallback order.
- Surface analysis coverage in tooling.
- Backfill the shipped library before enabling the feature broadly.

## Recommended Delivery Order

1. Define the reference phrases and normalization metric.
2. Remove NAM input auto-leveling behavior.
3. Add resource-level normalization data.
4. Implement output-only NAM compensation.
5. Turn on baseline IR compensation.
6. Add the user input calibration flow.
7. Migrate presets and settings.
8. Validate with tests and corpus scans.

## Success Criteria

- Switching NAM files no longer causes large loudness jumps.
- Switching IRs is level-stable by default.
- The same model no longer receives different drive solely because a different file was loaded.
- User-to-user predictability improves through explicit input calibration rather than hidden model-level input scaling.
- Old presets continue to load without reviving retired calibration behavior.
