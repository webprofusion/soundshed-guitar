# Dispersal

A long chain of all-pass filters, all tuned around one frequency, that delays that part of the
spectrum far more than the rest. Nothing is boosted or cut: the magnitude response is flat and
only the timing changes. A pick attack is a click, which holds every frequency at once; after
the chain its frequencies arrive one after another, so the click is smeared out into a sweep —
a "pew", a laser zap, a dub-siren drop. Palm mutes and staccato riffs zap on every note, a
picked chord blooms and a sustained note sounds nearly dry, because a steady tone only changes
phase. With Mix below 1 the dry signal comes back and the two comb against each other, which
is a very deep phaser; with an LFO on Frequency it becomes one that moves.

Effect id `dispersal`, GUID `83377dc3-04a5-416a-afd7-f75cdf188bba`, category `modulation`.
Engine: `core/src/dsp/effects/DispersalEffect.h` (+ `DispersalSupport.h` for the parameter
specs, the stage design and factory presets, on the Ring Modulator's split).

Status (2026-10-30): Phase 1 is built, uncommitted, and tested in Debug and Release; it has
not been heard in the running app yet. Phases 2–4 are not started. docs/fx-library.md has
the effect as built.

## Signal flow

```
in ─┬─ AP(f₀,Q) ─ AP(f₀,Q) ─ … ─ AP(f₀,Q) ─┬─ tap N (crossfaded on a change of Stages) ─ mix ─ out
    │  ← Stages second-order all-passes →   │                                             │
    └───────────────────────────────────────┴──────────────── dry ────────────────────────┘

 per stage, at its centre f₀:   τ ≈ 2Q / (π f₀)         chain:  N · τ
```

- **Second-order all-passes, not first-order.** A first-order all-pass delays everything below
  its corner, so a chain of them is a low-pass-shaped delay that smears the lows. A second-order
  stage concentrates its delay around f₀, with a width set by Q, so the chain delays one band
  and leaves the rest in time: the focused sweep is the effect. Peak group delay of one stage is
  4Q/ω₀ = 2Q/(πf₀); at 800 Hz and Q 1 that is 0.80 ms, so the default 64 stages hold 800 Hz back
  by about 51 ms. Toward DC a stage's delay only falls to 1/(Qπf₀), half its peak at Q 1, so
  at a low Focus the lows are held back too (200 Hz by 29 ms at the defaults) and only the
  highs get through early; a high Focus confines the delay to the centre.
- **Topology: the TPT state-variable filter** (trapezoidal SVF, all-pass output `x − 2k·band`).
  Frequency is modulated (LFO and envelope, Phase 2), and in a chain of up to 256 stages any
  energy a direct-form biquad gains while its coefficients move is compounded 256 times. The
  SVF's state stays bounded under modulation. It costs about six multiplies a stage-sample
  against a direct form's four or five. A test holds the chain to its input's level under the
  fastest modulation (Tests, below), so if a cheaper form passes it, it can replace the SVF.
- **One design for the whole chain.** Every stage shares f₀ and Q, so a change of Frequency or
  Focus recomputes three coefficients, not 256 sets. Coefficients move every 16 samples, glided
  over 20 ms (one `tan` per tick); no per-sample `tan`.
- **Stages are integer.** A change crossfades over 30 ms from the old count's tap to the new
  one, running the chain to the larger of the two while it fades. A half-way blend of two taps
  one stage apart has a notch at f₀ (that stage turns f₀ through 180°), so the fade is short and
  Stages is a knob to set, not to sweep: automating it steps, and no modulation source reaches
  it. Stages beyond the count are not run, so few stages are cheap; a stage is cleared when it
  drops out, so growing the count again starts it silent rather than replaying stale state.
- **Stereo** shifts the right channel's f₀ by up to an octave either way, so the zap reaches
  the two sides at different moments and the image spreads. At 0 the two sides are identical.
  No other link between the channels, so the type keeps them separate
  (`KeepsChannelsSeparate()` true). `CanWiden()` stays true, because Stereo can widen.
- **Mix** blends the dispersed signal with the dry, glided over 20 ms. Mix 1 is the default
  (a pure all-pass, level-matched with bypass by construction); in between the two comb.
- **No latency**: an all-pass delays frequencies relative to each other, which is the sound,
  so nothing is reported and nothing compensated.
- **Denormals**: each stage's state is flushed below 1e-9, as `reverb::ChirpDispersion` does,
  so a chain ringing out into silence does not stall on subnormals.
- **Tail**: at high Focus and a low Frequency the chain rings for a long time (256 stages at
  100 Hz and Focus 4 is over 6 s of peak delay). The mixer's sleep-on-silence
  (`SilenceSleepGate.h`) waits for the output to fall quiet as well as the input, so it does
  not cut the tail, and the node reports none.
- **Rejected: rendering the chain to an IR and convolving it.** That costs the same at any
  Stages, but it is static, and moving Frequency is half the effect.

## Parameters

| Key | Name | Range | Default | Notes |
| --- | --- | --- | --- | --- |
| `stages` | Stages | 1…256, step 1 | 64 | `LogTaper`: 1–16 gets real travel; crossfaded, never modulated |
| `frequency` | Frequency | 100…10000 Hz | 800 Hz | `LogTaper`; centre of every stage |
| `focus` | Focus | 0.3…6 | 1.0 | `LogTaper`; the stages' Q. Higher: a narrower, more tonal, longer zap |
| `stereo` | Stereo | −12…+12 st | 0 | right channel's f₀ offset |
| `mix` | Mix | 0…1 | 1 | |

Read-only: `GetParam("peakDelayMs")` answers N·τ at the current settings (the group delay at
f₀, from the digital design rather than the analogue formula). The UI has no generic way to
show a read-only value under the knobs, so the readout ("800 Hz held back 51 ms") comes with
the group-delay plot in Phase 3 instead.

Stages is the first stepped parameter on a log taper. Both UIs labelled a log-tapered value
before checking for whole steps, so it read "64.0"; `paramValueFormat.ts` and Nano Q's
`ParamFormat.cpp` now check whole steps first, which no other parameter is affected by.

Parameters live in one `constexpr std::array<EffectParamSpec, N>` in `DispersalSupport.h`
(`BuildParameterDefs`, `FindParamSpec`, `NormaliseParamValue`), as the Ring Modulator's do.

## Phases

### Phase 1 — the effect

- `DispersalEffect.h` / `DispersalSupport.h`: the chain, Stages crossfade, the glides, Stereo,
  Mix, `peakDelayMs`. The first block snaps every glide onto its target, so a preset loads at
  its settings instead of sweeping in from the defaults (as the Ring Modulator does).
- Registration: GUID in `EffectGuids.h` and `core/ui/ts/effectGuids.ts`; `BuiltinEffects.h`;
  `core/protocol/effect-aliases.json` and the UI's `EFFECT_ALIAS_MAP` (alias `dispersal`);
  an icon in `core/ui/data/effect-presentation.json`, then `node tools/gen-effect-presentation.mjs`.
- Factory presets, four or five: *Laser* (64 stages, 1.2 kHz, Focus 2), *Dub Drop* (160,
  300 Hz, Focus 3), *Soft Attack* (24, 2 kHz, Focus 0.5: rounds pick attack without a zap),
  *Wide Zap* (96, 900 Hz, Stereo +7), *Deep Phase* (32, 600 Hz, Mix 0.5).
- `docs/fx-library.md`: a "Dispersal (`dispersal`)" section and its row in the UUID table.
- Measure the cost (below) before deciding whether Phase 4 is needed.

The node panel is the generic one: knobs in a row. Nano Q gets the same generic panel.

Built as planned, with these differences: Comb Phase was added as a sixth preset (8 stages,
600 Hz, Focus 0.7, Mix 0.5), and *Dub Drop* is 96 stages at 250 Hz and Focus 2 (0.49 s)
rather than 160 at 300 Hz and Focus 3, which held the centre back over 1.6 s, longer than
most notes. The default preset is *Zap* (the defaults). `StageGroupDelaySeconds` (the
group delay at any frequency, exact for the digital design) landed now for the tests, and
is what Phase 3's plot will draw.

Still to do for Phase 1: listen to it in the Standalone on the demo DI riffs, and an
`audio-ab` run (`node tools/audio-ab/audio-ab.mjs HEAD`) once it is committed.

### Phase 2 — movement

- **LFO on Frequency**: Rate (0.05–8 Hz, `TempoSync` divisions like the Phaser's), Depth in
  octaves (0–3, so a sweep sounds even around any setting), Shape (sine, triangle). With a
  long chain the LFO's movement is itself smeared in time, which is what gives this its
  liquid, doppler-ish sound; Depth 0 is off and costs nothing.
- **Envelope sweep**: an envelope follower on the input moves f₀ by up to ±3 octaves (Sweep),
  with Attack/Release. Pick hard and the zap starts higher and falls (or the reverse), the
  Dispersal's version of an auto-wah. One follower for both sides, so this links the channels:
  `SetDualMono(true)` gives each side its own (docs/plans/stereo-input.md, Part 2) and
  `KeepsChannelsSeparate()` stays true.
- **Spread**: spaces the stages' centres across up to two octaves around Frequency instead of
  stacking them all on f₀, which turns a focused zap into a broader smear or a rising/falling
  chirp. This breaks the shared design: 256 coefficient sets per tick. Use a cheap `tan`
  approximation (the stages' frequencies are a geometric series) and measure it.

### Phase 3 — show the delay

The magnitude curve that `getEffectResponse` draws is flat for this effect, so it says
nothing. The plot worth drawing is group delay against frequency: a peak at f₀ whose height is
`peakDelayMs` and whose width is Focus.

- Add an optional virtual to `EffectProcessor`, `GetGroupDelay(frequenciesHz, delaysMs)`,
  default false, alongside `GetFrequencyResponse`. The Dispersal answers from its digital
  stage design (one stage's group delay times N), so the curve cannot drift from what plays.
- `getEffectResponse`'s reply gains an optional `groupDelayMs` array (absent for effects that
  have none, so older UIs ignore it); `core/protocol/ui-messages.json` and
  `docs/user-interface.md` record it, and `node tools/check-protocol.mjs` must pass.
- `effectResponse.ts`'s `ResponseCurve` gains `groupDelayMs?`; a small panel plot (ms on a log
  frequency axis) on the same coalesced lane as the other response plots.
- Nano Q: the same plot as a native widget, once the web one has settled.

### Phase 4 — speed, only if Phase 1 says so

The chain is serial: stage k at sample t needs stage k−1 at sample t. Within one block,
though, the input to a group of stages is all known, so a group of 4 (SSE/NEON) or 8 (AVX2)
consecutive stages can run as a wavefront — lane j runs stage s+j on sample t−j, taking lane
j−1's result from the previous step — and the skew lives inside the block's prologue and
epilogue, adding no latency. Stereo can also share a vector. Write the lanes as named locals,
not an array (MSVC ARM64 spills arrays), and keep `std::clamp`/`std::abs` out of the loop.

Only worth doing if it saves at least 10% of a realistic chain's audio-thread time; a few
per cent of the deadline at the default 64 stages does not qualify.

### Later, not planned

- **Feedback around the chain with a delay** (a dispersive echo: every repeat a longer chirp,
  springy and boingy). The spring reverbs already build this from `reverb::ChirpDispersion`;
  as a Dispersal mode it would need a Time and a Feedback control and its own stability work.
- A Dispersal stage inside the Phaser instead of a separate effect: no, the Phaser's four
  stages and its feedback are a different sound and preset ranges would clash.

## Cost (measured in Phase 1)

`DispersalEffectTests` in Release (MSVC, `/fp:fast`), one 64-sample block at 48 kHz, 2 s of
noise:

| Stages | Mono input (sides linked) | Stereo input | Share of 1333 µs |
| --- | --- | --- | --- |
| 64 (default) | 17.9 µs | 21.3 µs | 1.3–1.6% |
| 256 (max) | 70.7 µs | 71.9 µs | 5.3–5.4% |

About 4.4 ns, some 17 cycles, per stage-sample for one chain: the loop is bound by the latency
of each stage's recursion, not by arithmetic. That is why linking the sides saves so little —
the second chain runs in the first one's shadow — and it is what Phase 4 would exploit: two
more chains in flight (a one-sample skew between neighbouring stages) should cost almost
nothing. At the default 64 stages the gain is under 10 µs, a few per cent of a realistic chain,
so it waits for a reason (a preset that needs 256 stages, or a slow device).
Not yet measured on `SteadyStateProfiler` or on ARM.

## Tests (`core/tests/DispersalEffectTests.cpp`)

- **All-pass**: white noise through 1, 64 and 256 stages at several Frequency/Focus settings
  keeps its RMS within 0.05 dB, and sines across 50 Hz–16 kHz keep their steady-state level.
- **Delay where designed**: narrow tone bursts at f₀ and two octaves either side arrive as
  late as `StageGroupDelaySeconds` says (energy centroid, within 3%; measured within 0.05 ms),
  the centre latest, and `peakDelayMs` is that delay at f₀.
- **Stable under modulation** (Phase 2, but the harness lands in Phase 1 with Frequency
  automated block by block): 256 stages, Focus 6, Frequency swept across its whole range at
  the fastest rate for 10 s of noise: output finite throughout and its RMS within 0.5 dB of the
  input's.
- **No click on a Stages change**: steps of 1→256→32 during a sine; the largest sample-to-sample
  jump stays within a small multiple of the steady sine's.
- **Presets load without a sweep**: parameters set before the first block are in force at
  sample 0.
- **Stereo 0 keeps the sides identical**; Stereo ≠ 0 makes them differ. `ChannelLayoutTests`
  covers `KeepsChannelsSeparate`.
- **Release and clang**: run the finite checks with `/fp:fast` and in the clang-cl tree, using
  `FiniteCheck.h` (`FastMathNanTests` traps).
- `EffectAliasParityTests` and `tests/effectAliases.test.ts` pick the alias up; `npm run verify`
  covers presentation and protocol.
- `node tools/audio-ab/audio-ab.mjs HEAD` renders the new effect alone at its defaults; listen
  to it on the demo DI riffs, not only the test signals.

## Open questions

Settled 2026-10-30: the effect is called Dispersal, id `dispersal`, a name of our own. It was
built under a working name and renamed before anything was committed or saved with it, so no
alias is kept for the old id.

1. **Stage ceiling.** 256 is enough for long dub drops at low frequencies; going higher costs
   linearly and mostly makes the zap longer than a note. Raise it only if Phase 4 lands.
2. **Cap on total delay.** At 100 Hz and Focus 6, 256 stages hold the centre back by about
   10 s, which sounds like nothing happening. Either cap the peak delay (lower the effective
   Q when N·τ passes, say, 2 s) or leave it uncapped and let the readout explain it.
   Recommendation: leave it and show the readout.
