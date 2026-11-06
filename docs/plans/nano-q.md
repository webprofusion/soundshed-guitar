# Soundshed Nano Q: Soundshed Nano without JUCE

Status: **first working build, 2026-10-09. Windows verified; macOS builds and runs (2026-10-26, Intel,
Xcode 26.3); Linux not yet.** Branch
`nano-q`, in `nanoq/`. **Nano Q is now the native UI** (2026-10-27): Soundshed Guitar Nano, the
JUCE native UI it was ported from ([native-ui.md](native-ui.md)), was removed from the tree, so
work on a native UI goes here. The parity plan is [nano-q-parity.md](nano-q-parity.md).

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
| The 10,000 lines of Components in `juce/source/nativeui` (since removed) | `nanoq/src/ui/`, Elements elements drawn from the same theme tokens |

Nothing is reused from `juce/`. The two things the native UIs drew with live in core: the
generated colour tokens (`core/src/uiclient/ThemeTokens.generated.h`, a plain header made from
the web UI's CSS by `tools/gen-native-theme.mjs`) and the static Inter cuts
(`core/nativeui/fonts`, made by `tools/gen-nano-fonts.mjs`), so a theme looks the same in every
product.

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
- **No SVG files.** Artist parses SVG path data but not SVG documents. `ui/Graphics.*` reads the
  subset the icons in `core/ui/images/icons` use (path, line, circle, ellipse, rect, polyline,
  polygon, groups with transforms, `style` declarations) into Artist paths once per icon and
  draws them in one colour, as the web UI's icons take `currentColor`. A grey in a many-toned
  icon (the Inkscape guitar) draws that colour fainter. Artist's `canvas::transform(...)` *sets*
  the matrix on every backend rather than adding to it, so a shape's transform is composed onto
  `canvas::transform()` by hand. The same file loads the equipment artwork (PNG, JPEG) once per
  file.
- **No web view.** The Jam tab's YouTube player is a web page (Scales was too, until the Scale
  Guide was drawn natively on 2026-10-30: `ui/ScaleGuidePage.cpp`, docs/plans/scale-guide.md).
  `src/WebView.h` puts one in a native child window over the editor: WebView2 on Windows
  (`WebView.cpp`; the SDK is fetched from NuGet at configure, its loader linked statically), none
  elsewhere, where those pages open in the browser. The window is above everything Elements
  draws, so `ui/JamPage.cpp` places it over an empty `WebViewSlot` (its layout bounds times the
  zoom and the DPI) and hides it while a dialog or drop-down is open; links that leave the player
  go to the browser. To see its pages while testing, start the app with
  `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=<port>
  --autoplay-policy=no-user-gesture-required"` and capture them over CDP
  (`tools/agent-ui-debug/cdp-tool.mjs`): the driver's offscreen `shot` draws only the slot.
- **Tile layout traps.** A column is as wide as its narrowest child's *maximum* width, and a
  scroller reports its content's height as its maximum. `Row`, `Col`, `VScroll` and `HScroll`
  in `ui/Layout.h` correct for both.
- **No keyboard focus for drawn controls, and Tab that leaks.** Elements gives keys only along
  its focus chain, and its own Tab walks every child of a composite, shown or not, and runs off
  the end of a dialog into the page under it. Each control Nano Q draws derives from
  `Focusable` (`ui/Focus.h`) and wants focus only while the keyboard is in use, so a click still
  leaves none on a button and Space keeps its shortcut for a pointer user. `KeyRouter` (one per
  layer: the page, a dialog, a drop-down) does Tab and Shift+Tab itself over the controls it
  collects in layout order, round within its layer; the arrows move along a list, the chain or a
  row of tabs; it draws the focus ring (2 px, 2 px out, the accent) and, after a rebuild replaced
  the focused control, finds the new one by its role and name. Closing a dialog gives focus
  back to the control that opened it. Keys per control: Enter or Space presses a button or
  switch; a knob or slider turns with the arrows (Shift finely), Page Up and Down, Home and End,
  and Delete resets it; a chain card opens with Enter, bypasses with Space, goes with Delete,
  and Ctrl+Enter swaps it; the Menu key or Shift+F10 opens a control's context menu.
- **No number field in place.** A knob's value (and the tuner's reference pitch, the metronome's
  tempo) is typed where its value text is (`ui/InlineEdit.h`): a double click on the text,
  Enter, F2, or just typing a digit opens the field; Enter or leaving it commits, Esc cancels.
- **No accessibility.** Elements tells the platform nothing about what it draws.
  `ui/AccessBridge.*` is a UI Automation provider for the view's window on Windows: every
  control of the layer in front, with its role, name, value and state (`Focusable::Describe`),
  focus and value changes raised as events, and Invoke, Toggle, range values and SetFocus
  queued for the UI thread. Not done: macOS (NSAccessibility) and iOS (UIAccessibility), static
  text and headings (only controls are exposed), and the bars added over the page (the tone
  preview's, the Jam player's), which Tab does not reach.
- **No transitions.** Switches slide (0.2 s) by redrawing their own area each tick
  (`ui/SwitchMotion.*`); one rebuilt by its own tap slides from where the old one was. With
  Windows' "Show animations" off they jump.

## Building

```powershell
nanoq\build.cmd Release          # configures (first time: ~5 min, fetches Q, Elements, clap-wrapper, NAM, ...) and builds
nanoq\build.cmd Release nanoq_standalone   # one target
```

`build.cmd` configures only a new tree; after that Ninja re-runs CMake itself when a
CMakeLists or the source list changes (delete `build.ninja` to force a configure). The UI
sources that include `ui/Shell.h` share a precompiled header of Elements and `Shell.h`. With
both, an edit to one UI file rebuilds the standalone in ~10 s here (was ~57 s), and an edit to
`Shell.h` in ~40 s. Building every target also builds Q's `q_io`, which the plugins and the
standalone do not link; its libremidi fails here for want of a generated WinMIDI projection.

On macOS, with Xcode's command-line tools and Ninja:

```bash
cmake -S nanoq -B nanoq/build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build nanoq/build/Release
SOUNDSHED_NANOQ_PROFILE=/tmp/nanoq-profile "nanoq/build/Release/products/Soundshed Nano Q.app/Contents/MacOS/Soundshed Nano Q"
```

It needs no changes to build there: Elements takes its Cocoa host and Artist its Quartz 2D
backend, for the host's architecture. The products are `Soundshed Nano Q.app`, `.clap`,
`.component` (AUv2) and `.vst3`, with the engine data in each bundle's `Contents/Resources/ui`.
`SOUNDSHED_NANOQ_PROFILE` runs it on a throwaway data root instead of the real profile.

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
| `dev.ps1 -Profile <dir> [-Shot png] [-Wait n] [-NoRun] [-Target t]` | Stops a running Nano Q, builds the standalone (`-Target ""` for every target), reports only errors, runs the standalone on a throwaway data root and captures its window |
| `run-isolated.ps1` | The run-and-capture half |
| `window-shot.ps1 -Out png [-Resize WxH]` | DPI-aware `PrintWindow` capture, which works when the window is behind others |
| `click.ps1 -X -Y [-ToX -ToY] [-Wheel n] [-Right] [-Hover] [-Text s]` | Posts mouse and character messages straight to the view window, in capture coordinates; leaves the user's pointer alone |

**Driving it on any platform: `SOUNDSHED_NANOQ_DRIVE`.** macOS ignores mouse events posted to a
window that is not in front, so `click.ps1` has no Mac equivalent. With
`SOUNDSHED_NANOQ_DRIVE=<file>` set, Nano Q checks that file every tick; if it exists, its lines are
played into the Elements view and the file is deleted (`src/ui/DevDriver.*`). A script writes the
file, waits for it to disappear, then captures the window (`screencapture -l <window id>` on
macOS). Coordinates are the view's: logical pixels from its top-left, below the title bar.

```text
click 590 157          left click              double 395 170     double click
right 590 157          right click             move 380 165       pointer moves (hover)
drag 10 10 200 10      press, move, release    scroll 600 500 -40 wheel (negative: down)
shot C:\t\view.png     draws the view offscreen into a PNG: works while the window is hidden or the PC is locked
key ctrl+z             a key: tab, enter, escape, space, arrows, home, end, pageup, pagedown, menu, f1-f9, a-z, 0-9
type -6.5              characters, as typed into what has focus
```

**How much it draws: `SOUNDSHED_NANOQ_DRAWSTATS=<file>`** wraps the view in a probe
(`src/ui/DrawProbe.h`) that appends a line a second: paints, how much of the window they covered,
and the time spent drawing. With `SOUNDSHED_NANOQ_DRAWRECTS` set too, each paint's rectangle goes
to `<file>.rects`. On iOS, where Quartz draws on the CPU, the area is the cost; keep a widget that
changes with the feeds redrawn by its own rectangle (`Drawn`, `UiSession::Repaint`).

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
| Standalone app | Runs. Since 2026-10-27 its audio and MIDI devices are set in Settings (`ui/DevicePage.cpp`, the web UI's audioDevice.ts), answered by `StandaloneDevices` over clap-wrapper's standalone host (`src/standalone/ClapWrapperDevices.cpp`, built into the standalone target only, which registers itself; plugin builds link no backend and report no device settings). Driver, input and output device, sample rate, buffer size, MIDI inputs on and off, an input mute (kept in clap-wrapper's settings file as `soundshedInputMuted`), the input meter, a test tone, Reset Device and Rescan; every change saved to clap-wrapper's `standalone-settings.conf` at once. Not possible on clap-wrapper's host: channel choice (it opens a device's first channels), MIDI output (it drops the plugin's MIDI), a driver control panel, dropout counts, split input and output latency, a mute remembered per device pair, MIDI hot-plug, and rescanning ASIO while it streams. clap-wrapper's own window (Alt+Space) still works on the same settings |
| Compact frame, as the web UI's compact landscape layout (2026-10-26) | Done (`ui/ShellFrame.cpp`, `ui/ShellWidgets.h`, `ui/ShellIcons.cpp`): a 60 px rail with Chain, Effect, Setlist (the pads, since 2026-10-29), Tones and Jam and the Settings gear; the 40 px preset bar (previous, the name in its pill with the favourite heart, next, New, Edit, Save, Random, the preset menu, Controls); the 32 px footer (Setlists, Tuner, chain undo/redo, A/B, copy A to B, Options). Chain and Effect are stages that take turns, as at the web UI's compact size. The web UI's own icons, copied from its markup. Options is a sheet over the footer: the demo clip, play and repeat; setlists, tuner, input and output, mute; the metronome; the theme |
| Scenes | Done: chips over the chain (select, x to remove, right click to rename, + to add), and the chain's wrap switch beside them |
| Chain stage, drawn as the web UI's (2026-10-26) | Done: input and output as cards, each effect a 72 px card (86 with artwork, which sits over its top edge with the icon on it) with its icon in a round badge of its node class's colour (nodes.css, not the library colours), name in capitals, effect, NAM badge, signal light from the node's telemetry, the underglow, a category-coloured ring when selected, bypassed cards faded and missing files outlined in red; a power button on hover bypasses; a double click replaces; a 36 px cable with an arrowhead and a + dot that becomes the add button on hover inserts on that edge of the graph, so into any parallel branch too; parallel blocks between split and join rails; the chain centred in the stage and wrapped onto more lines with a cable back under each (or one line scrolling sideways with the wrap off); the round + at the bottom right adds before the output. Right-click menu adds Replace, Rename, Use the automatic name. No drag-to-reorder yet |
| Effect stage, drawn as the web UI's compact effect view (2026-10-26) | Done: a panel hanging from the preset bar, centred, at least 448 wide (the whole stage for a model or IR effect), over a darker stage; its header has the signal LED, the name with a pencil to rename, CATEGORY · EFFECT, the NAM badge, Presets and the ON switch; inset sections hold the model/IR row (label, previous, the loaded file on an accent field that opens the library, next, clear) with Main/Advanced underline tabs beside it, a splitter's Branches, and the parameters in groups (titled, an untitled one beside titled ones is Other), each a 34 px knob with its label over it and value under it (enums too, as the web UI's Mode knob), an on/off switch, or a chip that opens a list for an enum the engine marks `picker`; `visibleWhen` honoured. The effect chooser is the web UI's FX Library (since 2026-10-29): the chain's round + opens Add FX... and Add Scene; the library hangs over the editor (under the preset bar at the compact size) with its title, Close, the categories with counts, a search and the effects as cards in columns, with the composites (Supercharged Neural Amp) and a badge for a composite or an effect that needs a model; a card is clicked to add it, or dragged onto a card or cable in the chain to add it there |
| Model and IR slots | Done: Model (NAM), IR, and the IR Cabinet's IR A and optional IR B: the loaded resource, previous and next through the library, clear, and the field opens the resource chooser below. Since 2026-10-29 the chooser's Library rows read as the web UI's (category, creator, architecture, gear, tone type; uiclient `ResourceMetaLine`), a hovered or chosen row shows edit (name, category, tags), details, delete and select, and the filters have All / Favourites, the pack grouping and previous / next, with the hint in the footer. Esc and a tap outside the chooser cancel it, putting back the model it was previewing |
| Preset chooser: search, All/Favourites/Recents/Folders, favourite, delete, load with unsaved-changes confirm | Done. Since 2026-10-29 a popover under the preset selector, as the web UI's (`ui/PresetChooser.cpp`); the Presets page is gone |
| Setlists and performance pads | Done. Setlists are made, edited and stepped in the chooser's Setlists section. Since 2026-10-29 the pads are the web UI's (`ui/PadsView.cpp`, performancePads.ts): under the chain in place of the editor in the full layout, the Setlist page in the compact one, opened by the footer's grid button. A bank strip (the setlist before, this one with Edit, the one after), Add bank, and 4, 6 or 8 pads (`performancePads.padCount`, shared with the web UI); a pad plays its slot, its save button puts the preset playing there (asking before replacing one), Delete clears the pad playing. Not done: the experimental Effects pads (`effectsPerformancePads` feature) and the save button's press-and-hold, which Nano Q replaces with a tap and a confirmation |
| Controls (the global chain) | Done, as the web UI's Controls strip under the preset bar: input meter, IN with which inputs feed it, GATE (switch and threshold), TRANSPOSE, EQ (switch, and a button to its four bands), DOUBLER (switch and delay), OUT, output meter and mute. The limiter is in Settings |
| Tuner, metronome (tap tempo, meter, subdivision, click, volume) | Done. Since 2026-10-29 the metronome draws the web UI's card (`ui/Metronome.cpp`, `ui/MetronomeWidgets.h`): volume and pan knobs, meter and sound pickers, the tempo with its arrows (Shift for 0.1), wheel and double-click entry, round TAP and transport, the beat strip (accent dots, subdivision ticks, group gaps, the beat lit for 120 ms) and the running state; two rows of two at the compact size, as the web UI. The meter list has the web UI's two columns (simple; compound and odd) since 2026-10-29 |
| Settings: limiter, preset switch tails, NAM quality, oversampling, anti-alias, levels, devices, features, help | Done. Since 2026-10-29 laid out as the web UI's Settings (`ui/SettingsPage.cpp`): the sections General, Audio & MIDI, DSP Performance, Features and Help in the shared section list (chips at the compact size), each group in a card, choices as drop-downs and switches as toggle switches. The theme is the footer's, as in the web UI. The device level feed runs only while Audio & MIDI shows |
| Toasts for the engine's notifications | Done |
| Tones page (tone sharing), as the web UI's Tones tab (2026-10-26) | Done (`ui/TonesPage.cpp` over the shared `ToneService`): the account chip (email-code sign-in, sign out), Presets (the latest packs in a row that scrolls sideways over the community presets, search, tag filter, Load More), Packs (Previous/Next) and Installed (remove); cards with the pack's picture, chips, description, tags, creator and Install / Share, a pack also Open (its presets, each installable). Installing fetches the Tone3000 models a preset names that the library lacks, through Soundshed's relay, imports them, then installs (the engine refuses an archive whose models are missing). Since 2026-10-27 (`ui/TonePreview.cpp`, `uiclient/ToneServicePreview.cpp`): Preview on preset cards and in a pack's view, played by the engine from the download (`previewPresetArchive`, nothing written; Tone3000 models fetched by their ids), with a bar over every page while it plays (Install, Stop, which loads the preset from before); a pack's own view (picture, description, Share, Install pack, each preset with Preview and Install); a `soundshed://` link or share link on the standalone's command line opens the Tones page on that preset (Shared with you) or pack; a favourite or rating given to a preset installed from tone sharing is sent to the service when signed in (installs now record each preset's item, `presetItems`). Since 2026-10-29 the web UI's side list in the full layout (the account, Presets, Packs, Installed, My Content, + Create Pack, Cards / List, a help card; chips across the top at the compact size, as is Jam's), the List layout, My Content (the account's own presets and packs, read from `/items/me/list` and `/packs/me/list`), and Publish in the preset toolbar (the share button at the compact size), which with Create Pack says that publishing is Soundshed Guitar's. Not done: publishing, making packs, and editing or deleting one's own, which stay in Soundshed Guitar; the review queue; a link reaching a Nano Q already running, or on macOS (no single-instance hand-over in clap-wrapper, and the `soundshed://` scheme is registered to Soundshed Guitar); the service's favourite and rating counts, which the web UI hides too |
| Effect icons | Done: the shared SVGs, on chain cards, the effect page and the effect picker |
| Resizable window, and the full layout above the compact breakpoint (2026-10-27) | Done (`ui/WideFrame.cpp`): the window resizes from a 640 x 400 floor, and the layout follows the web UI's rule (compactMode.ts): compact at or below 980 wide or 620 high in layout pixels (the zoom divided out), unless `ui.density` (shared with Soundshed Guitar; Auto layout, Compact or Full in Options) says one always. Above it the full layout: PLAY / TONES / JAM tabs with the gear; the control bar with the input meter, In (and its input mode), Gate and Transpose, the preset toolbar (New, Edit, Save, Random, back and forward through the presets loaded, the preset menu) over the preset selector (previous, heart, name, User/Factory badge, next), EQ, Doubler, Out, the output meter and mute; the chain as tall as its lines (a third of the window at most, then it scrolls) with the editor under it, no wider than 900; the footer with chain undo/redo and A/B, the demo clip (play, repeat, render), Setlists, Tuner, mute, tempo with TAP, the metronome and its switch, zoom (70 to 150%, kept in the plugin state) and theme. The frame is swapped when the window crosses the breakpoint; each region yields its minimum size and is rebuilt for the new size the next tick, so a region built for the old size never holds the window. The compact footer now keeps Setlists, Tuner, the demo clip and play, chain history and A/B, tempo and the metronome beside Options (the demo clip goes below 760 px of stage). Since 2026-10-27 the editor shows the effect's artwork and the bar under the chain can be dragged. Not done: Publish, MIDI, riff recording and the looper in the footer; the standalone does not stop a window being dragged below the floor (clap-wrapper never asks; the view clips) |
| Jam tab (2026-10-27) | Done (`ui/JamPage.cpp`, `ui/PracticeToolPage.cpp`; uiclient `Jam`, `PracticeTool`): backing-track search and favourites, the YouTube player in a web view beside the results (playing on over other pages, with a bar to show or stop it), Scales in a web view, the Riff Library, and the Practice Tool. Scales is the Scale Guide, drawn natively since 2026-10-30 (`ui/ScaleGuidePage.cpp`, `ui/ScaleGuideDraw.cpp`; uiclient `ScaleGuide`) on every platform. Windows only for the web view; see [nano-q-parity.md](nano-q-parity.md), Progress 7+ |
| Touch long-press, phone layout | Not done |
| macOS build | **Builds and runs** (2026-10-26): all four products, the engine starts and loads the default preset, the window draws correctly, `NanoQEngineDialogTests` passes. Open and save dialogs are AppKit panels (`FileDialogs_mac.mm`) and open; before, macOS reported every dialog as cancelled. Apple silicon not yet built |
| Linux build | **Builds and runs** (2026-11-05): x64 and arm64 (`tools/linux-kiosk/build-nanoq.sh`, Debian trixie), the standalone on X11 with Elements on Cairo, full screen with `SOUNDSHED_KIOSK`. Run under Xvfb, and under cage in emulated arm64; no real X server, audio device or Pi yet. clap-wrapper takes `patches/clap-wrapper-x11-standalone.patch` (its loop spun a core on an unread timerfd, its window ignored resizes, RtAudio used four ALSA periods). HTTP is libcurl (`core/src/net/HttpCurl.cpp`), and file dialogs an in-app picker (`src/ui/FilePicker.cpp`), with files on a USB stick copied into the library. The kiosk types on an on-screen keyboard drawn by the app (`ui/OnScreenKeyboard.h`, follows `SOUNDSHED_KIOSK`). Not done on Linux: web views. See [tools/linux-kiosk/README.md](../../tools/linux-kiosk/README.md) |
| iOS / iPadOS | **Runs in the iPad Simulator** (2026-10-26): a UIKit host for Elements (`nanoq/ios/elements_host`), Artist's Quartz backend on Core Text (`nanoq/ios/patches`, applied at configure in iOS builds only) and an app that hosts the CLAP and plays it through RemoteIO (`nanoq/ios/app`). Built with `nanoq/ios/build.sh`. Device builds and what is left: [nano-q-ios.md](nano-q-ios.md) |
| Resource chooser, as the web UI's resource browser (2026-10-26) | Done (`ui/ResourceBrowser.cpp`): Select Amp Model / Pedal Model / IR Cabinet / Reverb IR, with Resource Library (search, category, File...), Folder (the folders the user added, shared with Soundshed Guitar through `resources.folderBrowser.*`; Add Folder..., a file heard where it is and added to the library in place on OK) and Tone 3000 (search by gear, sort and architecture through the relay, a tone opens to its A1 and A2 models, Select downloads, imports and loads one). A click is heard at once; Cancel puts back what the slot held. Not done: favourites, the creator filter, pack grouping, Tone3000 previews and Import All |
| Chain undo/redo and A/B (2026-10-26) | Done, as the web UI's (history.ts): `core/src/uiclient/ChainHistory` keeps two stacks of snapshots; an edit is recorded once the chain is still for 320 ms; a restore of the same chain with other settings goes back as node messages, anything else reloads the preset with the target chain (`UiCommands::LoadActivePresetWithGraph`), and the engine's echo within 1.5 s is not a new step. No Ctrl+Z yet |
| **Not yet at parity with the web UI's chain and effect view** (the full gap list and the phases to close it: [nano-q-parity.md](nano-q-parity.md)) | Since 2026-10-27 (parity phase 1): drag to reorder, favourites and recents in the chooser, the keyboard, channel mode, the artwork and the plugin-host and blend blocks are done. Still open: the NAM badge for a model whose library row lacks `architectureVersion` (the web UI reads the model file for it); the layout thumbnails some effects show on their cards; custom effect layouts (Layout Designer); the effect header's DSP status and layout switch; the EQ curve and spectrum, cabinet and Pickup Match response curves, IR alignment, Gate Learn (the noise floor and Learn), the spatial panner; per-input mixer levels as their own control; the blend knobs, composites, and hosting plugins without JUCE; the cables' live-signal animation; the Presets and Settings pages keep their older look (the web UI's preset chooser is a dialog with folders and tags) |
| CI | None yet; `nanoq/build.cmd` and the validator command above are what a workflow would run |

## Decisions and open questions

- **Separate directory, not a target in `juce/`.** `juce/CMakeLists.txt` brings in JUCE for
  everything it builds; Nano Q is its own CMake project that pulls in `core/` directly.
- **Host parameters follow the JUCE layout** so the two products expose automation the same
  way, even though a DAW project cannot move between them (different plugin ids).
- **Elements, not a bespoke UI toolkit.** Elements brings a window host for each platform (Win32,
  Cocoa, X11), a Direct2D/Quartz/Cairo drawing layer, text input and scrolling.
- **HTTP is shared (2026-10-26).** `core/src/net/Http.h` (built into `SoundshedUiClient`): a transport per platform (WinHTTP; NSURLSession on Apple platforms, picked when Objective-C++ is enabled before core is added; libcurl on Linux where the product sets `GUITARFX_UICLIENT_CURL`, as Nano Q does; elsewhere a transport that reports no network) behind `HttpTransport`, so a product can hand in its own, and `HttpWorkers` that cancel requests in flight when destroyed. `uiclient/ToneService` does tone sharing and Tone3000 with no UI framework in it, for Nano Q (and, until its removal on 2026-10-27, the JUCE Nano). Tone3000 always goes through Soundshed's relay (`api-guitar.soundshed.com/v1/resourcesearch`), whose own model URLs it rewrites (`tone3000::ThroughRelay`). The Apple transport has not been compiled yet.
- **Open: QPlug is new (Q 1.5).** Its API may change between minor releases; Q is pinned to a
  commit, and `clap_entry.cpp` leans on its `q_plug_entry_*` symbols.
