# Architecture and Code Health Review

**Date:** 26 September 2026
**Baseline:** `96d3b5f9` on `main`, one week and 74 commits after the [previous review](code-review-2026-09-19.md).
**Scope:** Application architecture, realtime safety, security, data integrity, the host layer, build and CI, test health, and documentation accuracy.

## Executive Summary

The static gates added last week are working. The protocol manifest, the alias parity checks, the cycle check and the ratcheting size budgets all pass, and they are shrinking files. The engine's main audio path is disciplined: builds happen off the lock, retirement goes through a reaper, and automation allocates nothing.

The serious problems sit where those gates cannot see: content imported from other people, rarely taken threading paths, and the Multi-Rig mixer. Four findings can cause harm in normal use:

- Imported preset data reaches `innerHTML` unescaped, in a WebView whose bridge can load native code.
- The Tone3000 bearer token is sent to any host an archive names.
- Uninstalling a tone pack deletes files but leaves their library entries in the store.
- In Multi-Rig, editing one rig's chain drops every other rig from the mix.

Two process gaps let these ship. No native test runs in CI, and none of the native build workflows has succeeded recently.

The controller refactor plan from 20 September has gone backwards. New work, most of it Nano, added `Handle*` methods instead of registry handlers, and it widened the unencapsulated edit session.

## Severity

- **P0:** exploitable, a crash, or data loss in normal use.
- **P1:** a real bug or risk likely to bite, including an audible dropout.
- **P2:** design or process debt with a concrete cost.
- **P3:** minor.

**Confidence** says how each finding was checked:
- *Confirmed*: the review lead read the code path end to end.
- *Reported*: an area reviewer read it; the lead spot-checked it but did not re-trace it.

Nothing was reproduced at run time (see Verification Performed).

## Findings

### P0 — Imported preset data reaches the DOM unescaped, with a privileged bridge behind it

The UI escapes interpolated data one call site at a time, and several call sites that render preset data do not. Importing an archive regenerates only the preset id. `sanitizePresetForArchive` strips only globals, and the engine keeps node ids verbatim.

Sinks fed by archive content:

- `preset.fxChain` strings go into `<span class="fx-node-label">` unescaped (*Confirmed*).
- Node ids go into `data-node-id="${node.id}"` raw at about 58 sites (*Confirmed* at the site below).
- Reported by the UI reviewer:
  - `attachment.type`, as the card label;
  - `current.filePath`, in both an attribute and text;
  - preset and setlist names in the setlist views;
  - layout zip `fontFamily`, `color` and `bg.value`, placed inside `style="..."` so a `"` breaks out of the attribute.

Nothing contains an injection that gets through:

- `index.template.html` declares no Content-Security-Policy.
- The WebView keeps any URL that merely *contains* "youtube.com", plus any `file://` or `data:` URL (*Confirmed*).
- The native bridge takes file paths, and a hosted-plugin config ends in a native DLL load.
- Alpine's CDN build evaluates expressions with `new Function`.

A script injected by a shared pack could read `appSettings` (API key, session id), drive the engine, and send data anywhere.

Relevant locations:

- `core/ui/ts/views.ts:611` (render at :622) and `:602`
- `core/ui/ts/signalPath.ts:966`
- `core/ui/ts/presets/archive.ts:1266` (`importPresetArchive`)
- `core/ui/ts/presets/sanitize.ts`
- `core/ui/ts/signalPath/paramsPanel/resourceSelector.ts:219`, `:409`
- `core/ui/ts/layoutRenderer.ts:297`, `:626`
- `core/ui/ts/updateCheck.ts:176` (release notes and `download_url` from the update API)
- `core/ui/index.template.html`
- `juce/source/PluginEditor.cpp:20` (`isYouTubeUrl`), `:197` (`pageAboutToLoad`)

**Recommendation:**

- Escape every sink above.
- Validate or regenerate node ids on import, for example `^[\w-]{1,64}$`, remapping edges, scenes and automation. Drop resource `filePath` from imported presets.
- Add a CSP (`script-src 'self'` plus hashes for the inline scripts, `object-src 'none'`), and switch to `@alpinejs/csp` or drop Alpine.
- Match allowed hosts exactly in `pageAboutToLoad`, and stop keeping `file:` and `data:` once the resource provider is running.
- Make escaping the default rather than a per-site habit: a tagged `html` template or `eslint-plugin-no-unsanitized`.

**Status:** Fixed in 86113960 (29 September). Every sink above is escaped, and an ESLint rule fails a plain string interpolated into a quoted HTML attribute. Node ids are validated and resource paths dropped on import, in the UI and in the engine. The page has a CSP with build-time hashes, and Alpine is gone. Still open: `pageAboutToLoad` still keeps any URL that merely contains a YouTube host name, and any `file:` or `data:` URL.

### P0 — The Tone3000 bearer token is sent to any host an archive names

*Confirmed.*

`tone3000AuthenticatedFetch` adds `Authorization: Bearer <token>` to whatever URL it is given. Outside proxy mode, `normalizeTone3000RequestUrl` returns its input unchanged.

On archive import, `downloadTone3000ResourceByReference` is called with `ref.modelUrl`, taken straight from the archive JSON, and this happens before the `previewOnly` check. For a user with their own API key, importing or previewing a crafted pack sends the token to a server the pack chooses.

Reported, same family: `buildApiUrl` and `resolvePackThumbnailUrl` pass absolute URLs through, carrying `x-session-id`. At the baseline, the debug snapshot's redaction list also missed `toneSharing.sessionId`, and that snapshot is the file users attach to bug reports.

Relevant locations:

- `core/ui/ts/tone3000.ts:56` (`normalizeTone3000RequestUrl`), `:269` (`tone3000AuthenticatedFetch`), `:353`
- `core/ui/ts/presets/archive.ts:1079`
- `core/ui/ts/toneSharingPanel/api.ts:43`
- `core/ui/ts/toneSharingPanel/images.ts:94`
- `core/ui/ts/messages/debugSnapshot.ts:19`

**Recommendation:** Attach credentials only when the host is the Tone3000 API host or the configured proxy, and refuse any other host. Download nothing when `previewOnly` is set.

**Status:** Fixed in 86113960 (29 September). Tone3000 requests go only to the official API's origin or the configured proxy's, and an archive's `modelUrl` is never fetched. A preview downloads nothing. The Tone Sharing session id goes only to the API's own origin, and debug snapshots redact it.

### P0 — Uninstalling a tone pack deletes files but not their library entries

*Confirmed.*

Removing an installed Tone Sharing pack sends `cleanupResourceLibrary` unconditionally; the feature flag guards only the Settings button. The handler has three faults:

- It removes each entry with `mResourceLibrary.RemoveResource`, which changes only the in-memory library. `RemoveUserLibraryResource` also calls `ResourceLibrary::RemoveFromStore`, but the cleanup never calls it.
- It then rewrites the legacy `resources-index.json`, which only the storage migration reads.
- Its in-use check walks `preset.graph.nodes` only, never `preset.scenes`. `FindFirstPresetUsingResource` in the same file does walk scenes.

The library loads from the document store at startup. After a restart, the removed entries come back pointing at files that are already deleted. A model or IR used only in a later scene of a user's preset is deleted from under it.

Relevant locations:

- `core/src/controller/PluginControllerResources.cpp:1691` (handler), `:1742` (`addUsedPreset`), `:1858` (removal), `:1864` (file delete), `:1886` (legacy index write), `:1933` (`RemoveUserLibraryResource`), `:1975` (`LoadResourceLibraries`)
- `core/ui/ts/toneSharingPanel/installedPacks.ts:390`
- `core/ui/ts/settings/libraryCleanup.ts:70`

**Recommendation:**

- Remove through `RemoveUserLibraryResource`.
- Reuse the scene-aware usage check.
- Read user keys from the store rather than the legacy file.
- Add cases to `ResourceLibraryDeleteWorkflowTests`: removal survives a restart, and a resource used only in a later scene is skipped.

**Status:** Fixed in 7d654f30 (29 September), with both test cases. The single delete (`deleteLibraryResource`) then got the same in-use walk in e79459f3 (2 October), so it also refuses a resource that an effect preset, composite, custom effect, the global chain or an unsaved rig still uses.

### P0 — In Multi-Rig, editing one rig's chain drops the others

*Confirmed by reading; not reproduced live.*

`ApplyPreset` performs a whole-mixer swap. `PresetVoicePool::CommitSwap` retires every live instance, and `mMixerPresetJsonCache` is replaced with a single entry.

Almost every structural edit of the working copy calls `ApplyPreset(*mActivePreset)` directly: adding, removing or moving a node, changing a resource, applying an effect preset, or changing hosted-plugin config. So with two or more rigs in the mixer, editing one fades out the others and discards their unsaved slot edits.

The correct path already exists. `ApplyActivePresetInItsSlot` replaces only the active rig, but only the automation and preset-edit paths use it. A comment in `PluginControllerCustomEffects.cpp` notes the hazard.

Relevant locations:

- `core/src/controller/PluginControllerPresets.cpp:639` (`ApplyPreset`)
- `core/src/dsp/PresetVoicePool.cpp:185` (`CommitSwap`)
- `core/src/controller/PluginControllerSignalPath.cpp` lines 489, 546, 794, 905, 1008, 1093, 1213, 1402, 1476, 1592 and 1650
- `core/src/controller/PluginControllerEffectPresets.cpp:441`
- `core/src/controller/PluginControllerCustomEffects.cpp:548`, `:1015`, `:1565`
- `core/src/controller/PluginControllerHostedPlugins.cpp:220`, `:763`
- `core/src/controller/PluginControllerMixer.cpp:299` (`ApplyActivePresetInItsSlot`)

**Recommendation:** Send every rebuild of the working copy through the in-slot path, and keep `ApplyPreset` for changing preset. Add a workflow test with two rigs: a structural edit on one leaves the other live and its slot JSON unchanged.

**Status:** Fixed in ce5fc501 (2 October). Every rebuild of the working copy goes through `ApplyActivePresetInItsSlot`. It readies the preset as `ApplyPreset` does, through the shared `PreparePresetForEngine`, and replaces only that rig's slot, which keeps its mix settings. `MultiRigEditWorkflowTests` is the two-rig workflow test recommended here.

### P1 — A race in the worker pools can run a task from the previous dispatch

*Confirmed by reading; not reproduced.*

The mixer's and the executor's worker loops read the task count once after waking, then claim tasks with `fetch_add` until they pass it. They never re-check the dispatch generation. The race runs like this:

1. A worker is preempted after a claim, or before its first one, for longer than the rest of that dispatch.
2. The next dispatch has fewer tasks.
3. The worker claims an index below its old count but past the new one, and runs a stale `mWorkItems` slot.
4. It also increments the done counter, so the audio thread can stop waiting while real work is still running.

What the stale slot does depends on the pool:

- **Mixer:** the stale slot can hold a rig already handed to `DspReaper`, which means use after free, or the same rig processed twice at once.
- **Executor:** a node from the previous parallel level runs again while the next level reads its buffers, which is an audible glitch.

The mixer pool, the executor pool and `DualLaneExecutor` are three separate implementations of the same fan-out. That is why the race exists twice.

Relevant locations:

- `core/src/dsp/MultiPresetMixer.cpp:865` (`WorkerLoop`), `:1317` (publish)
- `core/src/dsp/SignalGraphExecutor.cpp:1173` (`WorkerLoop`), `:1023` (publish)
- `core/src/dsp/RealtimeParallel.h`

**Recommendation:** Claim tasks only within the current generation, for example by packing generation and head into one atomic and claiming by CAS. Never count a stale claim as done. Merge the three pools into one realtime pool. Add a test that alternates shrinking dispatches while forcing worker delays.

**Status:** Fixed in 73d72332 (2 October). The three pools are now one, `RealtimeTaskPool`, which claims by compare-exchange on a word holding the generation. Its workers also set FTZ/DAZ. `RealtimeTaskPoolTests` forces the stalls and shrinking dispatches. Run against the old loop, it also found a hang when the next dispatch is larger.

### P1 — No native test runs in CI, and no native build workflow has succeeded recently

*Confirmed from the workflows and `gh run list`.*

- Only `ui-checks` and `cpp-structure` run on push. Every native build workflow is `workflow_dispatch` only, and none of them runs ctest.
- `GUITARFX_CORE_BUILD_TESTS` defaults to OFF, and `build_windows.bat` builds only the plugin targets.

The fast-math NaN suite, the thread and lock tests, and the JUCE tests therefore run only on a developer's machine.

Run history:

| Workflow | Result |
|---|---|
| `build-windows` | Last 15 runs failed. The area reviewer found the latest failing at KMS signing, after validation passed. |
| `build-macos` | Last succeeded on 4 July. |
| `build-linux` | Fails at Checkout. It uses an `ubuntu:20.04` container without git, and focal's cmake is below the required 3.25. |
| `build-linux-native` | Names the runner `ubuntu-24-04`; its run sat queued for 24 hours. |
| `publish-release` | Has never run. It takes each platform's latest successful artifact separately, so a release can mix commits. |

Relevant locations: `.github/workflows/*.yml`, `core/CMakeLists.txt:27`, `build_windows.bat`.

**Recommendation:**

- Add a push- and PR-triggered Windows job: configure `core` with `-DGUITARFX_CORE_BUILD_TESTS=ON`, build Debug and run `ctest -LE benchmark`. The suite takes about a minute.
- Add a Release clang-cl leg for `FastMathNanTests`.
- Repair the Linux workflows.
- Build a release from one commit.

**Status:** Open.

### P1 — Realtime violations on the audio thread

Found by the engine and controller reviewers. The main per-block path is clean; these are on side paths.

- **Transpose rebuilds in `Process`** (*Confirmed*). Stft Transpose and Hybrid Transpose apply a pending semitone, mode or timbre change inside `Process`. When the window profile changes, `Configure` → `RebuildPipeline` runs `std::make_unique` for the STFT and its core, plus vector `assign`s. When it doesn't, `ApplyCoreOptions` still builds a temporary vector, and `normalization(true)` calls `make_shared`. Automating either effect allocates on the audio thread.
  - `core/src/dsp/effects/StftTransposeEffect.h:84`, `:199`, `:346`
  - `core/src/dsp/effects/HybridTransposeEffect.h:439`
- **Riff capture posts UI messages from the audio thread** (*Confirmed*). `ProcessRiffCaptureBlock` builds `nlohmann::json` and calls `SendMessageToUI`. That reaches `juce::MessageManager::callAsync`, which allocates and locks. A progress message with a 256-float peaks array goes every 250 ms (*Reported*).
  - `core/src/controller/PluginControllerRiffs.cpp:1531`, `:1596`, `:1617`
- **Riff guidance loads files while holding `mDSPMutex`** (lock scope *Confirmed*, load *Reported*). Arming, starting or previewing a riff calls `ActivateRiffGuidance` under the lock. That reaches `MetronomeClickLibrary::Load`, which reads, decodes and resamples WAV files. `ProcessAudio`'s `try_lock` fails meanwhile, and the block is output as silence.
  - `core/src/controller/PluginControllerRiffs.cpp:147`, `:254`, `:1061`, `:1124`
  - `core/src/controller/MetronomeService.cpp:744`
- **Worker threads have no denormal protection or priority** (*Reported* by two sub-audits). Only `PluginProcessorAdapter.cpp:324` sets `ScopedNoDenormals`. The three worker loops set neither FTZ/DAZ nor a raised or MMCSS priority, yet the audio thread waits on them.
  - `DualLaneExecutor::Run` takes a blocking lock, notifies, and spins on `CpuRelax()` with no yield fallback. The mixer already learned that lesson at `MultiPresetMixer.cpp:1348`.
  - `DualLaneExecutor::Instance()` creates its thread lazily, from the first qualifying `Process` call.
  - Location: `core/src/dsp/RealtimeParallel.h:48`, `:87`, `:95`
- **WASM effects have no execution bound** (*Confirmed*). The engine uses `wasm_engine_new()` with the default config and no fuel or epoch interruption. The guest is called once per sample with `mDSPMutex` held. A module that loops forever hangs the audio thread, then every message-thread handler, then the host.
  - `core/src/dsp/effects/WasmEffect.cpp:619`, `:1120`
- **`setNonRealtime` takes a blocking lock** (*Confirmed*). JUCE's LV2 wrapper calls `setNonRealtime` from `run()`. When the mode changes, `SetOfflineRendering` calls `PushNamQualityToDsp`, which takes a blocking `lock_guard` on `mDSPMutex`, builds strings, and re-prepares NAM. `mOfflineRendering` is a plain `bool` shared across threads. This happens once per bounce start and end. A comment says it runs off the audio thread, which is wrong for this wrapper.
  - `core/src/controller/PluginControllerSettings.cpp:294`, `:309`
  - `juce/source/PluginProcessorAdapter.cpp:637`
- **Atomic `shared_ptr` loads on the audio thread** (*Confirmed*). The metronome does these every block while it is on, and demo preview while it plays. MSVC and libc++ implement them with a lock pool. `StopPreview` and `DeactivateGuidance` can leave the audio thread holding the last reference and freeing the buffer.
  - `core/src/controller/MetronomeService.cpp:217`, `:260`
  - `core/src/controller/DemoPreviewService.cpp:120`, `:132`

**Recommendation:**

- Stage STFT reconfiguration through `DeferredRebuild`.
- Publish riff progress through atomics and send the messages from `OnIdle`.
- Load click samples off the lock and swap a pointer in under it.
- Set FTZ/DAZ and priority in every worker loop, with spin-then-yield waits and threads created at Prepare.
- Enable wasmtime epoch interruption, with a watchdog and a per-block deadline.
- Defer the offline switch to the message thread.
- Replace the atomic `shared_ptr` loads with a lock-free pointer handoff.

**Status:** Open.

### P1 — The host bypass path is not latency-compensated, and release builds ship JUCE debug

*Confirmed.*

- `processBlockBypassed` clears the MIDI and calls JUCE's default. That default passes the dry signal through with no delay and asserts `getLatencySamples() == 0`. With an IR cab, NAM oversampling or a transpose in the chain, a host bypass shifts the track earlier by the reported latency.
- `GUITARFX_JUCE_FORCE_DEBUG` is 1 for every desktop build. It is deliberate, to keep WebView2 DevTools in release. But it also turns on `jassert` and the leak detectors in shipped VST3, CLAP and Standalone builds, including on the audio thread, and the assertion above then fires on every bypassed block.
- `JUCE_WEBVIEW2_DEVTOOLS_ENABLED=1` is not referenced anywhere in JUCE (*Reported*). *Fixed 2026-10-04 with the JUCE 9.0.3 bump: `JUCE_FORCE_DEBUG` and this define are gone, so release builds are release builds on every platform; DevTools in the WebView now follow the build configuration.*

Relevant locations:

- `juce/source/PluginProcessorAdapter.cpp:387`
- `juce/CMakeLists.txt:55`, `:435`

**Recommendation:** Delay the bypass path by the reported latency. Find another way to keep DevTools, such as a runtime flag or environment switch, so release builds can be real release builds. Android already does this.

**Status:** Open.

### P1 — Windows path handling in the host layer

*Confirmed.*

- Files picked through the JUCE chooser become `std::filesystem::path(juce::String::toStdString())`. On MSVC that decodes UTF-8 bytes through the ANSI code page, so a path such as `C:\Users\José\amp.nam` becomes `JosÃ©` and fails to load. The same pattern appears elsewhere in the adapter, at `:929`, `:962` and `:1164` (*Reported*). Nano already uses `util::PathFromUtf8`.
- The Windows `SaveFileAsync` runs an `IFileSaveDialog` on a detached thread and calls the core's callback on that thread. Six controller callbacks capture raw `this`, so closing the plugin with the dialog open leaves a dangling controller.
- Those callbacks call `generic_string()`, which throws for characters outside the code page, and an exception on a detached thread terminates the process.
- The dialog's title and default name are widened byte by byte.

Relevant locations:

- `juce/source/PluginProcessorAdapter.cpp:764`, `:778`, `:843`
- `core/src/controller/PluginControllerPresetArchive.cpp:426`
- `JuceHostedPluginEffect.cpp:114`, `:289` (*Reported*: `path.string()` throws, the bug fixed in the core by f89ee24d)

**Recommendation:** Use `util::PathFromUtf8` and `PathToUtf8` throughout `juce/`. Use `juce::FileChooser` for saving, as the non-Windows branch does, or marshal the result back to the message thread with a liveness token.

**Status:** Open.

### P1 — Background housekeeping stops while the editor is closed

*Confirmed.*

`PluginController::OnIdle` is called only from the editor's timer (`SoundshedEditorBase.cpp:130`). It is the only regular caller of `ApplyDeferredNodeRebuilds` and `CollectRetiredMainThread`.

The plugin's always-on 30 Hz timer runs only `DrainControlSurfaceRequests`, which applies queued host state and folds automation changes. While the editor is closed, two things therefore stall:

- DAW or MIDI automation of an IR cab's Normalize or Low Latency is recorded but never built, including during offline bounces.
- Retired chains holding hosted plugins pile up in the main-thread queue.

Relevant locations:

- `core/src/PluginController.cpp:328`, `:580`
- `juce/source/PluginProcessorAdapter.h:184`

**Recommendation:** Move the editor-independent parts of `OnIdle` into the always-on timer.

**Status:** Open.

### P1 — The working copy and the mixer's saved copy of each rig drift apart

*Reported by the controller reviewer.*

Knob and bypass edits, the NAM level reset and automation folds update `mActivePreset` but never refresh that rig's entry in `mMixerPresetJsonCache`. Two consequences:

- Switching rig tabs (`FocusMixerPreset`) reloads the stale cache entry, so the editor reverts edits the audio still has.
- `BuildHostState` saves unfocused rigs from that cache, so a DAW project loses them.

`UpdateResourceForNodeType` edits the top-level graph, which the scene switch inside `ApplyPreset` then overwrites. So `loadModel` and `loadIR` do nothing. They have no UI sender, so this is latent.

Relevant locations:

- `core/src/controller/PluginControllerSignalPath.cpp:92`, `:140`, `:1555`
- `core/src/controller/PluginControllerNodeControl.cpp:161`
- `core/src/PluginController.cpp:516`
- `core/src/controller/PluginControllerMixer.cpp:233`
- `core/src/controller/PluginControllerHostState.cpp:239`

**Recommendation:** This is the bug class that step 2 of the controller plan exists to remove. Build an `ActiveEditSession` value type with one mutation entry point that re-syncs the scene and marks the rig's cache dirty. Serialise it lazily, and drop `mActivePresetJson`, which is only ever copied into the cache.

**Status:** Mostly fixed in ce5fc501 (2 October). Every in-place edit of the working copy now re-serialises it into that rig's cache entry through `MirrorActivePresetJson`, and `FocusMixerPreset` mirrors the rig it leaves, which catches automation folds. Still open: the `UpdateResourceForNodeType` scene overwrite, which is still latent, and `ActiveEditSession`, which does not exist yet, so the mirroring is still a call each edit path has to remember.

### P2 — Controller robustness

*Reported by the controller reviewer.*

- **Queued callbacks outlive the controller.** `RunOnMainThread` and `SendMessageToUI` post raw `this` through `callAsync`. `~PluginController` cannot cancel what folder-scan workers or hosted-plugin callbacks have already queued. The tuner callback, set in `ApplyPreset`, holds `this`, but `mTuner` is destroyed before `mPresetMixer`, whose destructor joins the worker that calls it.
  - `juce/source/PluginProcessorAdapter.cpp:669`, `:937`
  - `core/src/controller/PluginControllerHostedPlugins.cpp:36`
- **Sync between plugin instances misses changes.**
  - `PollSharedSyncState` marks a version as seen before checking that the UI is ready, so changes made while the editor is closed are never applied.
  - Nano never asks for `getSharedSyncState`.
  - `TouchSharedSyncState` marks its own version as seen, skipping any other instance's change in the same poll window.
  - Location: `core/src/controller/PluginControllerSharedSync.cpp:118`, `:177`, `:312`
- **Failures are swallowed.**
  - `RestoreHostState` clears every rig, then a single trailing `catch` hides any error after that.
  - The `addActivePreset` route has an empty `catch (...)`.
  - A preset payload that fails to parse makes `loadPreset` return silently.
  - Locations: `core/src/controller/PluginControllerHostState.cpp:491`, `:610`; `core/src/dispatcher/MessageDispatchMixer.cpp:38`
- **The broadcast coalescer is bypassed.** 28 synchronous `BroadcastState()` calls skip it, and each one checks every resource file on disk.

### P2 — UI robustness

- **Preset load race** (*Confirmed*). `loadStoredPreset` activates preset B optimistically. A late `presetLoaded` for A then clears B's loading state and makes A active.
  - `core/ui/ts/presets/load.ts:141`
  - `core/ui/ts/messages/presetHandlers.ts:29`
  - Fix: match the reply to the loading id.
- **Inbound dispatch is unguarded** (*Confirmed*). `handleIncomingMessage` wraps neither `JSON.parse` nor the handler call, so a handler that throws aborts silently and can leave state half applied.
  - `core/ui/ts/messages.ts:147`
- **The write checker misses some `uiState` writes** (*Reported*):
  - `layoutManager.ts` writes `layoutLibrary` through an alias.
  - `gateSettings.ts` mutates `globalSignalChain`, and through a shallow copy, the shared default graphs.
  - The active preset draft is mutated in place from about 10 modules via `getActivePresetForRender()`.
- **Async results can land on the wrong item** (*Reported*):
  - A late Tone3000 details response imports tone A's models under tone B (`tone3000DetailsView.ts:88`).
  - `blendEditor.ts:939` rebinds every row on each Add, so handlers fire repeatedly.

### P2 — Host and engine correctness

- `getTailLengthSeconds()` returns 0 despite reverbs and delays. *Confirmed* in code; the effect on host tail handling is unverified.
  - `juce/source/PluginProcessorAdapter.cpp:554`
- *Reported:* a hosted plugin's dry/wet mix ignores the plugin's latency and comb-filters below 100% wet. `latencyChanged` is ignored too.
  - `juce/source/JuceHostedPluginEffect.cpp:1326`, `:1780`
- *Reported:* the IR cab has no mono path. A mono IR on the default mono chain runs two identical convolutions, and more with slot B or during a crossfade.
  - `core/src/dsp/effects/IRCabEffect.h:240`, `:1403`
- *Reported:* a NAM quality change holds `mDSPMutex` through every NAM node's re-prepare and prewarm, across all rigs and global chains.
  - `core/src/controller/PluginControllerSettings.cpp:309`
- *Reported:* ConvNet `.nam` models allocate every block inside the pinned NeuralAmpModelerCore fork. WaveNet, the common architecture, does not.
  - `convnet.cpp:224` in the fetched source
- *Reported:* SetParam hazards on the audio thread. Each is on first use of a key or on malformed input.
  - AutoArp rebuilds its step vector on every call.
  - `MixerEffect` and `AutoArp` call `std::stoi` on key suffixes and throw uncaught on a bad key.
  - `MultiModelNAM`, `Wasm` and composite `SetNodeParam` insert into maps on a new key.
  - Advanced Reverb's `character` resets the whole tail even when unchanged.

### P2 — Build hygiene

*Reported by the host reviewer; the warning level was spot-checked.*

- The core, about 95k lines, sets no warning level, so MSVC uses its default. Only `juce/source` gets JUCE's recommended warning flags, and nothing uses `/WX` or clang-tidy.
- `Sanitizers.cmake`, `Tests.cmake` (which references missing files), `Assets.cmake` and `Benchmarks.cmake` are never included.
- The post-build step copies all of `core/ui`, including about 108 MB of `node_modules`, into each artefact. The installer and the macOS workflow then keep separate exclusion lists for it.
- JUCE modules and `JuceHostedPluginEffect` are recompiled for each of the Guitar, Nano and several test and tool targets.
- The WASM runtime is downloaded without a `URL_HASH` (`check-dependency-pins` notes it).
- CI supply chain: build and signing share one job with `id-token: write`, after `npm ci` and unpinned global npm installs. The signing tool MSI is installed without a checksum.

### P2 — Nano duplicates presentation logic, and nothing builds it automatically

*Reported.*

Nano's layering is clean. It talks to the engine only through the JSON protocol, and `check-protocol` covers it. But:

- About 2.5k lines re-implement TypeScript presentation logic (`ParamFormat`, which mimics JavaScript's `toFixed`; `PresetBrowse`; `ChainLayout`; `NodeLabels`) with no shared test vectors.
- Nano has its own tone-sharing HTTP and session client, about 1.3k lines.
- No script or workflow builds, validates or packages it.

**Recommendation:** Generate shared test vectors, for example parameter formatting cases in a JSON file that both vitest and a C++ test read, and add Nano to the CI build.

### P2 — Test health

*Reported by the tests reviewer; counts spot-checked.*

- 77 of the 161 UI→engine message types are never sent by any C++ test. They include most delete and save handlers (`deleteSignalPathNode`, `deleteRiff`, `deleteLayout`, `saveRiffTake`, `savePresetArchive` and others) and `cleanupResourceLibrary`, whose bugs are above.
  - Suggested: a table-driven test that reads `core/protocol/ui-messages.json` and sends every type, both empty and with null fields, to a sandboxed controller.
- 9 effects have only the registry smoke test, or edge checks only: Chorus, Phaser, Tremolo, the VCA and Opto compressors, Auto-Wah, Ambient reverb, Chamber reverb and Doubler.
- One test cannot fail. It prints "SKIP" on failure "to keep suite green" (`core/tests/SignalGraphExecutorTests.cpp:850`).
- 25 of the 36 test files that use temp folders use fixed `%TEMP%` names, so concurrent runs clobber each other.
- 54 files define their own check macros, 24 their own test host, and about 18 copy the profile-sandbox code. ctest sees 88 executables for 527 cases and can't run a single case.
- The UI tests import 73 of 259 modules directly. Nothing tests `messages.ts` dispatch, `stateHandlers`, `importPresetArchive`, the tone-sharing panel (0 of 26 modules) or the layout designer.
- Orphaned test code:
  - `Catch2Main.cpp`, although Catch2 isn't a dependency anywhere;
  - `helpers/test_helpers.h`, which includes a header that doesn't exist;
  - five extended-DSP test files that no build tree compiles.
- No per-test ctest timeout is set except on `SteadyStateProfiler`.

### P3 — Minor

- Engine:
  - The summed-mono input branch can never run (`MultiPresetMixer.cpp:1052`). *Removed 3 October.*
  - `core/src/dsp/simd` (1,304 lines) is used only by a benchmark.
  - `MultiPresetMixer.h:12` includes `ParametricEQEffect.h` without using it, and that reaches 36 TUs through `PluginController.h`. *Removed 3 October.*
  - Executor move-assignment drops the parallel-level scores, so a global chain installed by swap never runs levels in parallel. *Fixed 3 October: `SignalGraphExecutor` is no longer movable; `GlobalChainEngine` holds its executors by pointer and `DspReaper` takes an outgoing one whole, worker threads included.*
- Controller:
  - `SelectSceneByIndex` and the setlist bank functions call `try_lock` on a mutex the calling thread may already hold, which is undefined behaviour for `std::mutex`. *Fixed 7 October: automation, which always applies under the lock, parks every scene, bank and setlist request for the message thread, and the UI's scene switch calls `SelectSceneByIndexDirect`. The wrappers are gone.*
  - The layout association index is a JSON file outside the store and outside shared sync.
- Dead code:
  - the composite-preset file API;
  - `PresetStorage::SaveAllToDirectory`;
  - `SyncPresetSceneFromGraph`;
  - 9 unused store item types;
  - the `loadModel`, `loadIR`, `browseModel` and `browseIR` UI senders;
  - 22 unused TypeScript exports.
- DSP duplication. The shared helpers in `DelayLineSupport.h` and `BiquadDesign.h` are mostly used only by the newest effects. Each of these is hand-rolled in about 5–14 files:
  - LFOs (per-sample `std::sin`);
  - delay lines;
  - envelope followers;
  - DC blockers;
  - one-pole smoothers.

  There are 16 RBJ cookbook designs across 7 files, and 3 different denormal-flush thresholds.
- Only 13 of 41 effects use `EffectParamSpec`, although the quickstart names it as the one place ranges live. Tape Delay and Analog Delay carry a duplicate spec type.
- Host:
  - Deep-link parsing is duplicated in `Main.cpp` and `PluginEditor.cpp`, and the Android `soundshed://` intent filter is dead.
  - The Nano debug server ships in release. It is environment-gated and localhost-only, but its screenshot command deletes any path it is given.
  - The session logs grow without bound.
- UI:
  - `views.ts` mixes five unrelated concerns.
  - There are two Tone3000 browsers and two library list renderers, and `sanitizeFilename` is defined three times.
  - 978 literal colours sit outside the theme files, and `themes/light.css` carries about 140 component overrides as a result.

## Follow-up on the 19 September Review

| Item | State at this baseline |
|---|---|
| Riff buffer work off `mDSPMutex` | Done, and still holding. New riff issues on the audio thread are reported above. |
| UI import cycles | Holding: 0 cycles across 259 modules. |
| `uiState` write ownership | Slow. 84 → 82 pinned write pairs, 17 fields still with more than one writer, and some writes the checker cannot see. |
| `MultiPresetMixer` god object | Largely done: 1,740 lines, split into voice pool, global chain engine, reaper, tuner and telemetry. `Process` is still 532 lines, and the swap rules are split between the mixer and the pool. |
| `PluginController` god object | **Regressed** (see Architecture Trajectory). |
| Protocol manifest | Holding: 161 UI→engine and 111 engine→UI types agree. Generated unions and payload schemas are still open, and 108 inbound UI handlers take `Record<string, unknown>`. |
| Retired auto-level | Done. |
| Dependency pins | Holding: 13 pinned. The WASM runtime download has no hash. |
| Size ratchet | Working: 41 C++ and 16 UI files are over budget, all pinned and shrinking. One pin can be tightened now (`OptimizedNAMAmpEffect.h`). |
| Header-only effects | Still open: 41 effect headers, about 24k lines, pulled into every test TU that includes `BuiltinEffects.h`. |

## Architecture Trajectory

The plan of 20 September for `PluginController` had four steps:

1. Extract the area-local member clusters.
2. Introduce an `ActiveEditSession` value type.
3. Introduce a `BroadcastCoalescer`.
4. Retire the `Handle*` methods into registry registrations.

A week later:

| Measure | 20 Sep | 26 Sep |
|---|---|---|
| `Handle*` methods | 115 | 130 |
| Message types served by the registry | — | 14 (of about 163 routed) |
| Edit-session members | 5 | 10 |
| TUs including `PluginController.h` | 55 | 61 |
| Data members | 77 | 78 |
| Area clusters extracted | 1 of 7 | 1 of 7 |

The edit session grew because Nano added a baseline, a dirty flag and a mute. Most of the new `Handle*` methods came from Nano's preset-edit and effect-analysis messages. Neither `ActiveEditSession` nor `BroadcastCoalescer` exists yet.

The drift in the P1 finding on each rig's working copy and in the Multi-Rig P0 is the concrete cost.

Suggested gates so the plan cannot slide again:

- Fail `check-protocol` when a new message type is routed through a dispatcher TU instead of a registry.
- Fail a check on any new direct `BroadcastState()` call.

## Documentation Drift

Claims at the baseline that do not match the code:

- `docs/architecture-overview.md`:
  - :75 "Audio ↔ UI: Lock-free queues" — false. UI edits reach the DSP under `mDSPMutex`, and only telemetry and `NodeChangeQueue` are lock-free.
  - :76 "UI → Background: Task queue" and :72 "Below Normal" priority — false. There is no such queue, and nothing sets a thread priority.
  - :109 certificate pinning — false.
  - :112 file-size validation for NAM and IR files — false.
  - :113 JSON schema validation with depth limits — false.
  - :119 Content Security Policy — false.
  - The performance targets table is not measured or asserted anywhere.
- `docs/TODO.md`:
  - :8 the noise gate hold is done (reworked in d41ad1ed).
  - :28 IR resampling is windowed sinc now (`ImpulseResampler.h`).
  - :25 profiling infrastructure exists (`SteadyStateProfiler`).
- `docs/agent-quickstart.md`:
  - :192's configure line omits `-DGUITARFX_CORE_BUILD_TESTS=ON`, so a fresh tree builds no tests. `.github/copilot-instructions.md` repeats it.
  - The controller file table omits `PluginControllerEffectAnalysis.cpp` and `PluginControllerPresetEdits.cpp`.
  - The services table omits `ControllerDisplayFeed`.
- `.github/copilot-instructions.md` names `removeSignalPathNode`, which never existed (the message is `deleteSignalPathNode`), and `docs/prd/PRD.md`, which was deleted.
- `README.md:135`, `:141` list the targets `GuitarFX_OfflineProcessingTest` and `VST3DebugHost`, which do not exist.
- *Reported:*
  - `features.md` still says mixer auto-level exists, and cites the deleted `NAMAmpEffect.h`.
  - `neural-amp-modeler-core-integration.md` names upstream v0.4.0.rc3 rather than the pinned DLC86 fork.
  - `automation-and-midi-mapping.md` says `acceptsMidi` is false and there are no programs; both exist now.

## What Is Healthy

- **The structural gates hold and drive improvement.** The protocol manifest, the effect alias parity on both sides, the presentation check, the ratcheting size budgets and the zero-cycle import check all run in CI on every push. `signalPath.ts`'s pin fell from 1,607 to 1,476 in a week.
- **The engine's main audio path is disciplined.**
  - Preset and global-chain swaps are built off the lock and installed under it, and what they replace is freed after it is released.
  - `DspReaper`'s retire from the audio thread is a try-lock into reserved capacity that never frees.
  - The execution plan is precomputed, and `node.*` automation is allocation-free.
  - `DeferredRebuild` moves the IR cab's and IR reverb's rebuilds off the audio thread.
  - No DSP code uses raw `isnan`/`isfinite` or a NaN sentinel.
- **The host state design is careful.** `HostStateRelay` covers saves, restores and program changes from any thread. The parameter layout is append-only with reserved slots, `getValue` reads are lock-free, and restores avoid creating VST3 undo steps.
- **Hosted plugins have clear lifetimes.** They are destroyed on the message thread via the reaper, and their processing try-locks and falls back to passthrough. The resource provider sanitises paths.
- **Tests are written against real bugs and keep pace.** File headers name the bug each test pins, and tests changed in 90 commits last month against 95 for source. No test touches the real user profile.

## Recommended Order of Work

1. The two security P0s together: escape the sinks, validate node ids on import, add a CSP, tighten navigation, and restrict where credentials are sent.
2. The two data-loss P0s: the library cleanup, and Multi-Rig structural edits. Then fix the working-copy drift, ideally by starting `ActiveEditSession`.
3. The worker-pool race, plus FTZ/DAZ and priority for the worker threads.
4. A push-triggered Windows ctest job. Then repair the release workflows so a release comes from one commit.
5. The realtime P1s (transpose, riff capture and guidance, WASM bound), then the host P1s (bypass latency, forced JUCE debug, UTF-8 paths, idle work with the editor closed).
6. Gates that stop the controller plan sliding, then its steps 2–4.
7. The test and doc debt: a message sweep test, a unique temp-dir helper and a shared test support header; then correct the doc claims listed above.

## Verification Performed

- `npm run verify` in `core/ui` passed:
  - typecheck and lint;
  - 52 test files, 497 tests;
  - 0 import cycles in 259 modules;
  - 82 pinned state-write pairs;
  - the protocol check (161/111 types, GUIDs match);
  - 42 effects in the presentation check;
  - 16 known oversized files;
  - all 60 stylesheets reachable.
- `node tools/check-cpp-file-sizes.js`: 475 files, 41 over budget, all known.
- `node tools/check-dependency-pins.mjs`: 13 dependencies, all git pins fixed. The WASM runtime URL has no hash.
- `gh run list`: the run history above.
- Five area reviewers (controller, engine, UI, host and CI, tests and docs) and three engine sub-audits (SetParam paths, Process paths and duplication, model and convolver lifetimes) read the code. The review lead re-read the code for each finding marked *Confirmed*.
- No native build, ctest run or live app session was performed. Another session was building in the same tree throughout. None of the findings has been reproduced at run time.

## Review Caveat

While this document was being written, four fix sessions started in the same working tree, one for each of the first five findings (the two security findings share one). About 100 paths were modified and uncommitted. Everything above describes the baseline `96d3b5f9`, not the working tree. This review added only this file. The exception is the **Status** lines of the findings fixed since, which were updated on 2 October to name the commits that fixed them.
