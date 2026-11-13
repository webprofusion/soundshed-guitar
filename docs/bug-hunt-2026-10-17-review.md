# Review of the 2026-10-17/18 bug-hunt fixes

Scope: every commit in `44f147e0..aed5d6af` on main (56 non-merge commits; 54 fixes, one
test follow-up, one CI change). Reviewed 2026-10-18 by five read-only passes, one per
area, each tracing the pre-fix code (`git show <hash>^:<path>`) for whether the described
symptom was reachable, then judging the fix for size. Spot-checked by hand: 41a9b30d,
7007f2cd, 9fce910f, 15be9742, 7ae0bb85, bde3555e, eadd4f20.

## Headline

- **No hallucinated bugs.** Every one of the 54 fixes addresses a code path that really
  misbehaved before it.
- **One landed fix is defective:** eadd4f20 relocks the DSP mutex and hangs on macOS,
  Linux and Android. See below.
- **Five fixes have no plausible user-visible symptom** under the session-2 rule ("only
  real bugs with a plausible user-visible symptom"). They are harmless hardening; their
  commit messages oversell them.
- **Five commit messages misstate the mechanism** (the fix itself is right in each case).
- A handful of small simplifications; one worthwhile cleanup (the import save).
- **The hunt left `node tools/check-cpp-file-sizes.js` failing on main:**
  core/tests/MultiPresetMixerTests.cpp (844 lines) and
  core/tests/PresetEditCommandsTests.cpp (823 lines; bde3555e and 7b73b87e added tests)
  are over the 800-line budget with no pin, so the cpp-structure CI job fails. Split them
  or `--repin`.

Status: the eadd4f20 relock was fixed on 2026-10-18 (inner lock dropped;
`TestPrepareUnderTheDspLockReturns` holds the mutex on another thread so MSVC's
owner-tolerant mutex cannot hide a relock; it failed against the unfixed service and the
suite passes with the fix).

## Defective fix: eadd4f20 (backing track resample on a host-rate change)

`PluginController::Prepare` takes `mDSPMutex` (core/src/PluginController.cpp:238) and
calls `mPracticeTool->Prepare` under it (:250). That calls `FollowSampleRate`, which takes
`std::lock_guard<std::mutex> lock(mDSPMutex)` on the same reference
(core/src/controller/PracticeToolService.cpp:162; bound at PluginController.cpp:76).
`FollowSampleRate` has no other caller, so the inner lock is always a same-thread relock.

- MSVC's `std::mutex` checks the owning thread id and only bumps a count on a relock
  (`mtx_do_lock`, `_Mtx_plain` branch, crt/src/stl/mutex.cpp), so Windows runs through
  it by accident. It is still undefined behaviour.
- libc++ / libstdc++ use the default pthread mutex, which deadlocks on a same-thread
  relock: on macOS, Linux and Android, a device or rate switch with a track loaded hangs
  the app.
- `TestRateChangeResamplesTheLoadedTrack` (core/tests/PracticeToolCrossfadeTests.cpp)
  calls `svc->Prepare` without holding its mutex, so it cannot see this.
- The comment "Resampled again here, off the lock" is also wrong: the resample runs
  under the controller's lock.

Smallest correct fix: delete the inner `lock_guard` (Prepare is documented as running
under the DSP lock, PracticeToolService.h:265) and fix the comment; add a test that
holds the mutex around Prepare. Alternative that avoids the copy and the position/loop
conversion entirely: have the render thread feed the stretcher
`buffer->sampleRate / hostRate` input frames per output frame instead of resampling.

## Doubtful value (keep or drop; harmless)

| Commit | Why the symptom is implausible |
| --- | --- |
| 15be9742 EQ frequencies on rate change | The clamp is `min(20000, fs*0.49)` (graphic) / `min(16000, fs*0.49)` (parametric shelf). It only bites with the host below ~32.65 kHz, then a rate change up without a preset reload. Nothing at 44.1/48/88.2/96 kHz. |
| 868eae81 WAV fmt chunk check | Needs a corrupt or crafted file (fmt size < 16, or channels=65535/blockAlign=1). No real writer produces one. Two conditions; fine to keep. |
| 92289602 Graphic EQ band key | Needs a crafted key ("band10" with no suffix). The throw was already caught: preset load and host-state restore sit in try blocks, so the pre-fix symptom was "Failed to load preset", not an escape from the graph build. The `string_view` change is cosmetic. |
| df92d6a8 UTF-8 archive title | Only matters on a non-UTF-8 ANSI code page, and the path is only used in an error string, so on CP1252 the pre-fix effect is mojibake in a message. One line; fine. |
| 51f6e87b, second half ("measure only its tone") | OnIdle runs at 60 Hz, so at most ~17 ms of guitar entered a 1 s full-scale measurement. The mono-input three-liner was the whole bug; the `toneDone`/CollectOutput handoff could be dropped. |

## Commit messages that misstate the mechanism (fix is right)

- **9fce910f** setlist cursor: describes a footswitch "next" step skipping a slot. No
  relative step exists: engine addresses are absolute `setlist.presetN` plus
  bankUp/bankDown/bankSelect (core/src/automation/AutomationSlotTable.cpp:240-320), and
  the UI has no cursor stepping. The real symptoms are the panel/pad highlight landing on
  the wrong slot, the pad clear target, and the persisted cursor / DAW program index.
- **41c695ae** resource delete: the "double click" story is wrong (the UI confirms via a
  modal first). The real path is a stale list across instances (shared sync) or the
  pack-uninstall cleanup naming an id already gone, which then resolved to a `__suffix`
  sibling and deleted its file.
- **94198f13** input mode: says the globalFx blob carries no input mode. It does
  (PresetTypesJson.h:311-327); it is a stale snapshot from the last global-FX save. The
  fix (carry the live values) is still right. Swapping the two Apply calls would have
  been even smaller.
- **92289602**: "throwing" was a caught, reported failure (see above).
- **51f6e87b**: the second half fixes a 17 ms inaccuracy at most (see above).

## Simplifications

1. **eadd4f20**: drop the inner lock (above).
2. **bde3555e** import save: `libraryOnly` is inferred from five terms
   (`payloadPreset && saveMode.empty() && !sourcePresetId && presetIdOverride &&
   !ResolveActivePresetSlotId(...)`, PluginControllerPresets.cpp:275). The three import
   senders (presets/archive.ts:1131 and :1315, toneBuilder/realise.ts:190) all post the
   same shape; an explicit `saveMode: "import"` would replace the inference. The branch
   also replies `presetSaved`, and `onPresetSaved` (messages/presetHandlers.ts:109) sets
   the active preset id on every reply, so a 50-preset import flips the UI's active id 50
   times and arms the echo guard just before e799731b's `loadPreset` corrects it. A
   distinct reply (or an import flag the UI honours) removes that.
3. **7007f2cd** Tone Sharing feed: the root cause, `renderFeedRows(rows, requestVersion
   = browseState.requestVersion)` (toneSharingPanel/feed.ts:43), is still there: the
   call-time default makes the check at :191 unfalsifiable for any caller that omits it.
   Remove the default so the compiler forces callers to pass it; the pre-render `isStale`
   checks in browse.ts then become redundant with feed.ts:191 (the post-render ones still
   guard `updateFeaturedMoreLink`/`setUploadStatus`). The `encodeURIComponent` change in
   the same commit is unrelated scope.
4. **b9798e28 (a)**: `IsHostPlaying` has no caller outside the adapter
   (IPluginHost.h:133 only), so `mHostPlaying` and its per-block store are dead.
5. **51f6e87b**: the `toneDone` flag and the CollectOutput handoff can go; keep the
   mono-input change and the Start() re-entry guard.
6. **1c27c31a** Channels setting: the bug exists because `PresetStorage` has its own
   `SerializeGraphNode`/`DeserializeGraphNode` duplicating `PresetTypesJson`. Delegating
   one to the other would stop the next field being missed. A refactor, not part of the
   fix.
7. **Stale-reply counters**: 66b68b90, 1c3a21ae, 57f410dc, 7007f2cd, 1946bc8e and
   3312a898 each roll their own generation counter. Every module already did before the
   hunt, so this is a codebase pattern rather than a hunt defect; a shared helper is
   optional.
8. Not worth changing: 7ae0bb85's slew loop (it is a rate limiter, not a fixed-duration
   glide, so `LinearRamp.h` is not a drop-in); its 2 s drain is a heuristic (a long
   high-feedback delay can still leave a quiet stale tail). 93b692d8's extra string
   coercions and 6ed22daa's two redundant lines cost nothing.

## Fixes that landed without a regression test

1c3a21ae, e78a2685, 41a9b30d, 7cd16ae5, 082aefa2, 868eae81, 80a90825, df92d6a8,
0fe2696c, 1676910c, and every sub-item of b9798e28 (the JUCE adapter has no ctest
coverage; it was compiled as a Debug VST3 but not run in a DAW). 98b01b3e's test runs the
queued task after `controller.reset()`, which only fails deterministically under ASan.
3e2d784f's test fails pre-fix only in Debug (the Release overrun lands in the adjacent
scratch array).

## Per-commit ledger

Verdicts: REAL = real bug, fix at or near minimum; REAL* = real, with a note above;
DOUBTFUL = no plausible user-visible symptom; DEFECTIVE = fix itself is broken.

| Commit | Title (short) | Verdict | Note |
| --- | --- | --- | --- |
| 77a301ab | NEON benchmark CI | n/a | CI/benchmark change, not a fix |
| 3e2d784f | Gate Learn slice overrun | REAL | one early return; Debug-only assert |
| 0f3800ac | Reverb tails chopped/replayed | REAL | Reset on same-character SetParam; Mix-0 hold |
| 0fe2696c | Reverb drive curve continuous | REAL | +2.2 dB step at the bottom of the knob; no test |
| 720256e4 | NAM re-prepare on rate change | REAL | ResetModelsIfNeeded skipped at equal model rate |
| 1676910c | IR Cab convolver reset once | REAL | 4 Resets per sample past the fade end; no test |
| 533ab930 | DAW params vs automation slots | REAL | defaulted moves deleted by atomics, so vector copied |
| 1c27c31a | Node Channels saved | REAL* | duplicate serialiser is the root cause |
| e8c9169e | Archive session folders | REAL | Store() never moves while the settings dir does |
| a3b7d6d3 | Dropped file overwrite | REAL | no-hash node drop skipped the exists guard |
| 41c695ae | Fuzzy-match delete | REAL* | message wrong; stale list / cleanup path real |
| df4fd02b | AIFC IRs, odd WAV chunks | REAL | AIFC reachable via folder scan (.aif) |
| 1f4dc098 | Float AIFF big-endian | REAL | memcpy of BE floats; NaN guard incidental |
| 868eae81 | WAV fmt check | DOUBTFUL | crafted file only; keep |
| 92289602 | Graphic EQ band key | DOUBTFUL | crafted key; throw was already caught |
| 15be9742 | EQ frequency clamp on rate change | DOUBTFUL | host must be below ~32.65 kHz |
| 51f6e87b | Signal test on mono input | REAL* | mono half real; "tone only" half negligible |
| 40074608 | Backing track end / mono out | REAL | ring held 2.7 s when Stopped was set |
| 8b4c57a3 | Save blocked in composite edit | REAL | Save wrote the composite under the preset id |
| fb84d8ee | Tone Builder Stop | REAL | abort controller created after the first await |
| 93b692d8 | Layout import type-check | REAL | number-typed fields bypassed the attribute lint |
| 66b68b90 | Tone3000 model list stale | REAL | two identity checks |
| 1c3a21ae | Tone3000 stale replies | REAL | also stops caching `[]` on error; no test |
| 57f410dc | Tone3000 download stale | REAL | options null after close |
| 7007f2cd | Tone Sharing stale tab | REAL* | call-time default still present |
| e78a2685 | Escape names/messages | REAL | text positions were outside the lint; no test |
| 1244923d | Riff take still armed | REAL | engine never sends `armed` |
| efb882ee | Bar-align offset once | REAL | re-applied on every trim-sourced stop |
| 7cd16ae5 | Riff preview on close | REAL | six lines; no test |
| 082aefa2 | Multi-Rig Mix/Pan store | REAL | Mute/Solo wrote the store, Mix/Pan did not |
| 41a9b30d | File drops to importer | REAL | `Number("")` is 0: a file drop moved slot 0; no test |
| 98b01b3e | Hosted plugin capture UAF | REAL | existing alive-token pattern |
| 39bd42e8 | Preview id | REAL | slot keyed by preset.id, params by request id |
| 94198f13 | Input mode on reload | REAL* | premise wrong (blob is stale, not absent) |
| 2ebeb4d2 | Loop preview seam | REAL | mCarryPos = 0 played the seam first |
| 3c7a7ccc | Global EQ shelf Q | REAL | SetEQBandQ rejected bands 0 and 3 |
| f62634a2 | Composite latency | REAL | no GetLatencySamples override |
| 80a90825 | Long-path delete/read | REAL | three call-site swaps; no test |
| df92d6a8 | UTF-8 archive title | DOUBTFUL | non-UTF-8 ACP only; one line |
| bde3555e | Import stays library-only | REAL* | five-term inference; see simplifications |
| 7b73b87e | Previews out of recents | REAL | mirrors RememberActivePresetAsLast's guard |
| 4ec9c011 | Blend Test knobs on draft | REAL | draft is a clonePreset of the cache |
| 9fce910f | Setlist cursor | REAL* | message describes a step that does not exist |
| 6ed22daa | Escape / chords in key learn | REAL | capture-phase listener stored Escape |
| 2cb424a0 | Edit after undo | REAL | 320 ms debounce inside the 1.5 s amend window |
| 74403991 | Blend knob drag | REAL | panel rebuild ended the pointer capture |
| a9409590 | EQ curve limits | REAL | UI +/-18 dB and wrong ranges vs engine +/-12 |
| 3312a898 | Jam cache key | REAL | keyed by the box's text at reply time |
| 1946bc8e | Preview Clear race | REAL | no cancellation across four awaits |
| 0327c0d3 | Layout designer undo | REAL | pushUndoState on every mousedown cleared redo |
| 47b3cdfb | Tuner average reset | REAL | averaged across a note change |
| 7ae0bb85 | Multi-Rig ramps | REAL | mute step, flat gain, trim snap all reachable |
| 22b6d851 | IR slot pan through change | REAL | prev blended after mic/pan |
| b668b1e0 | Amps recover from NaN | REAL | NAM full Reset prewarms on the audio thread; rare |
| e799731b | Load last imported | REAL | pair with bde3555e |
| b9798e28 | JUCE adapter batch | REAL* | all six sub-items real; `mHostPlaying` dead; 1 s bypass line generous |
| eadd4f20 | Backing track resample | DEFECTIVE | relocks mDSPMutex under Prepare |
| cf28843a | Gapless test by mix | n/a | test follow-up to 7ae0bb85 |
