# Nano Q's UI in the JUCE app

Status: **experimental, every desktop build.** Spike 2026-10-28 (see Results). Since 2026-11-19
`SOUNDSHED_NANOQ_UI` builds the native UI *beside* the WebView editor, not in place of it:
Settings → User Interface → Interface → Native UI (Nano) (`ui.nativeEditor`) picks one, the
standalone swaps its editor in place, and a plugin's next window follows the setting
([user-interface.md](../user-interface.md#settings--features)); the native UI's footer has a
**Standard UI** button back. Since 2026-11-20 it is on by default for every desktop build
(Windows on any architecture, macOS, Linux), so the native UI is always there to switch to. The
JUCE iOS and Android builds stay on the WebView and are not shipped: mobile ships Nano Q's own
apps. The installer ships the fonts beside each binary, where Elements
looks for them. An existing build tree keeps the value it cached; pass `-DSOUNDSHED_NANOQ_UI=ON`
once to turn it on there (`build_macos.sh` and `build_linux.sh` pass it). Windows x64 and macOS
(universal) have been built and run; see "Other platforms".

## The question

Can one native UI serve both shells, Nano Q (QPlug) and Soundshed Guitar (JUCE), so the UI is
written once and neither plugin framework becomes a permanent dependency?

## The shape

```
core/                      engine, controller, uiclient            (no framework)
nanoq/src/ui/              the native UI, drawn with Elements      (no plugin framework)
   UiLink.h                what the UI needs from its host: a JSON mailbox, the asset root,
                           layout images, a per-pump hook
   UiRoot.*                session + shell on an Elements view (both shells use it)
   EmbeddedUi.*            the UI in a child window of a window someone else owns
shells
   nanoq/src/              QPlug: Engine is the UiLink; QPlug's presenter gives the view
   juce/source/NanoQUiEditor.*   JUCE: the processor's WebView bridge is the UiLink; the
                           editor's peer HWND is the parent window
```

The UI never depended on QPlug. Before the spike it depended on Nano Q's `Engine` for five calls
(`SendRequest`, `TakeMessagesForView`, `SetViewAttached`, the asset path and the standalone's
device pump) plus one reach into the controller (`ResolveLayoutImageFile`). Those are now
`UiLink`, and `Engine` implements it. The UI also uses four platform helpers from `nanoq/src`
(`Paths`, `Log`, `WebView`, `WindowChrome`), which name no framework and compile in either shell.

JUCE's side is a `SoundshedEditorBase` subclass. Requests go to
`PluginProcessorAdapter::handleWebMessage`, posted to the message thread so they are handled
after the UI event that sent them, as a WebView message is. What the controller sends the page
arrives on `setWebMessageCallback` and waits for the UI's next pump. The engine cannot tell this
editor from the WebView one. The base class still drives `OnIdle`, remembers the size, and drops
the standalone's launch curtain.

## How the window is shared

Elements on Windows is a Win32 child window that draws with Direct2D and runs entirely on the
thread's message loop (its timer is a `WM_TIMER`; `view.post` is drained from it). JUCE's message
loop dispatches those messages like any other window's, so nothing has to be pumped.

The editor makes the Elements view a child of its peer's HWND and keeps it over the editor's area
in physical pixels, using the sums `juce::HWNDComponent` uses. `HWNDComponent` itself cannot be
used: it destroys the window it is handed when it lets go, and the Elements view destroys its own.

### Other platforms

`nanoq::ui::EmbeddedUi` takes the peer's native handle on each desktop, as QPlug's plugin view
does (its `q_plug/src/<platform>/host_view`):

- **macOS** (`EmbeddedUiCocoa.mm`): Elements' Cocoa view is added to the peer's `NSView` and
  placed in points, counting from the top of a flipped parent and from the bottom of an unflipped
  one. Events come through the host's own loop. The fonts go in each bundle's
  `Contents/Resources`, which Elements searches and activates fonts from.
- **Linux** (`EmbeddedUi.cpp`): Elements' X11 host (`ELEMENTS_HOST_UI_LIBRARY=x11`, Cairo) makes
  a child of the peer's X11 window, found afterwards with `XQueryTree` because Elements keeps it
  to itself, and placed with `XMoveResizeWindow`; the view hears of a resize as `ConfigureNotify`.
  An X11 view reads a display connection of its own, which JUCE never looks at, so the editor
  registers it with `juce::LinuxEventLoop::registerFdCallback` and runs a 60 Hz timer for the
  views' timers (`NanoQUiEventPump`, one for all open editors). It needs x11, xcursor, xrandr,
  cairo, cairo-xlib, cairo-ft, fontconfig, harfbuzz and libwebp: `build_linux.sh` checks for them
  and both Linux workflows install them.
- **HTTP**: the UI's network (tone sharing, Tone3000) uses core's platform transport, which in
  this build is WinHTTP on Windows and nothing elsewhere (NSURLSession needs Objective-C++
  enabled, libcurl is opt-in). So the JUCE editor hands it one built on `juce::URL` on macOS and
  Linux (`UiLink::MakeHttpTransport`, `JuceHttpTransport`).

**macOS, 2026-11-26**: the universal (arm64 + x86_64) standalone builds and runs. Artist's CMake
accepted only one macOS architecture, so `patches/elements-artist-macos-universal.patch` lets it
take the list. Run on a throwaway profile with `--native-ui`: the native UI draws in the window
(checked with a `SOUNDSHED_NANOQ_DRIVE` offscreen shot), and its Standard UI button swaps to the
WebView editor without a crash. Not tried: the VST3/AU/CLAP in a DAW.

Not built or run yet: Linux and Windows ARM64/ARM64EC. Expect the first Linux CI run to need fixes.

## Build

```powershell
cmake -G "Visual Studio 18 2026" -A x64 -S juce -B juce/builds-nanoq-ui -DSOUNDSHED_NANOQ_UI=ON
cmake --build juce/builds-nanoq-ui --config Release --target SoundshedGuitar_Standalone
```

`juce/cmake/NanoQUi.cmake` fetches Elements at Nano Q's pin with Nano Q's patch, compiles
`nanoq/src/ui` and the four helpers into `SoundshedNanoQUi`, links it into the shared code, and
copies the fonts into `resources/` beside each binary, where Elements looks. Elements' own
CMake is added with `add_subdirectory`, never `include(ElementsConfigCommon)` at the top level,
which would call `project(elements)` and rename the JUCE project.

## Results

Windows 11, Release, standalone, 175% display scaling, run on a throwaway profile (`APPDATA`
pointed at an empty folder) and driven with Nano Q's dev driver (`SOUNDSHED_NANOQ_DRIVE`, which
is in `ui/` and so came along). The PC was locked, so whole-window captures were black and the
driver's offscreen `shot` was used instead.

| Check | Result |
|---|---|
| Builds first time with Nano Q's UI sources unchanged apart from the link | Yes |
| The UI starts on the JUCE engine and loads the default factory preset | Yes |
| The child window covers the editor's area | Yes: maximised, client 3840x2036, `ElementsView` 3842x2037 (rounded up) |
| A click round-trips (next preset) | Yes |
| Typing reaches a text box through the child window | Yes (the effect chooser's search) |
| Plugin Host is offered, since the JUCE engine registers it | Yes |
| A VST3 loads through Browse... (DelayChorus) and is listed | Yes |
| Open Plugin shows the plugin's own window | Yes |
| The window shrinks to 2100x1300 and the UI lays out again | Yes: client 2075x1235, child 2076x1236 |
| Close quits cleanly | Yes, no crash events |

Nano Q after the refactor: the CLAP builds and `NanoQEngineDialogTests` passes. Nano Q itself
was not run (its standalone was in use), so its presenter and the engine-as-link path are built,
not exercised.

Not tried: a DAW (VST3/CLAP inside a host), keyboard focus there, a second monitor at another
DPI, macOS, the Jam tab's web view (built without WebView2 here, so it opens the browser),
zoom (no `getZoom`/`setZoom` from JUCE yet, so the zoom control is hidden).

## What this means

- **The UI can be written once.** Only `UiLink` and `EmbeddedUi` know there is more than one
  shell, and the JUCE half is one small editor class.
- **JUCE becomes a shell, not the UI.** It is still needed for what only it does today: hosting
  third-party plugins, AUv3, Android. Hosting moved into core without JUCE would make the JUCE
  shell optional; nothing in the UI would change.
- **Android** keeps the web UI either way: Elements has no Android host.

## Problems found

- **Two Elements binaries in one process: not a problem (checked 2026-11-19).** This list once
  said Elements registers `ElementsView` against the executable's module, so a second binary
  with Elements in one DAW would fail `RegisterClassW`. The Elements Nano Q pins (v1.0.2)
  does not: `ElementsView` (`lib/host/windows/base_view.cpp`) and `ElementsWindow`
  (`window.cpp`) are registered against the module that holds the code, found with
  `GetModuleHandleExW(FROM_ADDRESS)`, and unregistered when it unloads. Windows keys a
  non-global class by name and module, so each binary gets its own class and window procedure.
  Checked with a throwaway host that loaded two copies of the Nano Q CLAP from two folders into
  one process, opened both editors side by side, then closed and unloaded one while resizing
  and sending mouse moves to the other. Each view's class belonged to its own module, the class
  window procedures differed, no "Could not register class" box appeared, and the survivor
  kept running. (Each view's window procedure reads as comctl32's: Windows subclasses it, and
  the subclass passes on to the class procedure.) Not tried: a real DAW, and Nano Q beside the
  JUCE build of this spike, which works the same way.
- **JUCE's `HWNDComponent` cannot hold another library's window**: it destroys the window when it
  lets go. The editor places the window itself.

## Next steps, if this goes ahead

1. ~~Elements: a window class per binary.~~ Already so in Elements v1.0.2 (see Problems found).
2. Run the JUCE build in DAWs (Reaper, Cubase, Studio One): focus, DPI, editor reopen, two
   instances.
3. ~~macOS: `EmbeddedUi` on an `NSView` parent.~~ Written 2026-11-20, with Linux (X11); not yet
   built (see "Other platforms").
4. Zoom and the Jam tab's WebView2 in the JUCE shell.
5. Decide the product shape: one app with a UI switch, or the JUCE build's editor replaced.
