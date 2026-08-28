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
| `PluginController.cpp`                          | Lifecycle, audio callback, state (de)serialisation, idle loop, tuner |
| `controller/PluginControllerPresets.cpp`        | Preset load/save/apply, folders, favourites, setlists |
| `controller/PluginControllerSignalPath.cpp`     | Node graph edits, composite edit target               |
| `controller/PluginControllerResources.cpp`      | Resource library: import, edit, delete, usage index   |
| `controller/PluginControllerCustomEffects.cpp`  | Blends, custom effects, composites                    |
| `controller/PluginControllerRiffs.cpp`          | Riff capture, editing, playback, library index        |
| `controller/PluginControllerBroadcast.cpp`      | `BroadcastState` and the `Send*ToUI` pushes           |
| `controller/PluginControllerSettings.cpp`       | App settings load/apply/persist                       |
| `controller/PluginControllerHostedPlugins.cpp`  | Third-party plugin state capture and restore          |
| `controller/PluginControllerPresetArchive.cpp`  | Factory archives and archive sessions                 |
| `controller/PluginControllerLayouts.cpp`        | Custom effect layouts and their images                |
| `controller/PluginControllerAutomation.cpp`     | Automation slots, MIDI learn, setlist/scene switching |
| `controller/PluginControllerMixer.cpp`          | Mixer slots and levels                                |
| `controller/PluginControllerMetronome.cpp`      | Click track                                           |
| `controller/PluginControllerDemo.cpp`           | Demo render and practice-tool transport               |
| `controller/PluginControllerEffectPresets.cpp`  | Per-effect user parameter presets                     |

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
| `MetronomeService`               | Click track and riff-capture guidance click — one engine, guidance overrides it |
| `TelemetryPublisher`             | The three metering feeds, their rate limits and the diagnostics roster |
| `ControlSurfaceQueue`            | MIDI in, and setlist/scene requests parked for the message thread     |
| `PracticeToolService`            | Backing-track playback with tempo/pitch shift                        |
| `DemoPreviewService`             | Demo audio preview mixed into the input                              |
| `SignalTestService`              | Test-tone injection and the measurement it reports                   |
| `TunerService`                   | Pitch readings handed from the audio thread to the UI                |

What is left on `PluginController` itself is the shared core every area needs
— the host, the mixer, the DSP lock, the active preset, app settings, the
document store — plus the message handlers, which are declarations in the
header and definitions in the files above.

**Adding a controller method:** declare it in `PluginController.h`, define it in
whichever file above owns that feature. A new file needs registering in
`core/CMakeLists.txt` (`GUITARFX_CORE_SOURCES`).

**Size budget:** `node tools/check-cpp-file-sizes.js` fails if a source file
grows past 800 lines, or if a file already over it gets bigger. CI runs this
(`.github/workflows/cpp-structure.yml`). Re-pin deliberately with `--update`.

## Common Agent Tasks

### Add a New Effect

1. Implement EffectProcessor in core/src/dsp/effects/.
2. Register it via EffectRegistry in BuiltinEffects.h.
3. Define parameters (ranges, defaults) and category.
4. Update docs/fx-library.md if behavior changes.

### Add or Change a UI Message

1. Update types and handler in core/ui/ts/messages.ts.
2. Route it in core/src/dispatcher/MessageDispatch*.cpp, then implement the
   handler in the matching core/src/controller/PluginController*.cpp (declare it
   in core/src/PluginController.h).
3. Keep messages backward compatible and validate payloads.
4. Update docs/user-interface.md for the protocol contract.

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
- Validate parameter ranges and resource presence; fail fast with clear errors.
- Graphs must be acyclic; invalid graphs should not reach Process().

## Build and Test Shortcuts

- Configure core: cmake -G "Visual Studio 18 2026" -A x64 -S core -B core/build
- Build JUCE standalone debug: cmake --build juce/builds --config Debug --target SoundshedGuitar_Standalone
- UI build: cd core/ui && npm run build
- **UI checks (run this before any UI change is done): cd core/ui && npm run verify**
  Runs typecheck, eslint, vitest, the import-cycle gate, the file-size budget and
  the stylesheet-reachability check. The same set runs in CI
  (`.github/workflows/ui-checks.yml`).
- UI boot check: `node tools/agent-ui-debug/smoke-test.mjs` — builds the UI, syncs
  it into the Standalone artefact, launches the app and asserts it booted clean.
  This is the only check that catches an import-cycle TDZ crash; `tsc` cannot see
  those. Run it for anything that moves code between modules.
- Tests (Debug): cd core/build && ctest -C Debug --output-on-failure
- C++ format (core/ only; juce/ has its own config): `clang-format --style=file -i <files>`
- C++ structure check: `node tools/check-cpp-file-sizes.js`
- Skip the slow benchmarks: add `-LE benchmark`
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
  split. `ts/signalPath/`, `ts/presets/` and `ts/amp3d/` all follow this.
- **No top-level DOM access in a feature module.** DOM roots shared across a
  feature go in one `state.ts` (see `ts/signalPath/state.ts`), which also owns any
  mutable state, exposed through accessors. `export let` cannot be assigned by an
  importing module, so a bare `export let` compiles and then fails at runtime.
- **Senders are not receivers.** Outbound `postMessage` wrappers belong in
  `bridge.ts`, not alongside the inbound handlers. Two of them sitting in
  `messages.ts` were single-handedly responsible for a 20-module import cycle.
- **Ask for a re-render, do not reach for the renderer.** A submodule that needs
  the feature redrawn calls `requestSignalPathRender()` / `requestNodeParamsRefresh()`
  / `requestPresetLibraryRefresh()`; the owning module registers the real
  implementation once at load. This is what stops a leaf module importing the
  1,800-line facade and forming a cycle.
- **Mark type-only imports as `import type`.** `verbatimModuleSyntax` is on, so
  those are erased and cannot create a runtime cycle — which is why the cycle
  checker ignores them.

`npm run check:cycles -- --list` prints the current tangles. The baseline in
`scripts/cycles-baseline.json` is compared at feature level, so splitting a file
into a directory does not trip it, but entangling two features does.

### Stylesheets

`css/signal-path/` and `css/modals/` are ordered parts of what used to be one
file each. **The `<link>` order in `index.template.html` is the cascade** — the
parts must stay in the order listed there. `npm run check:stylesheets` fails if a
stylesheet is not reachable from the template, which otherwise shows up as one
silently unstyled panel.
