# Backend performance review, 2026-11-17

A read-only review of the C++ engine (`core/src`) and the JUCE adapter (`juce/source`) at
26baa3c9, done by five parallel reviewers: the realtime plumbing, the effects, the message
thread and UI bridge, memory and load time, and the adapter. Nothing was built or measured
unless it says so. Figures are estimates from reading the code, set against the recorded
costs (48 kHz/64 on x64: NAM standard 20.9 µs a block, IR Cab 4.9 µs, a 1333 µs deadline).
The items most likely to matter were checked against the code a second time. They are
marked **verified**.

It also covers a reported REAPER problem: plugin controls can't be used while the
transport runs.

## REAPER: controls unusable during playback

**It is not a flood of DAW parameter events.** Only 34 host parameters exist (setlist,
bank, trims, scene, 16 custom slots); ordinary knobs are not host parameters. JUCE drops an
unchanged value before it reaches us (`setValueAndNotifyIfChanged`, VST3 wrapper :827, and
the CLAP equivalent), REAPER sends nothing on a flat lane, and the drain applies only what
was queued, never every slot every block.

What makes playback different is that audio flows, so everything that reacts to audio runs.
Four candidate causes, most likely first.

1. **A Plugin Host node in the chain floods the message thread (verified).**
   `JuceHostedPluginEffect::AttachHostedPluginParameterListeners` (:2062) listens to every
   hosted parameter, read-only meters and gain-reduction outputs included. JUCE's VST3 host
   fires those listeners from the audio thread each block the value moves. Every change
   outside a gesture calls `ScheduleAutoCapture`, which runs the hosted plugin's
   `getStateInformation` and a base64 encode on the message thread, with no rate limit
   (:2362, :2436). When the state differs,
   `PluginControllerHostedPlugins.cpp:290-309` re-serialises the whole preset, queues a
   **Full** `BroadcastState`, and calls `NotifyHostStateChanged`, which marks the host
   project dirty. Presets with hosted state run from 150 KB to 1.1 MB, so each event costs
   milliseconds, plus a full UI re-render. None of this happens while stopped.
2. **UI edits wait on the DSP lock (verified).** `HandleUpdateSignalPathNodeParamRequest`
   (`PluginControllerSignalPath.cpp:96`) and the bypass handler (:137) take `mDSPMutex`
   with a blocking `lock_guard`. `ProcessAudio` (`PluginController.cpp:340`) holds it for
   the whole block. MSVC's `std::mutex` is an SRW lock, which is not fair, and REAPER's
   anticipative FX renders blocks back to back, so the message thread can wait out a burst.
   When it does win, that block is silent, because the audio thread's try-lock fails. Each
   event also re-serialises the whole preset (`MirrorActivePresetJson`, :104).
3. **Automated parameters re-render controls under the pointer.**
   - `FoldAutomationNodeChanges` sends `signalPathNodeParamUpdated` at up to 30-60 Hz.
     `showEngineParamChange` (`ui/ts/signalPath/paramsPanel/paramControls.ts:349`) draws
     only a plain knob in place. Enum pickers, EQ, blend, pitch-range controls and custom
     layouts instead rebuild the whole params panel every 100 ms (`engineParamChange.ts`),
     which recreates the control being dragged.
   - A bypass lane sets `bypassChanged = true` whether or not the state flipped
     (`PluginControllerPresetEdits.cpp:566`, `:609`, **verified**), so every value causes a
     mirror and a preset broadcast.
4. **The plugin never tells the host about a UI edit (verified).** No
   `beginChangeGesture`, `setValueNotifyingHost` or `endChangeGesture` exists in
   `juce/source` or `core/src`, except for a hosted plugin's own restore. As a result:
   - REAPER's Touch and Latch modes can't engage, its Read lane always wins, and moves
     aren't recorded.
   - For a node param mapped to a custom slot, the UI edit doesn't update the slot's cell.
     The next envelope value that differs from the cell overwrites the knob.
   - A scene picked in the UI updates the cell, but the host's copy stays stale. The next
     block re-applies the envelope's scene (a slot rebuild plus a full `presetLoaded`),
     so the scene bounces back.
   - Trigger slots reset their cell to 0 after firing (`AutomationSlotTable.cpp:879`). A
     lane resting at 1 therefore re-fires its setlist load or scene switch on every play or
     seek.

**How to tell which.** Check whether the chain has a Plugin Host node (cause 1). Turn off
anticipative FX for the track, or record-arm and monitor it, and see whether the UI frees up
(cause 2). Take a `cdb -pv` stack dump during a stall: the main thread is either in
`AcquireSRWLockExclusive` under the param handler, or in a hosted `getStateInformation`.

**Fixes, in order.**
1. Hosted plugins:
   - Ignore parameters where `!isAutomatable()`, and output parameters.
   - Coalesce captures to at most every 500 ms, or at gesture end and on editor close.
   - Send `signalPathNodeConfigUpdated` only (no Full broadcast).
   - Debounce `NotifyHostStateChanged`.
2. Fold bypass changes only when `node->enabled != enabled`. A one-line fix.
3. UI param and bypass edits go through a lock-free latest-value mailbox keyed by (preset,
   node, param), drained at the top of `ProcessAudioLocked`. The `node.*` route already
   applies allocation-free under the lock. Mirror the preset JSON lazily (a stale flag
   flushed by `FocusMixerPreset` and the readers that walk the cache), not on every event.
4. In the UI, hold back the 100 ms panel rebuild while a pointer is down on the panel, and
   don't push an engine value into the knob being dragged.
5. Report UI edits of DAW-mapped slots to the host: `beginChangeGesture` at drag start,
   `setValueNotifyingHost` on each move, and `endChangeGesture` at drag end. Update the slot
   cell for node params mapped to a slot. Weigh this against REAPER's per-gesture state
   snapshots (it calls `getStateInformation` after each gesture for undo), so the host-state
   blob needs to be cheap first (see M7).

**What landed (uncommitted, 2026-11-17).** Fixes 1, 2, 4 and 5. Fix 3 did not.
- Hosted plugins: listeners skip non-automatable parameters. Changes outside a gesture
  capture at most every 500 ms (a timer catches the last one). Program and parameter-info
  changes still capture at once. A captured state sends `signalPathNodeConfigUpdated` only.
  `NotifyHostStateChanged` is not debounced.
- The bypass fold marks a change only when a node flips.
- The UI skips engine values for a knob being dragged, and it holds the 100 ms panel
  rebuild until the last pointer is up.
- A UI edit of a node param or bypass that a DAW slot maps now writes the slot's cell.
  Trims, scene, MIDI and UI slot edits are marked too. The adapter's 30 Hz timer reports
  each marked parameter to the host: it opens a gesture, notifies the value, and ends the
  gesture after 300 ms idle. The Standalone skips this. A trigger the host fired keeps the
  host's value, so playback start no longer fires it again. A trigger fired from the UI
  still falls back to 0. Tests: `PluginEditHostReportTests`, and
  `TestUngesturedChangesAreThrottled` in `HostedPluginStateCaptureTests`.

**Live check in REAPER 7.86 (VST3, 48 kHz / 128).** The DI clip played on a loop, with
fast 2 Hz sine lanes on Input Level, Output Level and a custom slot mapped to the amp's
treble. Each run timed 100-200 knob edits sent from the WebView, from the send to the
`getAutomation` reply. It also made real 200 px mouse drags over CDP.

| Build | Chain | Stopped p50 / p99 | Playing p50 / p99 / max | Drag the lane's knob while playing |
|---|---|---|---|---|
| Before | user preset, 33% DSP | 1.7 / 3.6 ms | 1.7 / 5.5 / 5.7 ms | the lane overwrites it (dragged to 10, reads -0.02) |
| Before | + 6 hosted plugins | 2.0 / 6.0 ms | 2.2 / 7.4 / 8.2 ms | overwritten |
| After | user preset | 1.4 / 2.5 ms | 1.5 / 5.6 / 5.8 ms | holds at 10, then back to the lane after release |
| After | + 6 hosted plugins, 50% DSP | 1.7 / 3.5 ms | 1.7 / 4.3 / 4.5 ms | holds, then back to the lane |

- The DSP lock never stalled the message thread, even at 50-62% load with six hosted
  plugins. So fix 3 is parked until a stall is caught: get a `cdb -pv` dump and the chain.
- Drags of knobs no lane drives worked on both builds. No panel was rebuilt under them,
  because these were plain knobs.
- The six hosted plugins tried (kHs Flanger, SPAN, Ozone 10 Elements, NAM On Steroids,
  Slam Amp, Gateway) don't move their own parameters, so none caused a capture storm.
  That cause is covered by the throttle test, not by the live check.
- The symptom reproduced: during playback, a lane on a mapped knob takes it back from the
  user. That was REAPER's Read lane winning because no gesture was sent. Now REAPER treats
  the move as a touch.

## Message thread and loading

| # | Finding | Where | Cost | Fix |
|---|---|---|---|---|
| M1 | The preset list fully deserialises every user preset just to send id, name, category, tags and description (**verified**) | `PluginControllerBroadcast.cpp:629` → `PresetStorage::LoadAllFromStore` (:884) | 100-250 ms per editor open, preset delete or shared-sync catch-up on a 650-preset, 8 MB library; tens of MB transient | Cache summary rows, invalidated on preset writes, or select with SQLite `json_extract` |
| M2 | Each DAW instance builds the standalone's last preset in `Initialize` (models, IRs, hosted plugins), then the project restore throws it away (**verified**) | `PluginController.cpp:227` `LoadLastSessionState`; `LoadFallbackPreset` parses every preset to find the first id | 10-60 ms per instance for NAM+IR; seconds per instance if hosted plugins | In plugin mode, build the default chain from a posted message-thread task only if no restore arrived first |
| M3 | With the editor closed, retired chains that hold hosted plugins are never freed (**verified**) | Only `OnIdle` (:466) and the destructor call `CollectRetiredMainThread`; `OnIdle` needs an open editor | The whole outgoing hosted plugin: hundreds of MB for a sampler | Also collect from `DrainControlSurfaceRequests` (30 Hz, always on), gated by an atomic "queue non-empty", and at the end of `RestoreHostState` |
| M4 | A shared-sync catch-up reloads every domain whatever changed: resources, blends, composites (bumping the registry generation), riffs, automation, and the preset list | `PluginControllerSharedSync.cpp:51-123`, :190-245 | 150-400 ms in each other open editor, even for a setlist step | Reload only the named domains; a full reload only when the version skipped |
| M5 | `BroadcastState` re-parses and re-sends every mixer slot's cached JSON, the focused one too, with the `pluginStateBase64` that `SerializePresetForUi` strips. The UI then discards them | `PluginControllerBroadcast.cpp:200-219` | About 1 MB extra parse, escape and JS parse per state with a hosted-state preset (10-30 ms) | Skip the focused id; send each slot once per page load, or scrub at JSON level |
| M6 | Each UI knob event copies the scene graph and re-serialises the whole preset with `dump(2)` | `PluginControllerSignalPath.cpp:93`, :104; effect presets `:415` | Under 1% for a 6 KB preset; 2-10 ms per event (12-60% of the thread at 60 Hz) for hosted-state presets | Lazy mirror (see REAPER fix 3) |
| M7 | Host-state blob: `SerializeToJson`, then parse, then `dump()` of the same preset; non-focused slots deserialise and re-serialise; `uiSettings` emitted twice; a working-copy refresh every second during edits | `PluginControllerHostState.cpp:252`, :330-343 | About 0.5 ms typical; 15-30 ms with hosted state, plus a stall every second | `PresetStorage::ToJson(const Preset&) -> json` shared by all three serialisers; refresh only once a host has asked off-thread |
| M8 | Full broadcasts where `PresetOnly` would do: effect-preset apply, and the metronome BPM slider (each `input` event does a Full broadcast, a SQLite save and a shared-sync touch) | `PluginControllerEffectPresets.cpp:416`, `:466`; `PluginControllerMetronome.cpp:49-57` | A Full state costs the UI about 2-3x a PresetOnly one | `PresetOnly`; a small `metronomeState` message and a debounced save |
| M9 | The factory archive (7 MB) is read, every resource extracted, and the file hashed byte by byte on every launch, per instance, when unchanged | `PluginControllerPresetArchive.cpp:593-612`, `PresetArchiveSupport.cpp:328-358`, :497 | 20-40 ms and about 20 MB peak per instance | Store size and mtime; when they match, skip the hash and extraction |
| M10 | The resource library is copied 3-4 times on load; `ResourceFromJson` stats `resources/indexes/<rel>` before `resources/<rel>`, failing about 550 stats per load | `PluginControllerResources.cpp:2178-2185`, `ResourceLibrary.cpp:106-117` | 50-100 ms per instance; the stat order alone 5-15 ms | Try the current convention first (trivial); share a snapshot keyed on the sync version |
| M11 | Every UI message is JSON-parsed two or three times (adapter `:1495`, `PluginController.cpp:436`, `MessageDispatcher.cpp:18`); outbound messages are copied about five times | | Shows on multi-MB payloads | Substring checks before parsing; a `std::string` callback end to end |

## Audio thread and effects

The plumbing (mixer, executors, global chains, controller) is clean: no allocation or
blocking lock on the block path, and about 2-3 µs a block in total. The gains are in skipping
work and in the effects.

| # | Finding | Where | Estimated gain | Fix |
|---|---|---|---|---|
| A1 | IR Cab with a mono IR on a mono guitar runs two identical convolutions plus doubled output filters (**verified**) | `IRCabEffect.h:188-192`, :276-292 | 40-45% of the cab (about 2 µs x64, 5 µs A78); exact | On a Mono layout with mono IRs, no pan and LR split off: run L, copy to R; copy convolver state on the switch to stereo. Same for Simple Cab when `!mStereo`, and IR Reverb with a mono file (50% and 1.15 MB per 3 s IR) |
| A2 | No per-node silence skipping. A tailing preset runs its whole chain on exact zeros for the 2-bar tail (about 4 s, about 2x block cost). Behind a closed noise gate everything runs at full cost, because the mixer's sleep gate needs -80 dBFS at the raw input | `MultiPresetMixer.cpp:1417`, :1622; `SignalGraphExecutorPlan.cpp:461-748` | Tail window from about 2x to about 1.2x; near zero when idle behind a closed gate | First step: in a tailing instance, 250 ms after the input fade, skip nodes before the first delay or reverb. Later: a per-node `SilenceSleepGate` on non-time-based nodes |
| A3 | "Can ring out" counts bypassed delays and reverbs, so a preset with its reverb off gets the 4 s double-chain tail; the disabled global Doubler makes every session's sleep hold 5 s instead of 1 s | `SignalGraphExecutor.cpp:222`, :747; `MultiPresetMixer.cpp:269`, :1338 | A 21 ms fade instead of 4 s at 2x | Enabled-only `canRingOut`, evaluated at retire time. Trivial |
| A4 | Drive pedals oversample 8x on every model; the code's note says only high-gain models needed it. Biased stages recompute `Apply(bias)` per oversampled sample | `DrivePedal.h:124-132`; `DriveStages.h:176`, :184 | About 40% on low-gain models; 10-25% exact on biased fuzzes | Per-model factor in the voicing table (calibrate, aliasing check); cache `Apply(bias)` at `Design` |
| A5 | Compressor: `log10f`+`powf` per channel per sample (Opto adds `sqrt`); linked or mono computes the same envelope twice; no `ProcessMono` | `CompressorEffect.h:169`, :182, :451-470 | 3-5x on the effect (about 2-4 µs a block) | One envelope when linked; fast log2/exp2; `ProcessMono` |
| A6 | Chorus, Flanger and Tremolo do per-sample `sin`/`tanh`/`fmodf`/`%` (open since 2026-10-18); auto-wah redesigns its filter every sample | `ChorusEffect.h:86`, :242; `FlangerEffect.h:110`, :315; `TremoloEffect.h:311`; `WahEffect.h:508-555` | 3-6x per effect; 70-85% of auto-wah | `FastSin01` phasors, masked rings, feedback skipped at 0, hoisted constants; wah redesign every 8-16 samples |
| A7 | Pitch effects: no mono path (Signalsmith HQ prepared for 2 channels; Hybrid and STFT Transpose run separate L/R engines); Octave runs a voice at level 0 | `TransposeEffect.h:68`, `HybridTransposeEffect.h:479`, `StftTransposeEffect.h:573`, `OctaveEffect.h:210` | About 45-50% | One-channel engine on a Mono layout; skip a zero-level voice |
| A8 | Graphic EQ runs 0 dB bands as identity biquads | `GraphicEQEffect.h:351`, :66 | In proportion to flat bands; exact | Per-band active flag. Trivial |
| A9 | Reverbs keep full cost on silence; Verbal already tracks `mActiveQuiet` but only for hand-over | `VerbalEffect.h:205`, :318 | Average CPU and battery only | Dry-only past a near-silence threshold |

Below the 10% bar but trivial: per-node timing and `MixerTelemetry::Record` every block while
the UI is open (gate on the 30 Hz metering countdown); the latency notice sent twice
(`PluginProcessorAdapter.cpp:1366`); a hosted plugin with 32 or more channels allocating per
block (`JuceHostedPluginEffect.cpp:949`).

## Memory

| # | Finding | Where | Size | Fix |
|---|---|---|---|---|
| R1 | IR Reverb at Full (the default) keeps the whole IR: about 16 B a frame per convolver, plus the raw impulses, plus a resampled copy. A scene switch re-reads the WAV and rebuilds every convolver | `IRReverbEffect.h:662`, :603-636, :812; `PluginControllerMixer.cpp:323` | A 30 s quad IR: about 92 MB of convolvers + 23 MB raw (+23 MB resampled); -80 dB energy is reached at 6.2 s | Trim tails at about -90 dB remaining energy (needs a listening A/B); a process-wide cache of built spectra keyed on file, rate and quality; free raw impulses once played back |
| R2 | Looper (standalone): capture buffer value-initialised as soon as the looper is enabled, plus a capture-sized spare before any loop exists | `LooperService.cpp:38-47`, :253-305 | About 92 MB at 60 s / 48 kHz with nothing recorded; about 460 MB at 300 s | Allocate the spare during the first take; lazily committed or non-zeroed capture memory |
| R3 | The same preset is held 5-6 times per instance: `mActivePresetJson` (a pure duplicate of the cache entry), the baseline, the relay blob, the scene graph copy, all as `dump(2)` | `PluginControllerMixer.cpp:259`, :366 | About 5-6 MB per instance with a 1 MB hosted-state preset | Drop `mActivePresetJson`; compact `dump()` internally |
| R4 | Verbal: two full engines at maximum size; a machine change memsets the incoming engine on the audio thread | `VerbalEffect.h:403`, :436 | About 7 MB an instance at 48 kHz, 28 MB at 192 kHz; 0.1-0.3 ms on the audio thread | Clear only the new machine's lines |
| R5 | NAM model cache: 512 MB budget never lowered; each entry keeps parsed and packed weights; preview models stay cached | `NamModelCache.cpp:47` | About 50 MB after browsing 440 models | 64 MB budget; drop `data` after packing; evict on preview cancel |
| R6 | Practice Tool: a render thread and ring in every instance; a loaded track (about 115 MB for 5 minutes) is kept after editor close; the resample on a rate change runs in `Prepare` under the DSP lock | `PracticeToolService.cpp:70-160` | | Start lazily; unload on editor close; resample off the lock |

## Checked and ruled out

- Allocations or blocking locks on the audio thread, the node automation route, and the
  telemetry gating (hidden-UI, rate limits, roster).
- The tuner and analyser idle paths, and the denormal handling.
- Per-block copies in the adapter (pointers are passed in place); per-block parameter
  polling.
- The shared-sync poll (one small SELECT every 2 s); JsonStore WAL and transactions; the
  dirty check (0.1 ms every 0.4 s).
- NAM core buffers, the IR cab's footprint, static tables (`PolyNoteModelData.inc` is
  constexpr), and the voice pool caps.
- Already settled in earlier reviews: NAM inference, the convolver FFT, the Heavy American,
  and PitchTracker.
