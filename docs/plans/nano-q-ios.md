# Soundshed Nano Q on iOS and iPadOS

Status: **runs in the iPad Simulator, 2026-10-26** (Xcode 26.3, iOS 26.3 Simulator, Intel Mac):
the UI draws, takes touch, the engine loads its default preset from the bundle, and audio runs
through it. **The iOS 12 build runs on an iPad mini 2.** See Progress. Follows the macOS build of Nano Q
([nano-q.md](nano-q.md)).

Nano Q runs on Windows and macOS. Its three libraries stop at the desktop: Elements has
GTK, X11, Wayland, Cocoa, Win32 and headless hosts; Artist's Quartz 2D backend reaches
AppKit for fonts and one colour; QPlug's standalone app is clap-wrapper's, which plays
through RtAudio, and RtAudio has no iOS backend. This plan ports each of them, then
builds the app the way `ios/build.sh` builds the JUCE ones ([ios-build.md](../ios-build.md)).

## What the port needs

| Layer | Desktop | iOS |
|---|---|---|
| Window host | Elements' Cocoa host, `lib/host/macos` (an `NSView`) | A UIKit host, `nanoq/ios/elements_host/` (a `UIView`): touches become Elements' mouse events, a display link polls, the pasteboard is the clipboard, a hardware keyboard types |
| Drawing | Artist's Quartz 2D backend | On a device, Artist's Skia backend on the GPU (Ganesh on Metal), with a Skia built for iOS (see "Drawing on the GPU"). Quartz 2D remains for the Simulator and as a fallback: its AppKit calls (`NSFont`, `NSFontManager`, `NSColor`, `<Quartz/Quartz.h>`) become Core Text and Core Graphics calls, which iOS has too |
| QPlug's view glue | `q_plug/src/macos/host_view.mm` (`NSView`) | `nanoq/ios/q_plug_host_view.mm` (`UIView`) |
| App and audio | clap-wrapper's standalone, RtAudio and RtMidi | `nanoq/ios/app/`: a UIKit app that hosts the CLAP plugin QPlug builds, linked in statically, and plays it through `AVAudioSession` (PlayAndRecord) and a RemoteIO audio unit |
| Formats | CLAP, VST3, AUv2, standalone | The app only. An AUv3 is a later, separate piece of work, as for the JUCE apps |
| Build | Ninja, `make_clapfirst_plugins` | Xcode generator, `-DCMAKE_SYSTEM_NAME=iOS`, `nanoq/ios/build.sh` |

### The app hosts the CLAP, as clap-wrapper's standalone does

The iOS app is a minimal CLAP host around Nano Q's own plugin: the `clap_entry` that
`nanoq/src/clap_entry.cpp` defines is linked into the app, and the app calls it the way a
host calls a loaded module. That keeps everything Nano Q already is (QPlug's
`base_plugin`, the controller, the processor, the presenter, the parameter and state
handling, the engine thread) unchanged, and the app is only what a host is: it creates the
plugin, activates it at the session's rate and buffer size, calls `process` from the
RemoteIO render callback, runs `on_main_thread` when asked, and hands the CLAP GUI
extension a `UIView` as its "cocoa" parent (`CLAP_WINDOW_API_COCOA` is what QPlug asks for
on any Apple platform, and the handle is only a pointer).

The other choice was to call `make_controller`/`make_processor`/`make_presenter` directly
and skip CLAP. That would bypass `base_plugin`'s event slicing, parameter flushing and GUI
sizing, which would then have to be rewritten for one platform.

clap-wrapper (at Q's pin) does have an iOS path, but it is a different product: an AUv3 app
extension wrapping the CLAP, inside a host app that loads it (`target_add_auv3_wrapper`,
`src/detail/standalone/ios/auv3`). That is the natural route for the AUv3 follow-up, since
it would give Nano Q in AUM and GarageBand as well; it needs an app extension target, an app
group for a shared profile and an editor that copes with a host's view sizes. The standalone
here needs none of that, and keeps clap-wrapper out of the iOS build.

### Audio

- `AVAudioSession`: PlayAndRecord, mix with others, default to speaker, Bluetooth A2DP
  allowed (JUCE's iOS device uses the same set); a preferred buffer of 128 frames (256 in
  a legacy build), as the JUCE apps ask for.
- A RemoteIO unit, input enabled on bus 1, Float32 non-interleaved stereo on both sides.
  The render callback pulls the input with `AudioUnitRender` and runs the plugin's
  `process` in place of the output, on Core Audio's real-time thread.
- The plugin is activated for the session's buffer rounded up to a power of two (128 for
  a 2.7 ms buffer), not for the 4096 frames iOS may hand over with the screen locked: a
  longer callback is processed in blocks of that size. The engine is prepared for the
  block it usually gets, as the JUCE apps' is. (The Simulator calls back with about 490
  frames whatever is asked for, so it exercises the splitting.)
- Three seconds after the unit starts, the app logs how many render callbacks came, whether
  the plugin is active and the input and output peaks. A unit can start without error and
  still never call back. The app's lines (`Nano Q audio: ...`, interruptions, state saves) go
  to the console and to `nanoq.log` in the profile's `logs` folder, since a current Xcode
  cannot show an iOS 12 device's console: `ios-deploy --id <UDID> --bundle_id
  com.soundshed.guitar.nanoq --download=/Library --to <dir>` fetches it from
  `Library/Soundshed Guitar/logs/`.
- A route or rate change (an interface plugged in, a call) stops the unit, deactivates the
  plugin and reactivates it at the new rate.
- Microphone permission is asked for before the unit starts; without it the input is
  silence and the app still runs.
- The `audio` background mode, so the amp keeps playing behind another app.

### UI

- One full-screen view controller whose view fills the safe area. The Nano Q layout is
  the desktop layout; an iPad's screen (1024 by 768 points and up) is bigger than its
  960 by 600 default, so it fits as it is. The compact phone layout is not done
  ([nano-q.md](nano-q.md)), so the app is iPad-first; on an iPhone it may not fit.
- Touch: the first touch is the left mouse button (`click` down, `drag`, `click` up,
  with the tap count from `UITouch.tapCount`); the cursor is moved to it first, as a
  hover would. A long press (0.5 s without moving) is a right click, which is how Nano Q's
  card and preset menus open. Two-finger pans are scroll-wheel events, so the scrollers
  move.
- Hardware keyboard (`pressesBegan`) for key events and `UIKeyInput` for text. The
  on-screen keyboard is not offered yet (Elements has no "text entry focused" signal to
  show it with); the preset search field needs it later.
- File dialogs report cancelled on iOS for now; `UIDocumentPickerViewController` is the
  follow-up.
- Drawing (2026-10-27). Quartz draws on the CPU, so on iOS the area drawn is what costs, and a
  draw holds the main thread the touches wait on. UIKit merges every area invalidated in a frame
  into one rectangle; the host keeps them apart (`invalidate_rect`) and `drawRect:` draws each
  clipped to itself, leaving what lies between as the backing store holds it
  (`clearsContextBeforeDrawing = NO`); the whole view, more than eight areas, a resize or a draw
  UIKit asks for itself draws once. Nano Q redraws a changed widget by the rectangle it last drew
  into (`ui/Drawn.h`, `UiSession::Repaint`), never by Elements' search, which redrew the whole
  window past four in one poll and so on every telemetry tick while audio played; and
  `patches/elements-input-redraws.patch` stops a click redrawing the whole window and a press or
  move over a scroller's content redrawing all of it. Measure with `SOUNDSHED_NANOQ_DRAWSTATS`
  (`ui/DrawProbe.h`): paints a second and how much of the window they covered.

## Carrying the changes to Cycfi's libraries

**Decision: patch files applied at configure, iOS builds only; new code in this repo.**

- New files (the UIKit host, QPlug's iOS view glue, the app) live in `nanoq/ios/` and are
  compiled by Nano Q's CMake. Elements is not patched for its host: QPlug compiles the
  host layer into each plugin from a global property (`Q_PLUG_ELEMENTS_HOST_SOURCES`),
  and an iOS build points that property at `nanoq/ios/elements_host/` before
  `q_plug_add_host`.
- What has to change in fetched files is in `nanoq/ios/patches/`, one patch per
  repository, applied by FetchContent's `PATCH_COMMAND` through
  `nanoq/ios/cmake/ApplyPatch.cmake`, which skips a patch that is already applied:
  - `artist.patch` (Elements' `lib/artist` submodule): the Quartz 2D backend on Core Text,
    with `TARGET_OS_IPHONE` guards so the macOS code is untouched;
  - `q.patch` (Q's `q_plug/CMakeLists.txt`): on iOS, no clap-wrapper (no plugin formats
    to make), no AppKit, no `src/macos/host_view.mm`.
- Nano Q declares `cycfi_elements` itself, at the commit Q pins, so its declaration (with
  the patch) is the one FetchContent uses. **When Q's pin moves, this one must move with
  it**; `nanoq/CMakeLists.txt` says so beside it.
- A desktop build applies no patch, so its fetched sources are byte for byte what Cycfi
  publishes and the macOS and Windows builds cannot be changed by this work.

Why not forks: a fork is a repository to keep in step with Cycfi's for three libraries,
for a few hundred lines of change. Patches are reviewable in this repo, and each is small
enough to offer upstream as it is (a UIKit host for Elements would be a pull request of
its own).

## Steps

1. Artist on Core Text (`artist.patch`). Builds for macOS unchanged and for iOS.
2. Elements' UIKit host and QPlug's view glue. Nano Q's `nanoq_impl` builds for iOS.
3. The app: delegate and scene, the view controller, the CLAP host, the audio session and
   RemoteIO unit, Info.plist (microphone text, background audio, orientations, launch
   screen).
4. CMake and `nanoq/ios/build.sh`: an Xcode build for the Simulator, run on an iPad
   simulator: the UI draws, takes touches, and the engine plays the Mac's input.
5. A device build (`SSG_IOS_DEVELOPMENT_TEAM`), and the iOS 12 legacy build for the iPad
   mini 2 (`ios/cmake/IosFilesystemBackDeploy.cmake`; UIScene is iOS 13, so the app
   falls back to an app-delegate window there).
6. Regression: Nano Q's macOS build and `NanoQEngineDialogTests`.

## Building and running

```bash
nanoq/ios/build.sh run                       # Simulator, Debug: build, boot an iPad, install, launch
SOUNDSHED_NANOQ_PROFILE=/tmp/nanoq-ios nanoq/ios/build.sh run   # on a throwaway data root
nanoq/ios/build-skia.sh                      # once: Skia for the device builds (about 20 min)
SSG_IOS_DEVELOPMENT_TEAM=<team id> nanoq/ios/build.sh device release reconfigure
SSG_IOS_DEVELOPMENT_TEAM=<team id> nanoq/ios/build.sh legacy device release reconfigure   # iOS 12
```

A device build draws with Skia when `build-skia.sh` has built it, and with Quartz otherwise;
`skia` or `quartz` chooses (at configure, so with `reconfigure` to switch a tree). A
Simulator build draws with Quartz: the Skia is arm64 only, and the Simulator on an Intel Mac
is x86_64.

The tree is `nanoq/ios/build` (`build-legacy` for iOS 12), the app
`<tree>/<Config>-<sdk>/Soundshed Nano Q.app`. Installing on a device, and the iOS 12
caveats (registering the device by hand, `ios-deploy`, launching on the device), are as in
[ios-build.md](../ios-build.md).

`SOUNDSHED_NANOQ_PROFILE` reaches the app through simctl's `SIMCTL_CHILD_` prefix; the
Simulator's app can write to a host path, so the profile and logs (`logs/nanoq.log`,
`data/v1/logs/session-log.txt`) can be read from the Mac directly.

### Drawing on the GPU

Quartz draws on the CPU, and on an A7 that was too slow: selecting a chain node redraws
nearly the whole window, and on the iPad mini 2 that took 1.2 to 2.5 s, about two thirds of
it in the blurred shadows (the effect panel's and each card's), which Quartz blurs over the
whole of each shape. Touches were not the problem: they reached the view in 4 to 15 ms.

So a device build draws with Artist's Skia backend, as Elements' Cocoa host can:

- **Skia for iOS.** Artist publishes prebuilt Skia for macOS, Windows and Linux only.
  `nanoq/ios/build-skia.sh` builds Skia m148 with Artist's own vcpkg port and the vcpkg
  Artist pins, for the triplet `nanoq/ios/cmake/vcpkg-triplets/arm64-ios-nanoq.cmake`:
  static, release, arm64, iOS 12.0, Ganesh on Metal, harfbuzz, png, jpeg and webp. It all
  goes under `nanoq/ios/skia` (ignored by git). The port gives vcpkg's flags to Skia's C
  and C++ only, which left the Metal code (Objective-C++) built for the SDK's iOS, its
  `@available` checks folded away; the script adds gn's `ios_min_target` to the port, and
  checks every object in `libskia.a` is for 12.0. Skia falls back to Metal's minimum
  family when it cannot tell an iOS 12 GPU's, which is what an A7 is.
- **CMake.** `NANOQ_IOS_SKIA` (the installed tree; `build.sh` passes it) turns on
  `ARTIST_SKIA`, off the prebuilt bundle, and links Metal.
- **The host.** `nanoq/ios/elements_host/base_view.mm`: the view's layer is a
  `CAMetalLayer`. The areas invalidated are drawn, each apart and clipped, into a GPU
  surface that keeps the window, once a frame from the display link; the surface is then
  copied to the layer's drawable and presented. No GPU work is sent in the background.

**Measured on the iPad mini 2** (2026-10-30, legacy release build): selecting a chain node
went from 1,230-2,520 ms to 160-260 ms (about 85-180 ms recording the draw on the CPU,
75-100 ms on the GPU); small redraws (a knob, a light) 7 to 45 ms. The UI looks as it did
with Quartz. The first draws after launch still take about a second.

**Orientation.** The app is landscape only. In a Simulator held in portrait, iPadOS 26
shows the landscape window scaled down and letterboxed rather than rotating it, so a
screenshot taken that way shows a band across the middle: rotate the Simulator
(Device > Rotate Left).

## Progress

| Step | State |
|---|---|
| Plan | This document |
| Artist on Core Text | Done (`elements.patch`); also builds for macOS |
| Elements UIKit host, QPlug view glue | Done |
| App: CLAP host, audio, delegates, Info.plist, launch screen | Done |
| CMake, `build.sh` | Done. The patches apply at configure, and a reconfigure finds them applied |
| Simulator | **Runs** (iPad Air 11-inch M3, iOS 26.3): the UI draws with Inter, the engine starts from the bundle's data and restores its session, the audio unit calls back at 48 kHz and the plugin processes (output from a silent input: the Simulator had no microphone from the Mac) |
| Touch | **Works** in the Simulator: a tap selects a chain card and switches pages, a drag turns a knob (and the preset shows unsaved changes), a long press opens a card's menu without selecting the card, a tap on the backdrop closes it, two fingers scroll the preset list in the natural direction, and the transport's Play runs the demo riff through the rig (meters and DSP load move). Two taps at different places 33 ms apart (the test tool firing two calls at once) lost the second; not seen with taps at a human pace |
| Device (iOS 15+) | Not built separately: the legacy build below is the same arm64 code, compiled, linked and signed for a device |
| Legacy (iOS 12) device | **Runs on an iPad mini 2** (iOS 12.5.8): the engine starts from the bundle on the NEON NAM kernel and opens the view; presets load by touch; the session gives 48 kHz and a 5.3 ms (256-frame) buffer with input, the unit calls back steadily (617 callbacks in the first 3 s) with the plugin active; the plugin's state is saved on going to the background (the iOS 12 app-delegate path) and the last preset is restored on the next launch. No errors in its logs. How it sounds and whether it glitches under load is for the ear: the logs record neither. **Builds and signs** (`build.sh legacy device release`, team as for the JUCE apps): arm64, `MinimumOSVersion` 12.0, the profile includes the iPad mini 2. std::filesystem is in the binary (no `__fs` imports), and UIScene, UniformTypeIdentifiers and `os_workgroup` are weak imports. Not yet installed or run on the device |
| Drawing cost (2026-10-27) | Measured on Windows with `SOUNDSHED_NANOQ_DRAWSTATS`: playing, 22.5 windows of area a second (every paint the whole window) down to about 3.5, and to the meters and lights alone with the iOS host's separate areas; a knob drag no longer redraws the whole window on press and release or the editor on each move. On the iPad mini 2 (2026-10-29) the iOS host's separate areas left the window black but for the meters: Core Animation copies back only what lies outside the merged dirty rectangle, so the gaps between areas were never kept. The host now draws the areas into a bitmap of its own that keeps the window and copies the dirty rectangle from it. **Checked on the device**: the window stays drawn while playing and turning knobs, and the meters, lights and knobs update cleanly. Quartz was still too slow on an A7 (1.2-2.5 s to redraw after selecting a node); a device build now draws with Skia on Metal, 160-260 ms (see "Drawing on the GPU") |
| macOS regression | **Passes**: `nanoq/build/Release` configures and builds all four products, `NanoQEngineDialogTests` passes, and the standalone opens its view and loads its preset. A desktop build applies no patch |

## Not done yet

- Audio on a device: the iPad mini 2 runs the app; judge the sound and the DSP load there,
  in the release build, and with heavier presets (docs/ios-build.md, "Performance").
- The on-screen keyboard: the preset search field takes a hardware keyboard only.
- Saving to a file and choosing a folder (the open dialog is a document picker).
- An app icon (no asset catalog yet), and the strips outside the safe area follow the dark
  theme only.
- MIDI (CoreMIDI into the plugin's note port) and the audio device settings clap-wrapper's
  desktop standalone has (on iOS the session picks the route).
- AUv3, through clap-wrapper's AUv3 wrapper (see "The app hosts the CLAP").

