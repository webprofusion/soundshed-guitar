# Agent Quickstart

This doc is the minimal, high-signal guide for AI agents working in this repository.

## Minimal Context Bundle

If you only load a few files, use these:

- docs/architecture-overview.md
- docs/signal-chain.md
- docs/fx-library.md
- docs/data-models.md
- docs/user-interface.md
- .github/copilot-instructions.md

## Core Entry Points

- Application controller and UI bridge: core/src/PluginController.cpp, core/src/UiBridge.cpp
  (the controller is split across core/src/controller/ — see "Where controller code lives" below)
- DSP graph executor: core/src/dsp/SignalGraphExecutor.h
- Mixer: core/src/dsp/MultiPresetMixer.h runs the per-block signal flow and composes
  PresetVoicePool (preset instances from install to retirement, and the swap, tail and
  crossfade rules), GlobalChainEngine (the global pre/post executors, rebuilt and
  swapped), DspReaper (where retired chains are destroyed), TunerEngine and MixerTelemetry
- Effect base and registry: core/src/dsp/EffectProcessor.h, core/src/dsp/EffectRegistry.h
- Preset schema and storage: core/src/presets/PresetTypes.h
- UI messages and state: core/ui/ts/messages.ts, core/ui/ts/state.ts

## Where Controller Code Lives

`PluginController` is one class spread over several translation units. The
header `core/src/PluginController.h` declares all of it; the definitions are
grouped by feature so you can open the file for the thing you are changing
instead of scrolling one enormous one:

| File (`core/src/`)                              | Owns                                                 |
| ----------------------------------------------- | ---------------------------------------------------- |
| `PluginController.cpp`                          | Lifecycle, audio callback, idle loop, and the request handlers too small to be an area |
| `controller/PluginControllerHostState.cpp`      | The state blob a DAW saves with its project; editor window size |
| `controller/PluginControllerStorage.cpp`        | Document store open/migration/quarantine; UI storage documents |
| `controller/PluginControllerSharedSync.cpp`     | Cross-instance sync: the version doc, the poll, the reload |
| `controller/PluginControllerDiagnostics.cpp`    | Session log, debug snapshots, diagnostics and performance feeds |
| `controller/PluginControllerGlobalChain.cpp`    | Global signal chain, `setParameter` aliases, input stage |
| `controller/PluginControllerNodeControl.cpp`    | Live node control: enable, param, resource; model/IR loading |
| `controller/PluginControllerTuner.cpp`          | The signal-path test tone (the tuner answers its own messages) |
| `controller/PluginControllerPresets.cpp`        | Preset load/save/apply, folders, favourites, setlists |
| `controller/PluginControllerSignalPath.cpp`     | Node graph shape: add, remove, reorder; composite edit target |
| `controller/PluginControllerResources.cpp`      | Resource library: import, edit, delete, usage index   |
| `controller/PluginControllerCustomEffects.cpp`  | Blends, custom effects, composites                    |
| `controller/PluginControllerRiffs.cpp`          | Riff capture (including its audio-thread block), editing, playback, library index |
| `controller/PluginControllerBroadcast.cpp`      | `BroadcastState` and the `Send*ToUI` pushes           |
| `controller/PluginControllerSettings.cpp`       | App settings load/apply/persist                       |
| `controller/PluginControllerHostedPlugins.cpp`  | Third-party plugin state capture and restore          |
| `controller/PluginControllerPresetArchive.cpp`  | Factory archives and archive sessions                 |
| `controller/PluginControllerArchiveInstall.cpp` | Installing tones downloaded from tone sharing (`installPresetArchives`) and removing them; the `toneSharing.installedPacks` record |
| `controller/PluginControllerLayouts.cpp`        | Custom effect layouts and their images                |
| `controller/PluginControllerAutomation.cpp`     | Automation slots, MIDI learn, setlist/scene switching |
| `controller/PluginControllerMixer.cpp`          | Mixer slots and levels                                |
| `controller/PluginControllerMetronome.cpp`      | Click track                                           |
| `controller/PluginControllerDemo.cpp`           | Demo preview and offline render                       |
| `controller/PluginControllerEffectPresets.cpp`  | Per-effect user presets: parameters, resources, config |

Free functions shared between those files live in `core/src/controller/internal/`
in namespace `guitarfx::controller_detail` — settings keys, NAM metadata
parsing, archive scoping, offline render, hosted-plugin identity. Each
controller TU opens with `using namespace guitarfx::controller_detail;`, so
call sites read unqualified.

### Services the controller owns

Areas with state and a lifetime of their own are separate classes, held as
`unique_ptr` members and constructed in `PluginController`'s constructor with
their dependencies injected by reference plus a `SendMessageToUI` callback.
Prefer adding to one of these over adding another member to the controller:

| Service (`core/src/controller/`) | Owns                                                                 |
| -------------------------------- | -------------------------------------------------------------------- |
| `MetronomeService`               | Click track and riff-capture guidance click — one engine, guidance overrides it. Meter/accents/subdivision are resolved into an immutable bar plan (`MetronomeSupport.h`); the sounds come from `MetronomeClickLibrary` |
| `TelemetryPublisher`             | The three metering feeds, their rate limits and the diagnostics roster |
| `ControlSurfaceQueue`            | MIDI in, and setlist/scene requests parked for the message thread     |
| `PracticeToolService`            | Backing-track playback with tempo/pitch shift and its own EQ — split across `PracticeToolService.cpp` (lifecycle, loading, transport, audio-thread mix), `PracticeToolServiceRender.cpp` (the background render thread) and `PracticeToolServiceMessages.cpp` (its twelve UI messages) |
| `DemoPreviewService`             | Demo audio preview mixed into the input                              |
| `SignalTestService`              | Test-tone injection and the measurement it reports                   |
| `TunerService`                   | Pitch readings handed from the audio thread to the UI, and the tuner's own messages (on/off, live monitoring, reference pitch) |
| `HostStateRelay`                 | The DAW's saves, restores and program changes from other threads, handed to the message thread: a fallback blob for a save it cannot answer in time, and a queue for restores it cannot start in time |
| `ResourceFolderScanner`          | The resource browser's folder listing, on detached workers so a slow drive never stalls the message thread. A generation counter supersedes in-flight scans; `Shutdown()` (called from `~PluginController`, not left to member teardown) waits for the last worker |

What is left on `PluginController` itself is the shared core every area needs
— the host, the mixer, the DSP lock, the active preset, app settings, the
document store — plus the message handlers, which are declarations in the
header and definitions in the files above.

**A service that owns a feature outright answers its own messages.** It gets a
`RegisterMessageHandlers(MessageHandlerRegistry&)` and registers each message type
with a handler when the controller constructs it (`PracticeToolService`, `TunerService`
and `ResourceFolderScanner` do). `MessageDispatcher` tries that registry before its own routes,
inside the same JSON-error guard, and a type can have only one owner. The controller
then needs no `Handle*` forwarding method for it. Prefer this for a new message
whose handler only needs what the service already holds.

**Adding a controller method:** declare it in `PluginController.h`, define it in
whichever file above owns that feature. A new file needs registering in
`core/CMakeLists.txt` (`GUITARFX_CORE_SOURCES`).

**Size budget:** `node tools/check-cpp-file-sizes.js` fails if an unpinned source
file is over 800 lines, or if one of the already-listed files grows past its
pinned ceiling. Pins sit 20% above the file as last measured, so ordinary edits
to a known-large file pass and the check only fires on real growth — but new
files still have to come in under 800. CI runs this
(`.github/workflows/cpp-structure.yml`). The pins only ratchet down: `--update`
lowers pins for files that shrank and never raises one, and a pin more than 5%
looser than its file needs fails the check, so tighten it in the same change that
shrank the file. To let a file grow, use `--repin`; the raised number is the
review signal. The UI's `npm run check:sizes` works the same way.

**Fetched dependencies:** every `FetchContent_Declare` in `core/CMakeLists.txt` and
`core/cmake/` must pin a commit SHA or a release tag; `node tools/check-dependency-pins.mjs`
fails on a branch name, and CI runs it. To see which pins have newer upstream
commits or tags, run it with `--upstream`. Bump a pin deliberately, then rebuild and
run the tests.

## Common Agent Tasks

### Research and Create Artist Presets

For artist-inspired tones made in the running app, follow
[artist-tone-preset-workflow.md](artist-tone-preset-workflow.md). It covers
dated rig research, local/Tone3000 resource matching, scene authoring, folder
merging, and verification of both saved data and the audio path.

### Add a New Effect

1. Implement EffectProcessor in core/src/dsp/effects/.
2. Give it a fresh UUID in core/src/dsp/EffectGuids.h and the same constant in
   core/ui/ts/effectGuids.ts (`check-protocol` compares the two), then register it
   via EffectRegistry in BuiltinEffects.h.
3. Add it to `core/protocol/effect-aliases.json` with any legacy ids, and put
   those ids in the UI's `EFFECT_ALIAS_MAP`. `EffectAliasParityTests` holds the
   engine's registered aliases to that list and `tests/effectAliases.test.ts`
   holds the UI's, so a preset resolves to the same effect on both sides.
4. Define parameters (ranges, defaults) and category, as a `constexpr std::array<EffectParamSpec, N>`
   (`core/src/dsp/EffectParamSpec.h`): the one place the ranges live. `BuildParameterDefs` registers
   them, and `FindParamSpec`/`NormaliseParamValue` serve SetParam and GetParam without allocating
   (see WahEffect, SimpleCabEffect). A range spanning decades, a frequency most often, wants
   `LogTaper({...})` so the knob and automation move by ratio (docs/fx-library.md, Parameter tapers).
5. Design filters with `core/src/dsp/BiquadDesign.h` (RBJ designs clamped below Nyquist, their
   responses, TDF-II state, coefficient ramps) rather than another copy of the cookbook. An effect
   with a fixed, linear response should override `GetFrequencyResponse`: the engine can then answer
   `getEffectResponse` for its curve and render it as an IR (`core/src/dsp/EffectAnalysis.h`).
6. Give it an icon in `core/ui/data/effect-presentation.json` (both UIs read it) and run
   `node tools/gen-effect-presentation.mjs`; `npm run check:presentation` checks the entry.
7. Update docs/fx-library.md if behavior changes.

### Add or Change a UI Message

1. Update types and handler in core/ui/ts/messages.ts.
2. If a controller service owns the feature, register the handler in its
   `RegisterMessageHandlers()` (see "Services the controller owns"). Otherwise
   route it in core/src/dispatcher/MessageDispatch*.cpp, then implement the
   handler in the matching core/src/controller/PluginController*.cpp (declare it
   in core/src/PluginController.h).
3. Keep messages backward compatible and validate payloads.
4. Update docs/user-interface.md for the protocol contract.
5. Run `node tools/check-protocol.mjs` (part of `npm run verify`, and in CI). It reads
   what both sides send, route and handle, and fails when they disagree with each
   other or with `core/protocol/ui-messages.json` — a UI send nothing routes, a route
   nothing sends, a reply nothing handles, an undocumented type, or an effect GUID that
   differs between `EffectGuids.h` and `effectGuids.ts`. Adding a message means adding
   it to the manifest (`--update` re-pins from the code; review the diff).

### Load a Resource (NAM or IR)

1. GraphNode.resource uses ResourceRef (resourceType + resourceId preferred).
2. Resolve via ResourceLibrary; fall back to embeddedId or filePath.
3. Validate file existence and log errors on failure.
4. Update docs/data-models.md if behavior changes.
5. NAM calibration policy: interface calibration input level is injected for all
   calibratable NAM nodes with valid model resources; per-node `useCalibration`
   still controls whether metadata-based auto-gain is active.

## Realtime Safety and Validation

- Audio thread: no allocations, no locks, no blocking I/O.
- `SetParam` runs on the audio thread too, since MIDI and DAW automation apply there. A parameter
  whose change needs a rebuild records it and leaves the build to the message thread
  (`DeferredRebuild`, docs/fx-library.md "Where SetParam runs").
- Validate parameter ranges and resource presence; fail fast with clear errors.
- Graphs must be acyclic; invalid graphs should not reach Process().

## Build and Test Shortcuts

- Configure core: cmake -G "Visual Studio 18 2026" -A x64 -S core -B core/build
- Build JUCE standalone debug: cmake --build juce/builds --config Debug --target SoundshedGuitar_Standalone
- Android APK: `cd android && ./gradlew assembleDebug` — see `docs/android-build.md`
  for the toolchain versions, the `-Pssg.abis` switch, and what the Android build
  leaves out (WASM effects, plugin hosting, ASIO).
- UI build: cd core/ui && npm run build
- **UI checks (run this before any UI change is done): cd core/ui && npm run verify**
  Runs typecheck, eslint, vitest, the import-cycle gate, the uiState write-ownership
  gate, the protocol check, the effect presentation check, the file-size budget and the
  stylesheet-reachability check. The same set runs in CI
  (`.github/workflows/ui-checks.yml`).
- UI boot check: `node tools/agent-ui-debug/smoke-test.mjs` — builds the UI, syncs
  it into the Standalone artefact, launches the app and asserts it booted clean.
  This is the only check that catches an import-cycle TDZ crash; `tsc` cannot see
  those. Run it for anything that moves code between modules.
- Tests (Debug): cd core/build && ctest -C Debug -j 8 --output-on-failure
  About a minute here. The slowest tests set the pace, so a test that renders seconds of
  audio per case is worth trimming before it is added. If `MixerInstanceLockTests` fails,
  rerun its exe alone first: its riff-trim check counts missed blocks and can trip under
  `-j 8` load. NAM is built optimised even in Debug (`GUITARFX_OPTIMISE_NAM_IN_DEBUG`, see
  docs/neural-amp-modeler-core-integration.md); unoptimised it made this run several times slower.
- **Host validators:** `node tools/validate-plugins.mjs` runs pluginval at strictness 10
  on the VST3 (and the AU on macOS) and clap-validator on the CLAP, after
  `cmake --build juce/builds --config Release --target SoundshedGuitar_VST3 SoundshedGuitar_CLAP`.
  The first run downloads the pinned validators into `juce/builds/tools/`; pluginval's
  logs go to `juce/builds/validation/`. CI runs it in `build-windows.yml` (x64 legs)
  and `build-macos.yml`.
- **Did the sound change between versions?** `node tools/audio-ab/audio-ab.mjs <base> [<target>]`
  (target defaults to the working tree, so `... HEAD` shows what uncommitted changes do and
  `... 1.5.0` compares against the public release). It builds `core/tests/AudioSnapshot` in
  Release for each revision in a worktree of its own, renders every effect alone at its fresh-node
  defaults and the chains in `tools/audio-ab/snapshot.json` through a headless `PluginController`
  at default settings, and reports each as identical / rounding / inaudible / slight / audible /
  broken, with A/B/difference players in `report.html`. See `tools/audio-ab/README.md`.
- **NaN, infinity or float comparisons: test in Release, and on clang.** Release and
  RelWithDebInfo build with `/fp:fast` (MSVC) or `-ffast-math` (clang, so Android); Debug has
  neither, so a Debug pass proves nothing. MSVC Release breaks NaN comparisons; clang goes further
  and folds away `std::isnan`/`std::isfinite` and even NaN constants (clang 22 plain bit tests too). Detect
  non-finite values with `core/src/dsp/FiniteCheck.h`, and never use NaN or infinity as a sentinel.
  `FastMathNanTests` is the regression suite. To get Android's semantics on Windows, build the
  core with clang-cl (WASM off, as on Android; reuses the MSVC tree's fetched sources, and lld-link
  needs `avrt.lib` named for NAM's MMCSS calls):
  ```bash
  D="$(pwd -W)/core/build/_deps"
  cmake -G "Visual Studio 18 2026" -A x64 -T ClangCL -S core -B core/build-clangcl \
    -DGUITARFX_CORE_BUILD_TESTS=ON -DGUITARFX_CORE_ENABLE_WASM_EFFECTS=OFF \
    "-DCMAKE_EXE_LINKER_FLAGS=/machine:x64 avrt.lib" -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$D/nlohmann_json-src \
    -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH_LINEAR=$D/signalsmith_linear-src \
    -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH_STRETCH=$D/signalsmith_stretch-src \
    -DFETCHCONTENT_SOURCE_DIR_STFTPITCHSHIFT=$D/stftpitchshift-src \
    -DFETCHCONTENT_SOURCE_DIR_GUITARFX_MINIZ_SRC=$D/guitarfx_miniz_src-src \
    -DFETCHCONTENT_SOURCE_DIR_GUITARFX_SQLITE_SRC=$D/guitarfx_sqlite_src-src \
    -DFETCHCONTENT_SOURCE_DIR_GUITARFX_MINIMP3_SRC=$D/guitarfx_minimp3_src-src \
    -DFETCHCONTENT_SOURCE_DIR_GUITARFX_WASMTIME_SRC=$D/guitarfx_wasmtime_src-src \
    -DFETCHCONTENT_SOURCE_DIR_NEURALAMPMODELERCORE=$D/neuralampmodelercore-src \
    -DFETCHCONTENT_SOURCE_DIR_NAM_OVERSAMPLER_AUDIO_DSP_TOOLS=$D/nam_oversampler_audio_dsp_tools-src
  cmake --build core/build-clangcl --config Release --target FastMathNanTests
  ```
- C++ format (core/ only; juce/ has its own config), in this order:
  `uncrustify -c tools/uncrustify.cfg -l CPP --no-backup <files>` then
  `clang-format --style=file -i <files>`
- C++ structure check: `node tools/check-cpp-file-sizes.js`
- Benchmarks (`NAMPerformanceBenchmark`, `ResourceLoadBenchmark`, `SignalChainThreadingBenchmark`,
  `TransposeBenchmark`) print measurements and assert nothing about them, so ctest only
  registers them when configured with `-DGUITARFX_TEST_BENCHMARKS=ON`; then `ctest -L benchmark`
  runs them and `-LE benchmark` skips them. Or run an executable directly. Their numbers mean
  something in Release, not Debug.
- **Where audio-thread CPU goes:** `core/tests/SteadyStateProfiler.cpp` — build it
  `RelWithDebInfo` (it needs symbols) and run it from the repo root, which is where it
  looks for `resources/`:
  `./core/build/tests/RelWithDebInfo/SteadyStateProfiler.exe --profile applive --block 64 --diagnostics both`
  It drives the mixer on its own thread doing the host callback's per-block work and
  samples that thread's stacks, so costs land on real function names. `--profile` picks
  the chain: `light` (framework overhead only), `baseline` (all-DSP, no model files),
  `namconv`, or `applive` (the chain a real session runs — NAM, IR cab, room reverb,
  delay, doubler). `--callers-of malloc` is the one that turns "the heap is busy" into
  the line that allocates. ctest runs it too, unlabelled, but only as a smoke
  check of the sampler: one half-second pass per mode on the `baseline` chain, failing
  if a sampling pass captures nothing, with a 120 s timeout. What it prints there is
  not a profile.
- The signal-chain mutation stress test (~15 min concurrency fuzzer, random seed) is not
  registered with ctest by default. Run its executable directly, or reconfigure with
  `-DGUITARFX_TEST_STRESS=ON`. It writes `SignalChainMutationStressTest-last-trace.log`
  next to the working directory; the seed in that file's header replays the same
  mutation sequence via `GUITARFX_STRESS_SEED`.

## Live UI Testing (Computer-Use Debugging)

Static code review misses regressions that only show up at runtime — e.g. a DOM
`insertBefore()` against a node that moved to a different parent during a layout
rework, which throws and silently kills the rest of that render pass with no
compile error. Before declaring a UI-facing fix (especially in `core/ui/ts/signalPath/`,
mixer/tab rendering, or anything touching layout structure) actually done, run it.

The app's UI is WebView2 (Chromium) — a **different surface** from a normal browser
tab. Browser-automation/"computer use" tools cannot see or click this native window.
Instead, launch the Standalone build with WebView2's remote-debugging port enabled
and drive the *live, backend-connected* UI via Chrome DevTools Protocol:

```bash
cmake --build juce/builds --config Release --target SoundshedGuitar_Standalone --parallel
WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9333" \
  "juce/builds/SoundshedGuitar_artefacts/Release/Standalone/Soundshed Guitar.exe" &
curl -s http://127.0.0.1:9333/json/list   # -> webSocketDebuggerUrl
node tools/agent-ui-debug/cdp-tool.mjs "$WS" --eval "<js>" --screenshot out.png
```

Full workflow, gotchas, and how to verify backend-truth (not just optimistic
client UI state) via the in-app debug-state snapshot: `tools/agent-ui-debug/README.md`.
Always `taskkill //F //IM "Soundshed Guitar.exe"` and remove any test presets
from `%APPDATA%\Soundshed Guitar\` when done — it's the real user profile.

## UI Module Conventions

`core/ui/ts` is plain ES modules compiled by `tsc` with no bundler, so one source
file is one runtime module fetched through JUCE's resource provider. A few rules
keep that tractable:

- **A feature directory, behind the original module path.** A large feature lives
  in `ts/<feature>/`, and `ts/<feature>.ts` stays as the public facade that
  re-exports it. Importers keep using `./<feature>.js` and never learn about the
  split. `ts/signalPath/` and `ts/presets/` both follow this.
- **No top-level DOM access in a feature module.** DOM roots shared across a
  feature go in one `state.ts` (see `ts/signalPath/state.ts`), which also owns any
  mutable state, exposed through accessors. `export let` cannot be assigned by an
  importing module, so a bare `export let` compiles and then fails at runtime.
- **Senders are not receivers.** Outbound `postMessage` wrappers belong in
  `bridge.ts`, not alongside the inbound handlers. Two of them sitting in
  `messages.ts` were single-handedly responsible for a 20-module import cycle.
- **A control surface two features need is a component with a binding, not a
  second copy.** `ts/eqPanel.ts` is the four-band EQ — knobs, curve, toggle —
  and the Global EQ and the Practice Tool's backing-track EQ each supply only an
  `EqPanelBinding` saying where the values live and how a change reaches the
  engine. `ts/knob.ts` is the same idea one level down. Before adding an EQ (or
  any such surface) to a new feature, write a binding.
- **Ask for a re-render, do not reach for the renderer.** A submodule that needs
  the feature redrawn calls `requestSignalPathRender()` / `requestNodeParamsRefresh()`
  / `requestPresetLibraryRefresh()`; the owning module registers the real
  implementation once at load. This is what stops a leaf module importing the
  1,800-line facade and forming a cycle.
- **Mark type-only imports as `import type`.** `verbatimModuleSyntax` is on, so
  those are erased and cannot create a runtime cycle — which is why the cycle
  checker ignores them.
- **A lower module that needs a higher one gets a hook or a callback, not an
  import.** `presets/refresh.ts` (`requestPresetUIRender`), `signalPath/render.ts`
  (`requestNodeParamsPanel`) and `toneSharingPanel/refresh.ts`
  (`requestBrowseReload`) are registered once by the module that owns the work;
  `presets/history.ts`'s `stepPresetHistory` takes the loader as an argument.
- **Write shared state through its owner.** `uiState` is readable anywhere, but a
  migrated slice has one module with commands to change it:
  `appSettingsStore.ts` (`updateAppSetting` records a setting *and* sends it),
  `presetLibraryStore.ts` (the preset list, cache, filtered view, active and
  loading ids, active scene) and `mixerStore.ts`. Add to one of those rather than
  assigning into `uiState` from a feature module.

`npm run check:cycles -- --list` prints any import cycles. The baseline in
`scripts/cycles-baseline.json` is empty and compared module by module, so any new
cycle fails — inside a feature as well as between two.

`npm run check:state-writes` pins which modules write each top-level `uiState`
field (`scripts/state-writers-baseline.json`) and fails when a module starts
writing one it is not pinned for; `-- --where <field>` lists the write sites. Move
writes behind an owner, then tighten the pin with `-- --update`.

### Stylesheets

`css/signal-path/` and `css/modals/` are ordered parts of what used to be one
file each. **The `<link>` order in `index.template.html` is the cascade** — the
parts must stay in the order listed there. `npm run check:stylesheets` fails if a
stylesheet is not reachable from the template, which otherwise shows up as one
silently unstyled panel.
