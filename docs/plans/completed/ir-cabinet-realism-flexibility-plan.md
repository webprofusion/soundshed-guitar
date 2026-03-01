# IR Cabinet Realism & Flexibility Plan

> **Status: PHASE 1 IMPLEMENTED.** The dual-IR blend, per-slot controls, crossfade switching, and auto gain compensation are in `core/src/dsp/effects/IRCabEffect.h`. Additional features beyond the original Phase 1 spec were also added: air modeling (simulated room air high-frequency rolloff), and mic position controls (off-axis blend). Phases 2 and 3 remain future work.
>
> **As-built deviations from plan:**
> - DSP parameter names differ from spec. As implemented: `irBlend` (not `blendMix`), `slotAGain` / `slotBGain` (dB, not spec's `irSlots[].level`), `slotAPolarity` / `slotBPolarity` (not `irSlots[].polarity`). The `hpCutHz`, `lpCutHz`, `cabOutputDb` named in the spec are implemented as `hpCut`, `lpCut`, `output` (normalized/dB).
> - File touchpoints: `core/src/presets/PresetTypes.h` and `core/ui/ts/` (not `core/ui/ts/` as written — but path structure is correct relative to repo root).
> - Beyond-spec additions: `air` (presence/air high-shelf), `airGain`, `micPosition`, `micBlend` controls added to the effect, giving extra tonal shaping without an external EQ.

## Goal
Improve the IR cabinet effect so it sounds more realistic under playing dynamics while remaining easy to shape in mixes and reliable in real-time use.

## Scope (Phase 1)
1. Add multi-IR support (2-slot blend) with per-slot level and polarity.
2. Add cabinet control surface: low cut, high cut, and output trim.
3. Add IR metadata support (speaker/mic/cab tags) for better browsing.
4. Add safe runtime switching behavior (click-free IR swap with short crossfade).
5. Ship factory presets demonstrating modern and vintage cab workflows.

## Non-Goals (Phase 1)
- No full physical speaker simulation.
- No long-latency linear-phase convolution mode by default.
- No breaking preset schema changes.
- No mandatory redesign of existing panel layout.

## Design Principles
- **Backward compatible**: old single-IR presets load unchanged.
- **Real-time safe**: no allocations or locks in audio thread.
- **Progressive complexity**: simple defaults; advanced controls remain optional.
- **Mix-first workflow**: quick high/low cuts and gain normalization are always available.

## User Problems To Solve
- Single IR often feels static and hard to fit across guitars and pickups.
- IR switching can cause level jumps and audible artifacts.
- Users struggle to audition IRs without strong metadata and comparison tools.
- Blending mics/cabs is a common request currently requiring external tools.

## Feature Set

### Phase 1 (MVP)
- **Dual IR blend**
  - Slots `A` and `B` with:
    - `mix` (0..1 crossfade)
    - per-slot level trim (dB)
    - per-slot polarity invert
- **Tone shaping**
  - post-cab high-pass and low-pass filters
  - cabinet output trim (dB)
- **Safe IR switching**
  - crossfade old/new IR over short window (e.g., 20–80 ms)
  - optional auto gain compensation based on measured IR energy
- **Metadata-aware browsing**
  - tags: speaker, mic, position, cab type, source/provider

### Phase 2
- 4-slot blend matrix (up to 4 IRs)
- per-slot delay (sub-ms alignment) for phase management
- stereo modes (L/R independent or linked)
- A/B audition and quick scene snapshots

### Phase 3
- optional cabinet resonance model (lightweight modal enhancement)
- optional dynamic speaker saturation/compression stage
- quality modes (economy/standard/high) with explicit CPU budgets

## DSP Changes

### Data Model
- Extend cab node config to support:
  - `irSlots[]` (resource refs + gain/polarity/delay)
  - `blendMix`
  - `hpCutHz`, `lpCutHz`, `cabOutputDb`
  - `autoGainCompEnabled`
- Preserve old fields and auto-upgrade to slot `A` when only one IR exists.

### Processing Pipeline (Phase 1)
`input -> convolver(A) + convolver(B) -> blend -> hp/lp -> output trim`

### Runtime Behavior
- pre-load target IR to background-safe buffer/state
- swap via atomic pointer/state handoff
- crossfade convolution outputs to avoid zipper/click artifacts

### Performance Constraints
- Keep Phase 1 cost close to current + one additional convolver.
- Avoid per-block heap operations.
- Validate at 44.1k/48k/96k sample rates.

## UI Changes

### Core Controls (always visible)
- IR `A` select/browse
- IR `B` select/browse
- `Blend` knob/slider
- `Low Cut`, `High Cut`, `Output`

### Advanced Controls (collapsed section)
- per-slot level trim
- polarity invert
- auto gain compensation toggle

### Browser Enhancements
- filter chips for mic/speaker/cab/source
- quick compare mode: “keep playing while switching”

## Preset & Compatibility
- Existing presets map to:
  - `A = existing IR`
  - `B = empty`
  - `Blend = 0` (100% A)
- Serialization must include defaults only when needed to keep preset JSON stable.

## Validation Plan
- Load legacy presets and ensure identical sound within tolerance.
- Swap IRs during playback; verify no clicks/pops.
- Validate level consistency with auto-gain on/off.
- Confirm dual IR blend null/phase behavior with polarity tests.
- Confirm deterministic output in offline processing tests.
- Confirm UI state persists and reloads correctly.

## Test Coverage Additions
- **PresetDSPLoadingTests**: legacy + new cab schema load paths.
- **PresetDSPProcessingTests**: blend and filter parameter sweeps.
- **IRConvolutionTests**: crossfade correctness and channel integrity.
- **RealAudioProcessingTests**: real-time switch stress test.

## Telemetry / Diagnostics (Optional)
- cab CPU time per block
- IR switch count and switch glitch counter (if available)
- clipping/headroom stats pre/post cab output trim

## Rollout Plan
1. Implement Phase 1 backend schema + DSP path.
2. Add UI controls with legacy fallback.
3. Add metadata ingestion and browser filters.
4. Add/refresh factory presets.
5. Run audio regression and performance checks.
6. Enable by default after validation window.

## Risks & Mitigations
- **CPU increase**: ship 2-slot only first; optimize IR length/partition settings.
- **Phase issues in blends**: include polarity and optional micro-delay controls.
- **Loudness jumps**: add auto-gain and output trim defaults.
- **Preset migration bugs**: add explicit legacy mapping tests and fail-fast logs.

## File Touchpoints (planned)
- `core/src/presets/PresetTypes.h`
- `core/src/presets/PresetTypesJson.h`
- `core/src/dsp/effects/` (cab IR processor implementation)
- `core/src/PluginController.cpp` (state routing + resource updates)
- `core/ui/ts/presetV2.ts`
- `core/ui/ts/signalPath.ts`
- `core/ui/ts/types.ts`
- `core/ui/assets/` (factory presets/metadata)

## Open Questions
- Should dual-IR be limited to `cab_ir` only, or shared with composite-exposed cab resources immediately?
- Do we want auto-gain on by default for imported third-party IRs?
- What is the maximum acceptable extra DSP load at 48k/128 samples for Phase 1?
