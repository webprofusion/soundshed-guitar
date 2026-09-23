# Audio A/B between app versions

`audio-ab.mjs` answers one question: **does anything sound different between two versions
of the app?** It renders every effect at its defaults, and a set of whole signal chains
through the app at its default settings, for both versions. Then it compares the renders and
says which effects and chains changed, how (level, tone, waveform, tail, latency) and whether
a player would hear it.

```powershell
node tools/audio-ab/audio-ab.mjs 1.5.0              # the public release vs your working tree
node tools/audio-ab/audio-ab.mjs HEAD               # what your uncommitted changes do to the sound
node tools/audio-ab/audio-ab.mjs 1.5.0 main         # any two revisions
node tools/audio-ab/audio-ab.mjs 1.5.0 --rate 44100 --signals di,di_hot
node tools/audio-ab/audio-ab.mjs --help
```

A revision is anything `git rev-parse` accepts, or `WORKTREE` (the default target) for this
checkout as it stands, uncommitted changes included. The console summary lists everything
that changed. `report.html` holds the detail, with base, target and difference players for
each changed case, and `report.md` and `compare.json` sit beside it.

## How it works

For each revision the driver:

1. **Checks it out** as a detached git worktree under the work directory. `WORKTREE` uses
   this checkout directly.
2. **Copies in this checkout's harness**: `core/tests/AudioSnapshot.cpp`,
   `AudioSnapshotChains.h`, `AudioSnapshotSupport.h` and `AudioSnapshot.cmake`. If that revision's
   `core/tests/CMakeLists.txt` does not include `AudioSnapshot.cmake`, the driver adds the
   include. Both sides therefore run the same harness, so a difference can only come from
   the DSP.
3. **Builds `AudioSnapshot` in Release**, which is what ships and what `/fp:fast` and
   `-ffast-math` apply to. The build tree is the revision's own and lives under the work
   directory. A revision that pins the same dependencies as this checkout reuses
   `core/build/_deps`; otherwise it fetches its own, as the NAM core, Eigen and the resampler
   have moved between releases. `npm` is replaced by a no-op because the harness doesn't
   need the UI bundle.
4. **Renders** with the stimuli and test resources from *this* checkout, so both sides get
   identical input.

Renders of a committed revision are cached. The cache key covers the commit, the harness,
`snapshot.json`, the inputs and the options, so comparing against the same base again only
re-renders the working tree. The first run for a revision is the slow one: 1.5.0 took about
six minutes (two to configure and fetch, three to build, 40 s to render). After that, builds
are incremental and a full render takes about 30 s.

### What is rendered

**Effects, each alone at its defaults.** Every registered effect type gets its own
`SignalGraphExecutor`. Its parameters are the ones a node the user has just added gets: every
registered default, then the effect's nominated factory preset, else its first factory preset.
This mirrors `PluginControllerSignalPath.cpp`. The NAM types also get the interface calibration
the controller injects at default settings: 12 dBu, enabled. Effects that need a file get the
test resources named in `snapshot.json` under `effectResources`.

**Whole chains through the app.** Each chain in `snapshot.json` is built as a preset and sent
to a real `PluginController` with a `loadPreset` message. The controller runs standalone with
a fresh, empty profile, which makes every setting the app's default: mono input on channel 1,
limiter off, NAM calibration on at 12 dBu, the global chain present but switched off. The
render goes through the controller's own audio callback, so it covers the input stage, the
mixer, the global chain and the master too. Each render is primed with a second of silence,
so the preset's fade-in has finished before the stimulus starts.

Renders run with denormals flushed to zero, as the app's audio callback does
(`juce::ScopedNoDenormals`). Every case gets a fresh engine. The first stimulus is rendered
twice; if the two renders differ, the case is marked not deterministic and is judged on
level, tone and tail only.

| Stimulus  | What it is                                                                  |
| --------- | --------------------------------------------------------------------------- |
| `di`      | 6 s of the demo DI guitar (`core/ui/demo/DI_Guitar_L.wav`, from 12 s, faded in and out) + 2 s tail |
| `di_hot`  | the same, 12 dB hotter, to push drives and amps                              |
| `sweep`   | 4 s exponential sine sweep, 20 Hz to 20 kHz at −12 dBFS, + 1 s tail          |
| `impulse` | a −6 dBFS impulse at 10 ms, + 1.5 s tail                                     |
| `silence` | 1.5 s of digital silence: self-noise, hum, DC                                |
| `noise`   | 3 s of white noise at −20 dBFS rms, + 1 s tail                               |

The default set is `di, sweep, impulse, silence`, taken from `snapshot.json`.

### Reading the verdicts

| Verdict     | Meaning                                                                    |
| ----------- | -------------------------------------------------------------------------- |
| `identical` | bit-identical                                                              |
| `rounding`  | plain null below −120 dB: float noise                                      |
| `inaudible` | measurably different, but below every threshold below                      |
| `slight`    | may be audible side by side                                                |
| `audible`   | a player would hear it                                                     |
| `broken`    | non-finite samples, output fell silent, or a render is missing             |
| `new` / `removed` | the effect or chain exists on one side only                          |

The measures, taken per channel, with the worst channel reported (`analysis.mjs`):

- **Level**: rms change over the render. Slight at 0.1 dB, audible at 0.5 dB.
- **Tone**: the biggest change in any third-octave band within 40 dB of the loudest band,
  beyond the overall level change, so a pure gain change counts as level only. Slight at
  0.3 dB, audible at 1 dB.
- **Character**: the gain-matched null, which is what is left after aligning the target to
  the base and matching its gain. Slight above −40 dB, audible above −20 dB. A bad null with
  unchanged level and tone means the waveform itself changed: a new nonlinearity, a
  different modulation, a timing change.
- **Tail**: rms after the stimulus ends. For `silence`, that is the self-noise. Slight at
  0.5 dB, audible at 1.5 dB. A tail that appears out of digital silence is always flagged:
  audible above −70 dBFS, slight below it.
- **Latency**: the lag that best aligns the two renders, noted only when the aligned
  waveforms match (a gain-matched null below −20 dB), alongside any change in an effect's
  reported latency. This is a note, never a verdict, but a pure latency change is at least
  `inaudible`, never `rounding`.

Nothing quieter than −100 dBFS raises a flag, and a change that is itself quieter than
−70 dBFS (a residual, a band, a tail) is at most `slight`. Override any threshold with
`--threshold name=value`; the names are in `DEFAULT_THRESHOLDS` in `analysis.mjs`.

The report also lists **definition changes** between the two registries: renamed or retired
effects, and changes to the parameters a new node gets, their ranges, units, steps, choices
and tapers. A retired type is compared against the type that now answers to its alias; for
example, `amp_nam` is compared against `amp_nam_optimized`.

Exit status is 1 when a subject is `broken`, or at least as bad as `--fail-on`
(`broken`, the default, or `audible`, `slight`, `never`). Status 2 means the tool itself
failed.

## Adding a chain

Add an entry to `chains` in `snapshot.json`:

```json
{
  "id": "my-rig",
  "name": "My rig",
  "description": "Shown in the report.",
  "globalChain": { "gate": true, "eq": true },
  "nodes": [
    { "type": "overdrive" },
    { "type": "amp_nam_optimized", "resource": "nam-drive" },
    { "type": "cab_ir", "resource": "ir", "params": { "mix": 0.8 } },
    { "type": "reverb_room", "enabled": false }
  ]
}
```

Name node types by alias (`core/protocol/effect-aliases.json`) so that one definition serves
every revision. If a revision lacks a type, that chain is reported as `new` or `removed` for
it. A node gets fresh-node parameters, then its own `params` on top.
`"freshParams": false` gives it only its own `params`, which is how the UI's New Preset
template sends them. Nodes run in series unless the chain gives `edges`; see `wet-dry`, which
uses a splitter and a mixer node with `toPort` for each mixer input. Resources are named in
`resources`, with paths relative to `core/tests/testdata/assets`.

Adding or changing a chain invalidates the render cache for every revision.

## Changing the analysis

The verdict rules live in `analysis.mjs` and are pinned by synthetic cases in
`analysis.test.mjs`: `node --test tools/audio-ab/analysis.test.mjs`. To re-judge renders you
already have without building or rendering anything, run
`node tools/audio-ab/audio-ab.mjs report <renderDirA> <renderDirB>`.

## Keeping the harness buildable against old revisions

The harness is compiled against every revision it compares, so it can only use APIs those
revisions have. Everything it calls has been stable since 1.5.0: `SignalGraphExecutor`,
`EffectRegistry`, `PluginController`'s `Initialize`/`Prepare`/`HandleUIMessage`/`ProcessAudio`,
`PresetStorage::SerializeToJson` and `IPluginHost`. Where a newer field or method is useful,
reach it through a `requires` check in a template, as `FreshNodeParams` does for
`EffectPresetDefinition::isDefault`. The check compiles away against a revision that lacks
it. If a base revision fails to build, the driver says so and shows the compiler errors.

## Files and disk

Everything lives in the work directory: `../<repo>-audio-ab` by default, or `--work-dir`, or
`SSG_AUDIO_AB_DIR`.

| Directory     | Holds                                                               |
| ------------- | ------------------------------------------------------------------- |
| `src/<sha>`   | one git worktree per revision                                       |
| `build/<sha>` | its Release build tree (`build/worktree` for this checkout), 0.7–1.2 GB each |
| `renders/`    | float WAVs, `manifest.json`, `effects.json`: about 190 MB per revision at 48 kHz |
| `reports/`    | `report.html`, `report.md`, `compare.json`, and the base, target and difference audio of every changed case, so a report plays what it measured even after the renders are replaced (1.5.0 against today: about 200 MB) |
| `logs/`       | configure, build and render logs                                    |

`node tools/audio-ab/audio-ab.mjs list` shows what is cached. `clean` removes the worktrees
and builds, and `clean --all` removes the renders and reports as well.

## What it does not cover

- Plugin mode. Chains run standalone. A DAW host sets its own input configuration.
- The WASM host and hosted third-party plugins. There is no test module or plugin, so the
  WASM host renders as a passthrough.
- Composites and blends from the user's library, and factory presets that need resources
  outside `core/tests/testdata/assets`.
- UI-side defaults beyond what `snapshot.json` spells out. The New Preset chain copies
  the engine's `BuildNewPreset` (`controller/PluginControllerPresetEdits.cpp`) by hand, so
  keep it in step.
- Controller policy in the effects pass. That pass mirrors the controller's defaults (the
  12 dBu NAM calibration) rather than running the controller. The chains pass runs the real
  controller, so a policy change shows up there.
