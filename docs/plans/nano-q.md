# Soundshed Nano Q: Soundshed Nano without JUCE

Status: **first working build, 2026-10-09. Windows verified; macOS and Linux not yet.** Branch
`nano-q`, in `nanoq/`. Soundshed Guitar Nano ([native-ui.md](native-ui.md)) is untouched and
still the shipping native product.

**Soundshed Nano Q** is Soundshed Nano rebuilt on [Cycfi Q](https://github.com/cycfi/q)'s
**QPlug** plugin framework and its **Elements** GUI library, with no JUCE anywhere in the
program. It is a separate app, with its own name, plugin id and bundle id
(`com.soundshed.guitar.nanoq`), so it installs beside Soundshed Guitar and Soundshed Guitar
Nano. It shares their engine and their profile (settings, `soundshed.db`, presets, resource
library), so a user can move between them.

## Why this is mostly a UI port

`core/` has no JUCE in it (one stale comment aside), and Nano already reaches the engine only
through `core/src/uiclient`: the same JSON messages the web UI sends, parsed into
`ClientState`. So everything JUCE did for Nano falls into three jobs:

| JUCE did | Nano Q does it with |
|---|---|
| Audio and MIDI callbacks, plugin formats (VST3, AU, CLAP, standalone) | QPlug, which builds one CLAP and has clap-wrapper make the VST3, AUv2 and a standalone app from it |
| File dialogs, thread hops, message thread, paths (`PluginProcessorAdapter` as `IPluginHost`) | `nanoq/src/Engine.*`, `FileDialogs.*`, `Paths.*` |
| The 10,000 lines of Components in `juce/source/nativeui` | `nanoq/src/ui/`, Elements elements drawn from the same theme tokens |

What is **not** reused from `juce/`: all of it, with one exception: the generated colour tokens
(`juce/source/nativeui/theme/ThemeTokens.generated.h`, a plain header made from the web UI's
CSS) and the Inter font files, so a theme looks the same in every product.

## Architecture

```
host or standalone ──► QPlug plugin (CLAP; VST3/AU/standalone via clap-wrapper)
                         ├─ NanoQController   host parameters, state, owns the engine
                         ├─ NanoQProcessor    audio and MIDI → PluginController::ProcessAudio
                         └─ NanoQPresenter    the editor: UiSession + Shell
Engine  ── guitarfx::PluginController on a thread of its own (its "message thread")
UiSession ── pumps engine messages to UiClient on the UI thread, 16 ms
Shell   ── Elements UI, rebuilt per region from ClientState
```

**The engine runs on its own thread.** `PluginController` expects a message thread that it can
post to (`IPluginHost::RunOnMainThread`) and that owns its non-realtime state. QPlug hands a
plugin no such thing, so `Engine` owns one: every call that is not the audio callback runs
there. The editor talks to it by posting requests; what it sends "to the UI" is queued while an
editor is open (and dropped when none is, as in the JUCE editor) and collected by `UiSession`'s
timer on the host's UI thread. `SerializeState` and `DeserializeState` are called from the
host's thread and hand themselves over, as they already do for AU, AAX and LV2.

**Host parameters** are the engine's automation slots, in the JUCE adapter's order (stable
defaults, custom slots padded to `kMaxCustomSlots`, later defaults after), one normalised
parameter each. A host change is queued by the audio thread and applied at the top of the next
block under the DSP lock, or at once while audio is stopped: the JUCE adapter's rules. The
values are not saved by QPlug; the engine's state blob (in the controller's extra state) holds
them, and after a load the controller asks the host to rescan values (see below).

**The UI** is owner-drawn, as Nano's is: `ui/Widgets.*` are small Elements elements (text,
button, parameter slider, level bar, list row, toast, dialog surface), and `ui/Layout.h` wraps
Elements' tiles for building from pointers. `Shell` owns five swappable regions (top bar,
scenes, chain, page, transport) and rebuilds the ones a message touched, comparing a signature
of what each shows first; the values of the page being edited and the meters are patched in
place instead, so a drag is never interrupted by the value it just set coming back.

### Things QPlug and Elements do not give, and what Nano Q does

- **No rescan request.** After a state load the engine's parameter values can differ from what
  the host last read, and CLAP hosts (clap-validator) treat that as a bug unless the plugin
  asks for a rescan. `src/clap_entry.cpp` replaces QPlug's entry shim to remember the
  `clap_host`, and `NanoQController::RescanHostValues` calls `params->rescan`.
- **No denormal guard.** `src/NoDenormals.h`, around `process`.
- **No standalone flag.** `RunningAsStandalone()`: clap-wrapper's standalone links the plugin
  into its own executable.
- **No main-thread handle.** The engine thread, above.
- **No SVG.** Artist draws no SVG, so effect icons are not drawn yet (the category colour is).
- **Tile layout traps.** A column is as wide as its narrowest child's *maximum* width, and a
  scroller reports its content's height as its maximum. `Row`, `Col`, `VScroll` and `HScroll`
  in `ui/Layout.h` correct for both.

## Building

```powershell
nanoq\build.cmd Release          # configures (first time: ~5 min, fetches Q, Elements, clap-wrapper, NAM, ...) and builds
```

Products land in `nanoq/build/Release/products/`: `CLAP/`, `VST3/`, and
`Standalone-nanoq_standalone/`, each beside its fonts and the engine's runtime data
(`<name> Resources/` next to a CLAP or the exe, `Contents/Resources` in a VST3), the same
data Nano ships (`presets assets metronome demo data images` of `core/ui`).

Dependencies are fetched and pinned (Q to a commit; Q pins Elements, clap, clap-wrapper).
`GUITARFX_CORE_BUILD_WEB_UI` (new, default on) lets a native-UI product skip core's npm build
of the web UI; Nano Q turns it off, so building it needs no Node.

## Testing and driving it

`nanoq/tools/` (PowerShell, Windows):

| Script | Does |
|---|---|
| `dev.ps1 -Profile <dir> [-Shot png] [-Wait n] [-NoRun]` | Stops a running Nano Q, builds, reports only errors, runs the standalone on a throwaway data root and captures its window |
| `run-isolated.ps1` | The run-and-capture half |
| `window-shot.ps1 -Out png [-Resize WxH]` | DPI-aware `PrintWindow` capture, which works when the window is behind others |
| `click.ps1 -X -Y [-ToX -ToY] [-Wheel n] [-Right] [-Hover] [-Text s]` | Posts mouse and character messages straight to the view window, in capture coordinates; leaves the user's pointer alone |

**Always run with a profile argument.** The engine's data root is not where the host says: it
comes from the `APPDATA` environment variable through core's `FileSystem`. Nano Q's
`SOUNDSHED_NANOQ_PROFILE` calls `FileSystem::SetPlatformRootOverride`, which is what keeps a
test run off the real profile.

Plugin validation reuses `tools/validate-plugins.mjs`, which now takes `--product`:

```powershell
node tools/validate-plugins.mjs --artefacts nanoq/build/Release/products --product "Soundshed Nano Q" --formats CLAP,VST3
```

## Progress

| Area | State |
|---|---|
| Engine hosting, audio, MIDI, state, host parameters, FTZ/DAZ | Done |
| CLAP (clap-validator 0.4.1) | **35 passed, 0 failed**, 9 skipped, Windows |
| VST3 (pluginval 1.0.4, strictness 10, editor tests on) | **Passes**, Windows |
| Standalone app | Runs; audio device settings are clap-wrapper's own window (Alt+Space) |
| Top bar: preset name, previous/next, save, preset menu | Done |
| Scenes strip | Done (select); add, rename, remove from the preset menu |
| Chain strip: select, bypass, add (picker with categories), remove, move, parallel lanes | Done; the right-click menu is the only card menu (no long press yet) |
| Effect page: bypass, Main/Advanced, sliders, switches, enum steppers, effect presets | Done |
| Presets page: search, All/Favourites/Recents/Folders, favourite, delete, load with unsaved-changes confirm | Done |
| Setlists: choose, step, jump to a slot | Done (editing is in Soundshed Guitar, as in Nano) |
| Input and output sheet (global chain) | Done |
| Tuner, metronome (tap tempo, meter, subdivision, click, volume) | Done |
| Settings: theme, limiter, preset switch tails, NAM quality, oversampling, anti-alias | Done |
| Toasts for the engine's notifications | Done |
| **Tones page** (tone sharing) | **Not done.** Needs an HTTP client: core has none (JUCE's `URL` did it). Windows could use WinHTTP, macOS NSURLSession, Linux libcurl, plus image decoding for pack pictures |
| **Effect icons** | **Not done.** Artist cannot draw SVG; needs a small SVG path renderer |
| UI scale setting, wrapped chain layout, touch long-press, compact (phone) layout | Not done; Nano Q targets desktop plugin windows first |
| macOS and Linux builds | **Untested.** A Linux compile in a container was started and abandoned (see the session notes); QPlug documents both platforms |
| Resource browsing (NAM and IR models) | Not in JUCE Nano yet either (its phase 4) |
| CI | None yet; `nanoq/build.cmd` and the validator command above are what a workflow would run |

## Decisions and open questions

- **Separate directory, not a target in `juce/`.** `juce/CMakeLists.txt` brings in JUCE for
  everything it builds; Nano Q is its own CMake project that pulls in `core/` directly.
- **Host parameters follow the JUCE layout** so the two products expose automation the same
  way, even though a DAW project cannot move between them (different plugin ids).
- **Elements, not a bespoke UI toolkit.** Elements brings a window host for each platform (Win32,
  Cocoa, X11), a Direct2D/Quartz/Cairo drawing layer, text input and scrolling.
- **Open: Tones.** Which HTTP stack per platform, or one library (cpr/libcurl, cpp-httplib with
  TLS), is the decision to make before porting that page.
- **Open: QPlug is new (Q 1.5).** Its API may change between minor releases; Q is pinned to a
  commit, and `clap_entry.cpp` leans on its `q_plug_entry_*` symbols.
