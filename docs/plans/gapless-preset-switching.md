# Gapless Preset & Scene Switching

Status: **Phases 1–2 implemented**, plus the ring-out half of Phase 4 (see below); Phases 3
and 5 proposed. Covers crossfaded preset switching, optional delay/reverb tail preservation,
and the resource-loading work needed to make instance construction fast enough that switching
is perceptually instant.

## Goal

Switching preset → preset, or scene → scene, must never produce a dropout, a click, or a
CPU spike large enough to xrun. Two optional behaviours sit on top of that baseline:

1. **Crossfade** — old and new chains overlap for a configurable window instead of a hard cut.
2. **Tail preservation** — the outgoing chain keeps ringing (delay repeats, reverb decay)
   after its input has been handed to the new chain, then retires itself.

## Assumptions

1. The primary consumer is live use: footswitch/MIDI-driven setlist and scene changes where
   a 200 ms gap is unacceptable. DAW/offline rendering is secondary but must not regress.
2. Global pre/post chains (gate, transpose, EQ, doubler) stay shared and are *not* part of the
   swap — they already come from app settings, not presets (`PluginController.cpp:11794-11825`).
   Their tails therefore survive a switch for free.
3. CPU headroom is finite. A crossfade runs two full chains; the design must degrade
   gracefully rather than xrun.
4. No preset schema break. Any new fields are additive with defaults.

---

## What happens today

**Preset switch** (`PluginController::ApplyPreset`, `PluginController.cpp:11753`):

1. Normalize preset off-lock.
2. `MultiPresetMixer::PreparePresetSwap()` (`MultiPresetMixer.cpp:300`) builds the whole new
   `PresetInstance` off the DSP lock — `SetGraph()` (create processors + load resources) and
   `Prepare()`. This part is already good.
3. Under `mDSPMutex` (`PluginController.cpp:11839-11894`): `SetGlobalChainConfig()`,
   `CommitPresetSwap()`, NAM calibration injection, callback attachment.
4. `CommitPresetSwap()` (`MultiPresetMixer.cpp:366`) does `mInstances.clear()` then installs
   the pending instance, and schedules a 1024-sample output fade-in (`MultiPresetMixer.cpp:1519`).

**Scene switch**: the UI writes the scene's graph into the preset and re-sends the whole preset
as a `loadPreset` message (`signalPath.ts:6728-6740` → `pushScenePresetToBackend`), which lands
in `HandlePresetLoadRequest` (`PluginController.cpp:4731`) and takes either `ApplyPreset` or
`ReplaceActiveMixerPresetInPlace` (`PluginController.cpp:4453`). **Either way the entire graph is
torn down and rebuilt, and every NAM model and IR is re-read from disk** — even when the two
scenes share the same amp and cab.

### Root causes of audible glitching

| # | Cause | Evidence |
|---|---|---|
| G1 | Audio thread `try_lock`s `mDSPMutex` and **outputs silence on failure**. Any lock hold spanning a block boundary is a dropout. | `PluginController.cpp:2530-2539` |
| G2 | `SetGlobalChainConfig()` → `RebuildGlobalChains()` runs **inside** the lock and does `SetGraph()` + `Prepare()` on both global executors — construction and allocation while the audio thread spins. | `PluginController.cpp:11851`, `MultiPresetMixer.cpp:450-484` |
| G3 | `mInstances.clear()` **destroys** the old instance under the lock: freeing NAM models, convolver partition tables and node buffers on the message thread while audio is blocked. | `MultiPresetMixer.cpp:373` |
| G4 | Hard cut of the old chain — all tails die instantly. The only masking is a 1024-sample fade-in of the *new* output; there is no fade-out. | `MultiPresetMixer.cpp:377-378`, `:1519-1535` |
| G5 | No latency alignment. A and B can declare different `GetTotalLatencySamples()`; the switch produces a phase jump, and `UpdateHostLatency()` re-negotiates PDC. | `PluginController.cpp:11541` |
| G6 | Rebuild cost is on the critical path for scenes, and resource loading has **no cache at all**: `nam::get_dsp(path)` is called twice per NAM node (L and R), each re-parsing the file. | `OptimizedNAMAmpEffect.h:552-553` |

G1–G3 are the ones that produce the actual dropout. G4–G6 are what stops it feeling seamless.

---

## Target architecture

Use the mixer as the crossfade engine, which is what it structurally already is — instances
already sum with per-instance gains. What is missing is a **lifecycle** per instance and a
**lock-free install path**.

### Instance lifecycle

```
  Building (message/worker thread, off audio)
      │  install command
      ▼
  FadingIn ──► Active ──► FadingOut ──► Retired ──► reaper thread deletes
                   │                        ▲
                   └──► Ringing (input muted, output live) ──┘
```

Per-instance state added to `MultiPresetMixer::PresetInstance`:

- `inputGain` — smoothed ramp, `1.0` normally, ramped to `0.0` to start a ring-out.
- `outputGain` — smoothed ramp, drives the crossfade (separate from user `cfg.mix`; final
  gain is `cfg.mix * outputGain`).
- `phase` + `samplesRemainingInPhase`.
- `alignDelay` — small delay line for latency matching (see L1 below).
- `silentBlockRun` — consecutive blocks whose output is below the retire threshold.

Ramps are per-sample linear over the block; no branch per sample beyond the existing mix loop.

### Lock-free install

Replace "message thread takes `mDSPMutex` and mutates `mInstances`" with:

- A fixed-capacity instance array owned by the audio thread (8 slots; `kMaxWorkItems` is
  already 16 so the parallel dispatch absorbs this).
- An SPSC command ring drained at the top of `Process()`:
  `InstallInstance{slot, PresetInstance*}`, `StartCrossfade{fromSlot, toSlot, samples, curve}`,
  `StartRingOut{slot, inputFadeSamples, maxTailSamples}`, `RetireInstance{slot, fadeSamples}`,
  `SetParamRamp{slot, nodeId, key, target, samples}`.
- Ownership transfers via raw pointer in the command; the audio thread never allocates or frees.
- Retired instances go onto a garbage SPSC ring drained by a reaper thread that deletes them.

Net effect: **the message thread never holds a lock the audio thread needs during a switch**,
so G1/G3 disappear regardless of how long the build takes.

`mDSPMutex` stays for the many other message handlers that poke node params; it is only the
swap path that moves to the command ring.

### Crossfade

Both instances receive the same pre-chain output. Outgoing ramps `1 → 0`, incoming `0 → 1`,
over `crossfadeMs`. Curve is a setting (see decisions).

### Tail preservation

On switch with `preserveTails`:

- B fades in over `crossfadeMs`.
- A's **input** ramps to zero over `tailInputFadeMs` (a ramp, not a cut — a step into a delay
  line is itself a click).
- A's **output** stays at its current gain, so repeats and reverb decay keep sounding.
- A retires when its output stays below `-84 dBFS` for 4 consecutive blocks **or**
  `tailMaxMs` elapses, whichever comes first; retirement applies a 20 ms fade-out.
- At most `maxRingingInstances` (default 1) ring at a time. A faster switch fast-fades the
  oldest ringing instance over 30 ms.

**The optimisation that makes this cheap** — most of a ringing chain is doing nothing. Add to
`EffectProcessor`:

```cpp
/// True if this effect can generate output from an all-silent input (delay, reverb,
/// feedback modulation). False for memoryless-after-latency effects: amp, cab IR, EQ,
/// drive, dynamics.
[[nodiscard]] virtual bool ProducesTail() const { return true; }
```

`SignalGraphExecutor::Process()` tracks a per-node silent-input run length (the block peak is
already computed on the diagnostics path) and skips any node with `!ProducesTail()` once its
input has been silent for longer than its own `GetLatencySamples()`. During a ring-out the NAM
model and IR convolver self-bypass within a few ms and the residual cost collapses to the
time-based nodes only. This is worth doing on its own merit — it also kills the cost of silent
parallel branches in normal playback.

### Latency handling

- **L1** — during a crossfade, delay the shorter chain's output by `|latencyA − latencyB|`
  samples using the per-instance `alignDelay` (preallocated, e.g. 8192 samples max). Without
  this the fade window comb-filters and the end of the fade steps in phase.
- **L2** — report `max(latency)` across live instances to the host, and coalesce
  `UpdateHostLatency()` so it fires once when the mixer settles to a single instance, not
  mid-fade.
- **L3** — if `|Δ| > alignDelay` capacity, skip alignment and shorten the fade to 10 ms
  (documented degradation, reported to the UI).

---

## Fast instantiation (the other half of the problem)

Crossfading hides latency in the *transition*; it does not help if building B takes 400 ms and
the user is hitting a footswitch. Two tracks: make builds cheap, and do them before they are needed.

### Resource caches

**NAM model cache.** The vendored core exposes `nam::get_dsp(dspData& conf, DspLoadOptions)`
(`NAM/get_dsp.h:90`) and `dspData` is a plain copyable struct (version, architecture, config
JSON, metadata, `std::vector<float> weights`) (`NAM/dsp.h:348`). So:

- Cache key: canonical path + file size + mtime + slimmable-size setting.
- Cache value: `std::shared_ptr<const nam::dspData>`.
- Construction becomes `get_dsp(copyOfCachedData)` — no file IO, no JSON parse of the weights
  blob. That is the large majority of the per-model cost.
- Fixes the double-parse at `OptimizedNAMAmpEffect.h:552-553` for free (second call is a cache hit).
  Additionally, only construct the right-channel model when the node actually runs stereo —
  decide from the graph at build time, never lazily on the audio thread.
- `DspLoadOptions::prewarm` is available; keep prewarm on (we are already off the audio thread)
  but measure it — it may be a meaningful share of the remaining cost.

**IR cache.** `RealtimeConvolver::SetImpulse()` precomputes the frequency-domain partitions
(`RealtimeConvolver.h:48`, `:102`). Those tables are read-only during processing; per-instance
state is just the input FDL and overlap buffers. So:

- Cache key: path + size + mtime + sample rate + quality/length/normalize/lowLatency/stereo + `maxBlockSize`.
- Cache value: `std::shared_ptr<const PartitionedImpulse>`.
- `RealtimeConvolver` gains `SetImpulseShared(std::shared_ptr<const PartitionedImpulse>)`
  alongside the existing `SetImpulse`; the FFT partition build is skipped on a hit.

**Preset JSON cache.** Keyed by preset id + mtime, so setlist stepping does not re-read and
re-deserialize from disk.

**Eviction.** LRU against a memory budget (`resourceCache.budgetMb`, default 512). Entries
referenced by a live instance are pinned via the `shared_ptr` refcount. Eviction runs on the
reaper thread, never on the audio or message thread.

### Prebuild / warm-ahead

Once the caches exist, prebuilding is cheap enough to do speculatively on a background
worker (below-normal priority):

- **All scenes of the active preset** — usually 2–4, sharing nearly all resources. A scene
  switch then becomes a pointer install.
- **Setlist neighbours** — the next and previous slot, rebuilt whenever the cursor settles.
- **Browser hover / selection** — a "touch" that only populates the resource caches, no
  instance built.

Prebuilt instances are keyed by `(presetId, sceneId, contentHash)` and invalidated when the
preset is edited. Cap the pool (e.g. 4 instances) and evict LRU.

### Scene switching without a rebuild (the big win)

Scenes of one preset usually differ only in node params and enabled flags. Add a graph diff:

- If node ids + types + edges + resource refs are **identical**, apply only the param/enabled
  deltas through the existing `SetNodeParam`/`SetNodeEnabled` setters. No rebuild, no fade,
  no tail loss — delay and reverb state is *preserved* rather than merely ringing out. This is
  the genuinely gapless case and should be the default scene path.
- Otherwise, fall back to the prebuilt-instance crossfade path.

Caveat: many effects do not smooth their parameters, so a large jump still clicks. Pair the
diff path with `SetParamRamp` (already in the command list above) applied over ~20 ms. Scope
this honestly: ship the diff with immediate application first, measure which params click
(expect gain/level/mix/drive), then ramp that identified set.

### Remove the unconditional global-chain rebuild

`ApplyPreset` calls `SetGlobalChainConfig()` on every load, which rebuilds both global
executors even when the config is byte-identical (the common case, since globals do not come
from presets). Compare the config and skip. This removes a full rebuild plus its lock hold from
**every** preset load and is the single cheapest fix in this document.

---

## Decisions (proposed — confirm before implementation)

| Topic | Decision | Rationale |
|---|---|---|
| Default switch mode | `crossfade`, 150 ms | Hard cut stays available for players who want an abrupt channel-switch feel |
| Default curve | **Linear (equal-gain)** | Both slots carry the same source and are strongly correlated at the fundamental; equal-power overshoots ~3 dB. Equal-power offered as an option for long, character-changing swells |
| Tail preservation default | **On**, two bars at the current tempo | Matches how hardware multi-FX behave. Shipped as a bar count rather than `tailMaxMs`: the cap only has to catch a tail that never decays on its own, and retire-on-silence ends every other one long before it |
| Where settings live | App settings, not presets | Matches the existing treatment of global chain config; per-scene overrides are additive and optional |
| Scene switch default | Diff-and-apply when topology matches | Preserves state; strictly better than any crossfade |
| Instance cap | 8 slots, ≤1 ringing by default | Bounds worst-case CPU |
| Latency alignment | On by default during fade | Otherwise the fade comb-filters |

## Settings and protocol

New app settings (additive, all defaulted):

```
switching.mode              "hard" | "crossfade"       (default "crossfade")
switching.crossfadeMs       0..2000                    (default 150)
switching.curve             "linear" | "equalPower"    (default "linear")
switching.alignLatency      bool                       (default true)
switching.prebuildScenes    bool                       (default true)
switching.prebuildSetlist   bool                       (default true)
resourceCache.budgetMb      64..4096                   (default 512)
```

Shipped, superseding the three `switching.tail*` keys this originally proposed:

```
audio.presetSwitch.tailBars 0..4                       (default 2, 0 = off)
```

One key rather than three. Bars rather than milliseconds because the point is musical — the
tail should die out over the phrase, so the number only becomes seconds when the switch
happens, resolved against the tempo and meter the player is on then
(`PluginController::UpdatePresetSwapTailBudget()` → `MetronomeService::BarSeconds()` →
`MultiPresetMixer::SetPresetSwapTailSeconds()`). There is no separate on/off or input-fade
setting: 0 bars is off, and the input fade is the declick window the crossfade already uses.
Surfaced under Settings → General → Preset Switching.

Optional additive `PresetScene` fields — `crossfadeMs`, `preserveTails` — so a solo-boost scene
can be instant while an ambient scene swells. Older builds ignore unknown fields.

New UI messages:

- `switchPreset` → `{presetId, sceneId?, crossfadeMs?, preserveTails?}` — explicit switch
  intent, distinct from `loadPreset` (which also means "here is edited preset content").
  Footswitch/MIDI paths use this.
- `presetSwitchState` (backend → UI) → `{phase: "prebuilt"|"fading"|"ringing"|"settled", fromId, toId, progress}`
  for a subtle transition indicator and for CPU-degradation notices.

`loadPreset` keeps working unchanged.

## CPU safeguards

- Predict combined load from `complexityScore` (already computed, `MultiPresetMixer.cpp:155`)
  plus the last block's measured DSP load. If the pair would exceed a threshold (~75% of
  real-time), shorten the fade to 10 ms and skip tail preservation for that switch, and report
  it via `presetSwitchState`.
- Crossfading instances participate in the existing mixer work-stealing pool automatically.
- The `ProducesTail()` silence bypass is what keeps a ringing instance near-free.

---

## Phasing

**Phase 1 — stop the dropout** — **DONE**. No new user-facing behaviour or settings.

1. ✅ Global chain config compared by value (`operator==` on the graph/config types) and the
   rebuild skipped when unchanged — the common case on every preset load.
2. ✅ `PrepareGlobalChainSwap()` / `CommitGlobalChainSwap()` build the pre/post executors off
   the lock and install them under it. All four call sites converted (fixes G2).
3. ✅ Retired instances and executors go to a background reaper thread instead of being
   destroyed under `mDSPMutex` (fixes G3).
4. ✅ `CommitPresetSwap()` crossfades: the outgoing instance stays in the mix ramping down
   while the incoming one ramps up, over `kPresetFadeSamples` (1024, ~21 ms). The old
   master-output fade-in is gone — it could not hide the step down to silence and it ducked
   the global post-chain's tail. `ReplaceActivePresetInPlace()` (the multi-slot scene path)
   crossfades the same way.

Notes on what Phase 1 forced:

- `mInstances` now holds `std::unique_ptr<PresetInstance>`. Required, not cosmetic: dropping a
  finished fade-out on the audio thread would otherwise move `SignalGraphExecutor` values, and
  its move-assignment calls `StopWorkers()`, which joins threads.
- Retiring instances stay in `mInstances` (so the existing dispatch/mix paths handle them with
  no special casing) but are hidden from every lookup, count and query. Necessary because a
  scene switch reuses the preset ID — `FindInstance()` would otherwise route parameter updates
  into the chain on its way out.
- `BeginFadeOut()` resumes the ramp from the instance's current gain. Switching again while a
  fade-in is still running must not snap it back to unity; that step is the click being
  designed out, and it showed up in testing.
- Residual: swapping a global chain executor still joins that executor's worker threads under
  the DSP lock (bounded, and zero for the linear default chains, which never start workers).
  Fully removing it needs the executors held by pointer too, or `SignalGraphExecutor`'s move to
  transfer worker threads — deferred to Phase 3, which reworks that ownership anyway.

Covered by `core/tests/GaplessSwitchingTests.cpp`: crossfade continuity, retiring instances
hidden from queries, unchanged-config rebuild skip, in-place replace preserving other slots,
and rapid switching staying bounded and finite.

**Phase 2 — build cost** — **DONE**. See "Measured build cost" below for the numbers.

5. ✅ NAM `dspData` cache (`core/src/dsp/NamModelCache.h/.cpp`), keyed by canonical path +
   size + mtime, LRU against a byte budget. Both NAM amp effects go through it, so the
   second per-node model construction is a struct copy rather than a second file parse.
   Stereo-aware right-model construction turned out to be unnecessary: with the cache the
   second construction costs ~0.1 ms, so the only thing worth avoiding was its prewarm.
6. ✅ Redundant model prewarm removed. `nam::DSP::Reset()` prewarms, and it ran twice per
   channel per switch: once from `LoadModelResource()` and again from `Prepare()`. The
   load-time pass is skipped entirely (the effect does not know the real sample rate yet,
   so `Prepare()` would redo it), and `Prepare()` re-prewarms only when the resolved model
   rate or block size actually changed. Safe because `SignalGraphExecutor::Process()`
   refuses to run unless prepared, and `SetGraph()` re-prepares when already prepared.
7. ✅ Node `Prepare()` parallelised across the graph in `SignalGraphExecutor::Prepare()`,
   mirroring the existing dispatch in `CreateProcessors()` (main-thread-required effects
   stay serial). The remaining left/right model prewarm inside `OptimizedNAMAmpEffect`
   runs the two channels concurrently, since a single-amp preset is the common case.
8. ✅ `CompositeEffectProcessor` now reports `RequiresMainThreadLoad()` by asking its inner
   graph (`SignalGraphExecutor::AnyNodeRequiresMainThreadLoad()`). Without this a composite
   wrapping a plugin host looked thread-safe to the parent and would have been dispatched
   to a worker by the new parallel `Prepare()` — the exact deadlock the main-thread
   carve-out exists to prevent. Also closes the same gap on the pre-existing load path.

Deferred from Phase 2, on measurement:

- **IR partition cache / `SetImpulseShared()`** — measured at 0.33-0.89 ms per `SetImpulse()`,
  and IR loading already overlaps NAM loading on the parallel dispatch, so it is currently
  hidden entirely. Not worth the shared-ownership complexity until NAM prewarm stops
  dominating.
- **Preset JSON cache** — not yet measured as significant next to the ~4.5 ms prewarm floor.

### Measured build cost

`core/tests/ResourceLoadBenchmark.cpp` (labelled `benchmark`, excluded from default runs)
reports these. Rig: `input -> NAM(411 KB) -> IR cab -> delay -> reverb -> output`, 48 kHz,
512-sample blocks, Release, warm OS file cache.

| | Before Phase 2 | After Phase 2 |
|---|---|---|
| `SetGraph` (create + load resources) | 21.6 ms | 4.3 ms |
| ↳ NAM node alone | 22.4 ms | 1.3 ms |
| `Prepare` (prewarm + buffers) | 9.2 ms | 6.6 ms |
| **Total switch latency** | **32.0 ms** | **11.4 ms** |

Where the original 32 ms went, and what happened to it:

| Cost | Before | After | Fix |
|---|---|---|---|
| `get_dsp(path)` × 2 (file read + JSON parse) | 11.3 ms | 0.15 ms | dspData cache |
| Prewarm × 2 at load time | 9.6 ms | 0 ms | skipped; `Prepare()` owns it |
| Prewarm × 2 in `Prepare()` | 9.6 ms | ~4.5 ms | run concurrently |
| IR decode + partition | ~1.5 ms | hidden | already parallel with NAM |

Answers to two open questions in this document:

- **`nam::DSP` prewarm cost** (risk 2) is ~4.2-4.8 ms per model and is *not* part of
  `get_dsp()` — `DspLoadOptions::prewarm` made no measurable difference, because the
  prewarm happens on `Reset()`. It is now the floor on a cold switch.
- The dspData cache does **not** need to key on slimmable size:
  `ApplyGlobalNamSlimmableSize()` is applied to the constructed model, not to `dspData`.

**Phase 2b — beating the prewarm floor.** ~4.5 ms of unavoidable per-model prewarm now
dominates a cold switch. Only speculative prebuild (Phase 5) removes it from the critical
path, which raises its priority relative to Phase 3.

**Phase 3 — instance lifecycle and crossfade**
8. Per-instance `inputGain`/`outputGain` ramps and `phase`; command ring + reaper; lock-free install.
9. Crossfade on preset switch, with `switching.*` settings and the CPU guard.
10. Latency alignment (L1–L3) and `UpdateHostLatency()` coalescing.

**Phase 4 — tails**
11. `EffectProcessor::ProducesTail()` and the executor's silence-bypass. **Still open**, and
    now the main lever left: a ringing instance runs its whole chain, NAM and IR included,
    for as long as it rings. `canRingOut` (item 12) bounds *which* presets pay that and
    `kMaxTailingInstances` bounds how many at once, so the worst case is 2× the DSP load of
    one preset for the length of one tail — but the silence-bypass is what would make it
    approximately free, and it would also stop the amp contributing sustain of its own.
12. ✅ Ring-out lifecycle, retire-on-silence, `maxRingingInstances`. **DONE**, ahead of
    Phase 3 — it needed only a new instance phase, not the command-ring rework.

    A superseded instance enters `Tailing`: its *input* ramps to zero over the same 1024-sample
    declick window while its output gain is held flat at whatever it was at, so nothing new
    enters the chain and what is already circulating in it rings over the incoming preset.
    Only a graph that can actually still sound with its input cut is offered that at all —
    the build-time `canRingOut` check looks for a delay or reverb node, through composites,
    and treats a plugin or WASM host as a maybe. Everything else is cut on the declick ramp
    exactly as before. Without that check a preset whose chain does not decay — a synth voice,
    anything self-oscillating — kept playing under the new one for the whole budget, which is
    not a tail, it is the old preset refusing to leave.

    A ring-out is then dropped as soon as its own output stays under −70 dBFS for 150 ms,
    which is what ends an ordinary reverb long before the budget does. The budget is the
    backstop for a tail that never decays, and it releases over 250 ms rather than the
    declick, so cutting a live feedback delay at the end reads as an ending rather than a
    chop. One rings at a time (`kMaxTailingInstances`), so a switch can never more than
    double the DSP load; a second switch starts the first one's release rather than cutting it.

    A ring-out is also kept out of the parallel-dispatch decision, which counts live
    instances only. Letting it count put a single-preset session on the parallel path after
    every switch, and that path spin-waits on normal-priority workers **while holding the DSP
    lock** — a worker that is not scheduled promptly hangs the audio thread, and every
    message-thread handler behind it. That spin-wait is still a latent risk for a Multi-Rig
    session that legitimately fans out, and is worth removing on its own merits.

    Known trade-off, deliberate: for the ~21 ms of the input ramp the outgoing chain's output
    is not attenuated, so a chain that compresses hard (a high-gain amp) does not fall away as
    fast as its input does and the two presets can briefly sum above unity. On a linear chain
    they sum to unity exactly. A shorter input ramp trades that against the click it exists to
    prevent; 21 ms is the same window the crossfade already uses.

    Not done, from the design above: latency alignment during the ring-out (Phase 3's L1), and
    the per-scene `preserveTails` override.

**Phase 5 — scenes and prebuild**
13. Graph diff for same-topology scene switches; `SetParamRamp` for the click-prone params.
14. Speculative prebuild of scenes and setlist neighbours; browser-hover cache touch.
15. UI settings surface + `presetSwitchState` indicator.

Phases 1 and 2 are independently valuable and carry almost no risk. Phase 3 is the one that
touches the audio thread's ownership model and needs the most review.

## Testing

New suite `GaplessSwitchingTests` (add to `core/tests/CMakeLists.txt`), all offline and deterministic:

- **No-dropout**: drive a sustained sine through the mixer, issue a switch mid-stream, assert
  no all-zero block and no first-difference discontinuity above threshold across the transition.
- **Crossfade law**: switch between two known-gain passthrough graphs; assert the summed output
  tracks the configured curve within tolerance.
- ✅ **Tail preservation**: preset A is a fully wet delay with heavy feedback; switch away to
  something silent; assert energy is still present at t+400 ms with the spill on and none with
  it off, that a preset with no tail is dropped inside a quarter of its budget, and that a
  runaway feedback delay is gone once the budget and its release expire.
- **Latency alignment**: crossfade two chains with different declared latency; assert the
  spectral null depth at the fade midpoint stays above a threshold (i.e. no comb).
- **Silence bypass**: assert a `!ProducesTail()` node stops being processed after silent input
  exceeding its latency, and that its output is bit-identical to processing it.
- **Scene diff**: same-topology scene switch performs zero processor construction (instrument a
  counter) and preserves delay-line contents.
- **Cache**: same path loaded twice hits the cache and produces bit-identical output; mtime
  change invalidates; eviction never frees a pinned entry.
- **Benchmark harness**: build time for a representative NAM+IR preset, cold vs warm, reported
  so regressions are visible.

Because this is audio-thread behaviour, unit tests are necessary but not sufficient. Verify in
the live app over WebView2 remote debugging per `docs/agent-quickstart.md` ("Live UI Testing")
and `tools/agent-ui-debug/README.md`: switch presets and scenes repeatedly under load and
confirm no dropouts in the DSP telemetry, then listen.

## Risks and open questions

1. **Phase 3 changes audio-thread ownership.** The command-ring refactor is the highest-risk
   item here. Mitigation: land Phases 1–2 first, which already remove the dropout; Phase 3 then
   buys smoothness rather than correctness, so it can be gated behind a setting during bake-in.
2. **`nam::DSP` prewarm cost** may dominate what remains after the `dspData` cache. Needs
   measurement before promising a build-time figure.
3. **Hosted plugin nodes** require main-thread loading (`RequiresMainThreadLoad()`,
   `SignalGraphExecutor.cpp:592`). Speculative prebuild of presets containing plugin hosts must
   marshal to the main thread and may not be worth prebuilding at all — likely excluded in v1.
4. **Per-preset noise gates** inside a graph will chop that graph's own tail during ring-out.
   The global gate is upstream and unaffected. Document rather than fix.
5. **Equal-gain vs equal-power** default is a taste call; worth an A/B before locking.
6. **Memory**: prebuilt instances plus caches raise the floor. The 512 MB default budget and the
   4-instance prebuild cap need validating against real setlists with large IR/NAM collections.
