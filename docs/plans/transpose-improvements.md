# Transpose Improvements Plan

Status: in progress (2026-09). Covers the transpose/pitch-shift effect family: `pitch_shift`, `transpose` (also the global pre-chain transpose), `transpose_stft`, and `transpose_hybrid`. Related Signalsmith users: `octave`, `arp_auto`.

This revision recasts the live goal around **guitar and bass to -12 st**, with proof gates before listening, and a two-engine product (Signalsmith for shallow, a low-latency engine for deep drop). Do not implement DSP from this doc until the harness can actually prove bass -12.

## Goal

Ship **high-quality ultra-low-latency transpose down to -12 st** for live guitar **and bass**, with honest PDC and competitive tone vs HyperTune / Archetype.

HyperTune at ~7 ms on guitar is the existence proof that the latency budget is reachable. "Highest possible sound quality" is not a bigger FFT: a bass B0 after -12 is ~15 Hz and cannot be resolved as a fundamental inside a 7-10 ms STFT window. Quality at that latency means correct interval, intact attacks, usable chords, and lows that still sound like the instrument.

Two regimes need different engines. One fixed Signalsmith `presetCheaper` config cannot serve both.

| Regime | What wins today | Target |
|---|---|---|
| Shallow drop **+/-1 to -3 st** | Signalsmith tone (STFT does not actually shift) | ~8-12 ms with correct pitch and good tone |
| Deep drop **-5 to -12 st** (live) | STFT latency; HyperTune-class tone not yet proven on bass | **<=8-10 ms** measured @ 48 kHz, HyperTune-class guitar **and** bass |
| Offline / max quality | Signalsmith | Latency secondary |

Global transpose stays **+/-12** until the live engine is solid. Transparent at 0 st remains the product contract (0 latency, dry copy).

## Physics constraint (why one engine cannot win)

At 48 kHz, **10 ms is about 480 samples**. STFT latency is `analysisWindow - synthesisWindow`. Enlarging the analysis window to lock a low fundamental **adds latency**; shrinking it to hit 8 ms **loses bass pitch**.

Displacement for a linear spectrum resample is `f0 * (1 - 2^(st/12)) * N / sr`. Below ~0.3 bins, energy does not move (this is the measured -1/-2 STFT failure). For an open low E (~82 Hz) at -1 st, N needs ~4096 -- over 60 ms. For bass after -12 (~31 Hz E0, ~15 Hz B0), a HyperTune-class window cannot carry the fundamental as an STFT partial.

**Implication:** live -12 for bass must treat lows with a **known-ratio time-domain** path (SOLA / resampling / grain), not a larger FFT. STFT remains the candidate for mids/highs and guitar polyphony.

## Current State

Global pre-chain node `global_transpose` (`EffectGuids::kTranspose`) is still **Signalsmith** (`TransposeEffect`, `presetCheaper(2, sr, false)`). Mixer/UI clamp +/-12. 0 st bypasses Stretch and reports 0 latency. STFT and hybrid are experimental-flag gated in the FX catalog.

| Effect | Engine | Range (st) | Latency @48 kHz (measured) | Notes |
|---|---|---|---|---|
| `pitch_shift` | Signalsmith `presetCheaper(2, sr, false)` | -12..+12 (continuous) | ~4800 samples (~100 ms) when shifting | Tonality limit 8 kHz |
| `transpose` | same | -36..+12 (integer); global clamp +/-12 | ~100 ms when shifting | Tonality limit 16 kHz; **this is the live global path today** |
| `transpose_stft` | STFT phase vocoder (`stftPitchShift`) | -12..+12 | LL ~6.7-14 ms; poly ~13-26 ms | Profiles by `abs(st)` + mode; experimental |
| `transpose_hybrid` | Dual-band **dual STFT** (900 Hz split) + dry transient assist | -15..0 | ~7-29 ms; **~2.1-2.5 ms/block** | Auto poly STFT at >=4 st depth; experimental; research only |

Sources: `core/src/dsp/effects/PitchShiftEffect.h`, `TransposeEffect.h`, `StftTransposeEffect.h`, `HybridTransposeEffect.h`; registration in `BuiltinEffects.h`; global node in `PresetTypes.cpp` (`BuildDefaultPreChainGraph`); clamp in `MultiPresetMixer::SetGlobalTranspose`.

### Numbers of record (guitar-riff-01 @ 48 kHz)

Internal pitch + latency: snapshot `pitch-metric` (2026-08-17). Competitive latency: `snapshot-20260712-083936` external passes. STFT windows: LL deep `{1024, 256, overlap 4}` -> 768 samples reported (16 ms); poly deep `{2048, 512, 8}` -> 1536 samples (32 ms).

| Variant | -12 meas / PDC | -12 CPU (avg block us) | -12 pitch | -2 pitch |
|---|---:|---:|---:|---|
| HyperTune Metal | **6.7 ms** / reports 0 | ~260 | not in pitch-metric snap | -- |
| Archetype Mansoor X | **18 ms** / reports 1.8 | ~360 | not in pitch-metric snap | -- |
| STFT low-latency | 12.7 / 16 ms | ~380-390 | **+2.3 +/- 6.1 c** | **+202 c (not shifting)** |
| STFT polyphonic | 26 / 32 ms | ~790 | -0.8 +/- 5.3 c | +4.3 +/- 24.7 c (-1 st fails) |
| Hybrid | 25 / 32 ms | ~2100-2500 | +1.6 +/- 5.6 c | **+202 c** |
| Signalsmith (`transpose` / `pitch_shift`) | 100 / 100 ms | ~60-70 | -1.8 +/- 8.4 c | +1.2 +/- 4.8 c |

Listening (guitar riffs only, 2026-07): STFT LL/poly approx HyperTune/Archetype at -12 mono+poly; hybrid has the most poly artifacts; at -7 Signalsmith tone wins and STFT starts to smear. No bass listening, no chord fixture, no commercial pitch numbers.

### Implementation note: hybrid is not time-domain lows

Code is dual STFT (`mLowPitch` / `mHighPitch` both `StftTransposeChannel`) plus a 900 Hz split and latency-aligned dry transient assist. That is why CPU is ~2x polyphonic STFT with extra smear risk. Hybrid stays a research branch until redesigned as **time-domain lows + locked STFT highs**. Do not promote it.

## Signalsmith latency model (canonical)

Source: [Signalsmith Stretch docs](https://signalsmith-audio.co.uk/code/stretch/) / README + vendored `signalsmith-stretch.h` (v1.3.2).

Latency is reported in **two halves**:

| API | Meaning |
|---|---|
| `inputLatency()` | How far **ahead** input sits relative to internal *processing time* (where pitch automation is centered) |
| `outputLatency()` | How far **behind** that processing time the audible output is |

For live 1:1 pitch shift (`process` with equal in/out lengths):

```text
PDC samples = inputLatency() + outputLatency()
```

Also relevant:

- After `reset()`, processing time is `inputLatency()` samples **before** the first real input -> pre-roll until the stream aligns.
- `outputLatency()` includes an optional **split-computation** hop: when `splitComputation` is true, one extra `intervalSamples` of output latency smooths CPU spikes.
- `presetCheaper` defaults `splitComputation=true`; we pass **`false`** intentionally for lower latency.
- Tonality limit is a **fraction of sample rate**: `setTransposeSemitones(st, hz / sampleRate)` -- our usage form is correct.
- For offline fixed-length renders: optional `seek` at start + silence pad + `flush` at end. Live/stream use does not require this; the benchmark deliberately measures stream warm-up latency.

Helper: `core/src/dsp/effects/SignalsmithLatency.h` -> `SignalsmithTotalLatencySamples()`.

### Latency contract (product)

| Situation | Report | Audio path |
|---|---|---|
| Shift active (nonzero st / wet pitch path) | `input + output` | Stretch process |
| Transparent (0 st, full wet) | **0** | Dry copy bypass |
| Partial mix while shifting | same total as wet | Dry delayed by total latency before blend |

Constant-latency-through-zero is **not** the chosen contract (transparent-at-zero is preferred for global transpose).

## Known Defects

1. ~~**Signalsmith latency under-report.**~~ **Fixed (2026-07):** report `inputLatency() + outputLatency()`; 0 st reports 0. Same pattern applied to `octave` and `arp_auto`. Dry/wet mix for `pitch_shift` / `transpose` / `octave` uses a latency-aligned dry delay.
2. **Inconsistent tonality limits.** `pitch_shift` uses 8 kHz, `transpose` uses 16 kHz for the same engine and preset. Unify (or make it deliberate and documented).
3. **Range/contract drift.**
   - Docs mention -24/-36 ranges but the runtime global transpose is clamped to +/-12 (`PluginController.cpp`, `MultiPresetMixer.cpp`, `controls.ts`). Decide: expand the runtime clamp or narrow the effect/docs. **Do not expand past +/-12 until the live engine is solid.**
   - `pitch_shift` has a `stepMode`/`minSemitones`/`maxSemitones` contract in UI/docs but the backend only supports `semitones` + `mix`. Restore backend support or remove from UI/docs.
4. ~~**Pitch Shift "does not shift" (listening).**~~ **Not reproducible (2026-08):** the pitch-accuracy metric measures `pitch_shift` within +/-3 cents at every setting from -12 to +12, jitter 3-8 cents. The original listening call was almost certainly the PDC under-report misaligning the compensated renders.
5. **STFT/Hybrid do not transpose at shallow settings.** Measured with the pitch-accuracy metric (snapshot `pitch-metric`, riff-01 @ 48 kHz):

   | Engine | -2 st | -1 st | +2 st |
   |---|---:|---:|---:|
   | STFT low-latency | **+202.4** +/-39.5 | **+83.8** +/-21.5 | **-192.6** +/-39.6 |
   | STFT polyphonic | +4.3 +/-24.7 | **+81.4** +/-38.2 | **-258.1** +/-184.8 |
   | Hybrid | **+201.8** +/-82.6 | **+88.0** +/-24.3 | -- |
   | Signalsmith | +1.2 +/-4.8 | +1.4 +/-3.5 | +2.8 +/-4.3 |

   At -2 st the error equals the interval: the output is at the **input pitch**. -3 st is marginal (+14.2 +/-43.8 LL); -5 st and deeper are correct. This is what "-1 st sounds awful" actually is -- not an artifact problem but a detuned near-copy of the input.

   **Mechanism:** `Pitcher` shifts by linearly resampling the spectrum, so a partial must move far enough between bins for interpolation to relocate energy. Displacement is `f0 * (1 - 2^(st/12)) * N / sr`; below roughly **0.3 bins** nothing moves. Confirmed experimentally: raising the polyphonic shallow profile to 2048 fixed -1 st (+0.4 cents) but tripled transient rise time (8.7 -> 26.7 ms).

   **This is structural, not a tuning problem.** Do not try to fix it by enlarging windows. Gate `transpose_stft` / `transpose_hybrid` off |st| <= 2 and route shallow shifts to Signalsmith or a time-domain engine.
6. **STFT latency slight over-report** on deep downshifts (e.g. rep 768 vs meas ~619 @ -12 LL, ~3.3 ms). Safer than under-report; tighten so |PDC - measured| < 1 ms before promoting.
7. **Hybrid docs/code mismatch** and high CPU / poly artifacts @ -12 -- do not promote from experimental yet.
8. **Proof harness cannot currently certify the goal.** Demo inputs are guitar-only; YIN dry floor is 65 Hz / absolute 30 Hz (bass -12 will read `n/a`); no chord fixture; no attack/onset metric; envelope latency bins ~0.67 ms (`kEnvelopeDecimation = 32`); blocks fixed at 512; commercial pitch numbers missing from the latest full snapshot.

## Proof program (do this first)

A path is not "high quality" until **pitch, latency, CPU, attacks, and listening** all pass on guitar **and** bass. Do not A/B tone on a pass that is not transposing.

### Harness gaps to close

`TransposeBenchmark` (`core/tests/TransposeBenchmark.cpp`) + `tools/transpose-benchmark/` already has honest vs measured latency, CPU, YIN pitch, HyperTune/Archetype via pedalboard, and HTML snapshot compare. Add:

| Gap | Why it blocks the goal |
|---|---|
| Bass DI fixtures (open E0 / B0 + riff) | -12 bass is the hard case; guitar riffs cannot proxy it |
| Sustained chord / poly fixture | Live drop-tune is chords, not only single-note riffs |
| YIN floor ~20 Hz (and longer correlation window on bass) | Current 65/30 Hz floors make bass -12 `n/a` |
| Attack / onset metric (rise time after PDC alignment) | Window shrink vs HyperTune is decided here, not by pitch alone |
| Finer latency (lower envelope decimation or subsample peak) | 0.67 ms bins are coarse vs an 8-10 ms budget |
| 64 / 128 block CPU in addition to 512 | Algorithm latency is window-based; live CPU is not |
| 48 kHz primary, 96 kHz check | STFT profiles are sample-count based today and will silently lose resolution at 96 k |
| Always vs HyperTune (+ Archetype) | Commercial bar must sit on the same report as internals |
| Pitch column on every snapshot | Latest full snap has no `pitchErrorCents`; pitch-metric snap has no commercial pitch |

Keep rendering WAVs compensated by **reported** latency so PDC lies stay audible.

### Numeric gates (before any listening)

| Check | Pass | Warn / fail |
|---|---|---|
| Pitch median | within **+/-10 cents**, jitter < 15 cents, enough frames | +/-25 cents warn; error approx -(interval) = **not transposing** (fail); `n/a` = no data, not a pass |
| 0 st | `0.0 +/- 0.0` cents, 0 latency, dry copy | any delay or pitch motion |
| PDC honesty | abs(reported - measured) **< 1 ms** | STFT currently ~3 ms over-report @ -12 LL |
| Live -12 latency | **<=10 ms** measured @ 48 kHz; stretch goal **<=8 ms** if quality holds | Signalsmith 100 ms is out; hybrid 25 ms is out |
| CPU | well below current hybrid (~2 ms/block); HyperTune ~260 us is the comfort bar | reject Hybrid-class cost without a clear quality win |
| Attacks | onset / rise competitive with HyperTune on the same fixture | smeared pick/finger attack = fail even if pitch is correct |

Listening only after gates: mono riff, chords, bass open-string + riff, guitar and bass.

## Latency Improvements

1. ~~**Honest Signalsmith PDC + dry align.**~~ Done (defect #1).
2. **Semitone-aware Signalsmith configuration.** Replace fixed `presetCheaper(2, sr, false)` with manual `configure(block, interval)` scaled by shift depth -- small shifts (+/-1-3 st) can target ~10 ms total. Scope as **shallow-shift / HQ mode**, not the live -12 path. After every reconfigure, re-query `SignalsmithTotalLatencySamples()` (never hardcode 2400/4800). Keep `splitComputation=false` unless block peaks force it.
3. **Sample-rate-aware STFT profiles.** Scale analysis/synthesis windows with sample rate so 96 kHz does not silently lose resolution.
4. **Time-domain lows for deep drop** (see live -12 path). Not a shallow-only SOLA experiment: this is the likely way to hit <=8-10 ms on bass.

## Live -12 path

Commercial bar is ~7 ms (HyperTune) to ~18 ms (Archetype, under-reports PDC). Our STFT LL is already in the 13 ms neighbourhood on guitar with correct pitch at -12. Closing the remaining gap is quality-then-latency, not the reverse.

1. **Laroche-Dolson identity phase locking** in `transpose_stft` polyphonic mode -- quality without extra latency; may allow slightly smaller windows later. There is no phase-lock in the current `StftPitchShift` wrapper.
2. **Retune STFT profiles for deep down (-8 to -12)**, not only `abs(st)` buckets. Keep shallow profiles gated off |st| <= 2.
3. **Re-measure -12 guitar and bass** after phase lock. Only then try shrinking toward 8-10 ms. Do not shrink windows to "fix" bass fundamentals.
4. **True hybrid (likely -12 winner on bass).** Time-domain SOLA/resampling **below ~400-800 Hz**, locked STFT above, keep dry-transient assist. Current dual-STFT hybrid is not this. Fold transient-assist into single-band STFT if a full redesign is not justified by data.
5. **Promote** the winning low-latency engine out of experimental only if A/B beats Signalsmith for live deep drop **and** matches HyperTune on mono, chords, bass, and attacks without Hybrid-class CPU.

Do **not** spend a major cycle making Signalsmith do live -12.

## Product / default strategy (after quality work)

Keep one global node id (`global_transpose`) and the existing UI knob. Route inside the processor (or a thin wrapper that still registers as `kTranspose`):

| Use | Engine |
|---|---|
| 0 st | Dry copy, 0 latency (unchanged contract) |
| Shallow live (abs(st) <= 2, optionally <= 3) | Signalsmith (semitone-aware config when that lands) |
| Global / live drop to -12 | Winning low-latency path (locked STFT, or true hybrid if bass requires it) |
| Max quality / offline | Signalsmith HQ |
| FX library `transpose_stft` / `transpose_hybrid` | Stay experimental until the live path wins A/B |

`pitch_shift` remains the continuous FX-library shifter (Signalsmith); do not silently swap its engine for STFT.

## Validation: Transpose Benchmark Harness

An offline benchmark renders demo audio through every variant at multiple semitone settings and produces an HTML report comparing snapshots (revisions) side by side -- latency (reported vs measured), throughput, pitch accuracy, and rendered audio for listening tests.

- Renderer: `core/tests/TransposeBenchmark.cpp` (CMake target `TransposeBenchmark`, ctest label `benchmark`, excluded from fast test runs).
- Report generator: `tools/transpose-benchmark/generate_report.py` (Python stdlib only).
- Pipeline: `tools/transpose-benchmark/run_benchmark.ps1`.
- Inputs today: `core/ui/demo/guitar-riff-01.wav`, `guitar-riff-02.wav`, `DI_Guitar_L.wav` (trimmed to 12 s, native sample rate, 512-sample blocks). **Bass + chord fixtures are required before claiming the goal.**
- Latency is measured two ways: `GetLatencySamples()` (what PDC would use) and envelope cross-correlation against the dry signal (ground truth). Rendered WAVs are compensated by the *reported* latency, so any PDC misreport is audible in the report.
- **Pitch accuracy** is measured per pass: `pitchErrorCents` (median cents error of the output fundamental vs the requested interval), `pitchJitterCents` (robust MAD-scaled spread) and `pitchFrames` (usable frames). See below.
- External references: `tools/transpose-benchmark/external-plugins.json` (HyperTune Metal, Archetype Misha Mansoor X) via pedalboard.

### Pitch accuracy metric

An engine can report honest latency, sit in a sensible CPU budget and render audio that *sounds* like a pitch shifter while not actually transposing by the requested interval. Latency/CPU/peak/RMS metrics are all blind to this -- it took a dedicated measurement to find that the STFT path was ~200 cents off at -2 st.

How it works (`MeasurePitchAccuracy` in `core/tests/TransposeBenchmark.cpp`):

1. Dry and output are mixed to mono and decimated to ~8 kHz (guitar fundamentals are well under 500 Hz; this shrinks YIN's search space ~6x). **Bass fixtures need a lower analysis band / less aggressive decimation.**
2. Both are tracked with YIN (cumulative mean normalized difference + parabolic refinement) on a common grid: 128 ms frame, 32 ms hop.
3. The output is aligned using the **measured** latency when the cross-correlation was trustworthy, falling back to the reported value -- the metric must stay honest when the reported latency is not.
4. The output's YIN search range is shifted by the requested interval, which keeps octave confusion out of the result.
5. Per frame where both signals are voiced: `1200 * log2(f0_out / (f0_dry * 2^(st/12)))`. Errors beyond +/-600 cents are discarded as tracker octave artifacts.
6. Reported as the median, with a MAD-scaled spread as jitter.

Reading the numbers:

| Result | Meaning |
|---|---|
| within +/-5 cents, jitter < 10 | correct transposition |
| +/-25 cents | audible tuning error, flagged `warn` in the report |
| error approx -(interval in cents) | **not transposing at all** -- output is at the input pitch |
| `n/a` | fewer than 10 usable frames; treat as no data, not as a pass |

Every 0 st pass must read `0.0 +/- 0.0` -- that is the metric's self-check. Coverage depends on the tracker locking onto the output, so a low `pitchFrames` count on an otherwise healthy-looking pass is itself a signal that the output is not periodic.

### Running the benchmark

**Quick start -- use the pipeline script (recommended):**

```powershell
# From repo root. Builds, runs, auto-renders external plugin passes, generates report.
.\tools\transpose-benchmark\run_benchmark.ps1

# All three demo samples + open report in browser when done:
.\tools\transpose-benchmark\run_benchmark.ps1 -AllDemoAudio -OpenReport

# Skip rebuild when the binary is already current (faster iteration):
.\tools\transpose-benchmark\run_benchmark.ps1 -NoBuild

# Skip auto-render (e.g. plugins not installed; use manual WAVs from external-renders/ instead):
.\tools\transpose-benchmark\run_benchmark.ps1 -NoAutoRender

# Debug binary, custom output directory:
.\tools\transpose-benchmark\run_benchmark.ps1 -BuildConfig Debug -OutputRoot C:\bench-out
```

The script wraps all steps below. Run it once to get everything; use the manual steps when you need finer control.

**Manual steps:**

```powershell
# 1. Build (Debug for a smoke test; use Release for meaningful perf stats)
cmake --build core/build --config Release --target TransposeBenchmark

# 2. Run one snapshot -- label it with the git rev or a descriptive name.
#    By default this renders only the first demo riff (faster iteration).
core\build\Release\TransposeBenchmark.exe transpose-benchmark-out <snapshot-label>

# 2b. Optional: render all demo audio instead of the default first riff
core\build\Release\TransposeBenchmark.exe --all-demo-audio transpose-benchmark-out <snapshot-label>

# 3a. Auto-render external plugin passes via pedalboard (requires: pip install pedalboard)
python tools/transpose-benchmark/render_external_passes.py transpose-benchmark-out/<snapshot-label>

# 3b. Compute metrics for each auto-generated manifest
python tools/transpose-benchmark/build_external_passes.py transpose-benchmark-out/<snapshot-label> transpose-benchmark-out/<snapshot-label>/auto-manifest-<pluginId>.json

# 3c. Optional: import manual DAW renders instead (see "Including external plugin passes" below).
python tools/transpose-benchmark/build_external_passes.py transpose-benchmark-out/<snapshot-label> tools/transpose-benchmark/external-renders/<pluginId>.json

# 4. Generate/refresh the report (aggregates all snapshots under the output root)
python tools/transpose-benchmark/generate_report.py transpose-benchmark-out

# 5. Open the report
start transpose-benchmark-out\report.html
```

### Including external plugin passes

External plugin results can be added automatically or manually.

**Automated (recommended) -- via `render_external_passes.py`:**

The pipeline script calls `render_external_passes.py` automatically. It reads
`tools/transpose-benchmark/external-plugins.json`, loads each VST3 plugin via
[pedalboard](https://github.com/spotify/pedalboard), sets the transpose
parameter at each configured semitone, processes the demo audio, and writes
manifests into the snapshot directory.

Requirements:

```powershell
pip install pedalboard
```

Plugin definitions in `external-plugins.json` must include:

- `pluginPath`: absolute path to the installed `.vst3` file
- `semitones`: list of integer semitone values to render
- `transposeParameter.name`: plugin parameter name (e.g. `"Transpose"`)
- `transposeParameter.mapping`: `"text"` (scan string values) or `"normalized"` (assume +/-24 st linear range)

Optional:

- `parameterOverrides`: array of `{ "name": "...", "value": 0.0 }` (normalized 0..1) or `{ "name": "...", "text": "Off" }` applied once after load. Used for Archetype to disable amp/cab/gate/FX sections so only Transpose remains for a fair comparison.

**Manual fallback -- via DAW renders:**

If a plugin cannot be loaded headlessly (e.g. requires GUI activation), pass `-NoAutoRender` and render manually:

1. **Render audio** through the external plugin at each semitone setting using the same dry sources (including new bass/chord fixtures once they exist).
2. **Copy the rendered WAVs** into the snapshot directory under `external/` (e.g. `transpose-benchmark-out/<snapshot>/external/HyperTune_m12.wav`).
3. **Write a render manifest** and drop it in `tools/transpose-benchmark/external-renders/`. See `tools/transpose-benchmark/external-renders.example.json` for the schema:

   ```json
   {
     "pluginId": "hypertune_vst3",
     "entries": [
       { "sample": "guitar-riff-01.wav", "semitones": -12,
         "wav": "external/HyperTune_guitar-riff-01_m12.wav", "reportedLatencySamples": 0 },
       { "sample": "guitar-riff-01.wav", "semitones": 12,
         "wav": "external/HyperTune_guitar-riff-01_p12.wav", "reportedLatencySamples": 0 }
     ]
   }
   ```

   - `pluginId` must match an entry in `tools/transpose-benchmark/external-plugins.json` for label/path metadata to be resolved automatically.
   - `wav` is relative to the snapshot directory.
   - `reportedLatencySamples` should reflect the plugin's reported PDC value (0 if unknown).

4. **Run the pipeline script** -- it discovers all `*.json` files in `external-renders/` and calls `build_external_passes.py` for each one, producing `external-passes-<pluginId>.json` in the snapshot directory. `generate_report.py` then merges these into the HTML report automatically.

### Comparing revisions

1. On the baseline revision, run the benchmark with a label like `baseline-<git-rev>`.
2. Apply a change, rebuild, and run again with a new label.
3. Re-run the report generator -- each snapshot becomes a column, with delta-latency warnings highlighted and audio players for A/B listening at each semitone setting.

**Gate metrics for each experiment:** see Numeric gates above. Always include -2 (shallow structural failure) and -12 (live goal) on guitar **and** bass.

Notes:

- Debug-build timing numbers (realtime factor, block us) are pessimistic; use `--config Release` for performance comparisons. Latency and audio output are valid in either config.
- Default runs render only the first riff (`guitar-riff-01.wav`) for faster iteration. Use `--all-demo-audio` when validating across all demo inputs.
- A full all-demo run is ~153 passes today; expect more once bass/chord fixtures land.
- Output directories under `testing/transpose-benchmark/` and `transpose-benchmark-out/` are benchmark artifacts and should not be committed.

## Suggested Order of Work

1. ~~Fix Signalsmith `GetLatencySamples()` (input+output), 0 st -> 0, dry delay for mix; same for Octave/AutoArp.~~ **Done.**
2. ~~Re-verify Pitch Shift actually shifts once PDC is honest.~~ **Done** (pitch-metric snapshot).
3. **Harden the proof harness** (defect #8): bass + chord fixtures, YIN floor, attack metric, finer latency, 64/128 block CPU, 96 kHz check, HyperTune on the same report. Take a new labelled baseline before any DSP change.
4. Unify tonality limits (defect #2) -- cheap, do not block #3.
5. Gate STFT/hybrid off |st| <= 2 in the engines themselves (defect #5) so experimental use cannot silently output a detuned dry copy.
6. **STFT phase lock + deep-down profile retune**; A/B vs HyperTune @ -12 guitar **and** bass against the new gates.
7. If bass -12 still fails quality at <=10 ms: **true hybrid** (time-domain lows + locked STFT highs). Do not iterate the current dual-STFT hybrid.
8. Semitone-aware Signalsmith for **shallow** shifts only; A/B @ -2.
9. **Router in global transpose**: shallow -> Signalsmith, deep live -> winning low-latency engine, 0 st unchanged. Same node id / UI knob.
10. Promote the live engine out of experimental only with data. Then resolve range/contract drift (defect #3) -- still no expand past +/-12 until that engine is the default.

## What not to do next

- Promote hybrid as the default path on current dual-STFT evidence.
- Enlarge STFT windows to "fix" bass or shallow pitch (buys latency, does not hit the 8-10 ms bar).
- Shrink STFT windows without phase locking and without attack+bass metrics.
- Spend major effort making Signalsmith do live -12.
- Expand global range past +/-12 before the live engine is solid.
- Enable Signalsmith `splitComputation` "for quality" -- it only adds latency for smoother CPU.
- Call a path correct because latency/CPU look healthy while pitch is `n/a` or ~200 cents off.

## Manual Listening Review

### first-pass

- all: Pitch Shift effect does not shift. **Superseded:** pitch metric shows it does; likely PDC misalignment at the time.
- -12 st: All variants sound ok with basic riff
- -7 st: Transpose STFT polyphonic, Transpose (Signalsmith) sound best. Former has best latency
- -2 st: Transpose (Signalsmith) is the only version with reasonable sound. Pitch Shift effect does not shift. **Superseded for Pitch Shift; STFT at -2 still does not shift.**

### Current Archetype VS HyperTune VS Ours

- -12: Our Transpose Hybrid has the most artifacts for polyphonic input. Our STFT low latency and polyphonic sound like HyperTune and Archetype for monophonic and polyphonic **guitar riffs**. Bass untested.
- -7: Our STFT starts to have artifacts; Transpose (Signalsmith) sounds most like HyperTune. HyperTune has slightly more presence
