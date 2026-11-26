# iOS and iPadOS Build

The iOS app is the JUCE **Standalone** target built for iOS, as the Android app
is ([android-build.md](android-build.md)). It runs the same `SoundshedGuitarCore`
DSP engine and the same web UI in a `WKWebView`. One universal app covers iPhone
and iPad. (Soundshed Nano Q, the native UI, has its own iOS build:
[plans/nano-q-ios.md](plans/nano-q-ios.md).)

**Not shipped.** iOS and iPadOS ship Soundshed Nano Q only, the native UI's own app. This JUCE
build is kept for development and comparison. It has no Native UI (Nano) switch: the desktop
builds' `SOUNDSHED_NANOQ_UI` is off on mobile.

There is no separate project to keep in step: `juce/CMakeLists.txt` builds for
iOS when configured with `-DCMAKE_SYSTEM_NAME=iOS` and the Xcode generator, and
`ios/build.sh` wraps that.

## Layout

```
ios/
  build.sh      Configures ios/build (Xcode), builds the app for the Simulator or a
                device, and optionally installs and launches it in the Simulator
  cmake/IosFilesystemBackDeploy.cmake
                std::filesystem for iOS 12 (legacy) builds
  compat/libcxx-filesystem/
                LLVM 20.1.8's libc++ <filesystem> sources and the prelude that uses them
  .gitignore    Ignores ios/build and ios/build-legacy
```

Everything else is shared with the desktop and Android builds:

| File | iOS-specific part |
| ---- | ----------------- |
| `juce/CMakeLists.txt` | `SSG_MOBILE`, Standalone-only formats, no plugin hosting, WASM off, the iOS minimums (15.0, and 16.2 for Soundshed Guitar), the Info.plist options, a flat-bundle resource copy of only the shipped UI folders |
| `juce/source/Main.cpp` | `SOUNDSHED_MOBILE` window handling; `setFullScreen (true)`; the 128-frame default buffer |
| `juce/source/LaunchCurtain.*` | Compiled out on mobile |
| `juce/source/PluginProcessorAdapter.cpp` | `locateAssetsRoot` also looks beside the executable |
| `core/src/dsp/RealtimeParallel.h` | Parallel DSP off, as on Android |
| `core/CMakeLists.txt` | stftPitchShift fetched for its headers only; its own project (with a command-line tool) does not configure for iOS |

## Prerequisites

| Component | Version used | Notes |
| --------- | ------------ | ----- |
| Xcode | 26.3 | iOS 26.2 SDK |
| iOS Simulator runtime | 26.3.1 | `xcodebuild -downloadPlatform iOS` (about 8 GB) |
| CMake | 4.2 | JUCE needs ≥ 3.25 |
| Node / npm | as for desktop | Builds `core/ui/dist` before the WebView app is packaged |

### Minimum iOS versions

| App | Minimum | Set by | Oldest devices |
| --- | ------- | ------ | -------------- |
| Soundshed Guitar | iOS / iPadOS **16.2** | WebKit: `color-mix()` | iPhone 8 and X; iPad 5th gen, iPad Air 3, iPad mini 5 |

The build tree's `CMAKE_OSX_DEPLOYMENT_TARGET` is 15.0, so the core and JUCE's
modules build for it. Soundshed Guitar's app target raises its own minimum
(`XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET`) to 16.2, and Xcode writes it into
the app's `MinimumOSVersion`. An app may link libraries built for an older iOS
than its own minimum.

**Soundshed Guitar: 16.2.** A `WKWebView` is the system's WebKit, so the iOS
version is the browser version. The UI uses `color-mix()` throughout (271 uses,
no fallback), which Safari got in 16.2. The rest is older or degrades:
`:has()` and `structuredClone` 15.4, container queries 16.0; `text-wrap:
balance` (17.5) and `<link rel="modulepreload">` (17.0) are ignored by older
WebKit, at the cost of a less even heading or a slower first load. The
TypeScript targets ES2019. The UI has no CSS nesting, whatever older comments
said. Going below 16 would mean replacing every `color-mix()` with a fixed
colour, to reach 15.4.

For iOS 12 devices (the iPad mini 2 and its generation) there is a separate
legacy build; see "iOS 12 (legacy) builds" below.

Raising the minimum is one line in `juce/CMakeLists.txt`. An existing build
tree keeps its cached `CMAKE_OSX_DEPLOYMENT_TARGET`, so after changing the
default there, reconfigure with `-U CMAKE_OSX_DEPLOYMENT_TARGET` (or delete
`ios/build`).

## Building

```bash
ios/build.sh                      # Simulator, Debug
ios/build.sh run                  # Simulator, then boot an iPad, install and launch
ios/build.sh release run          # Release build in the Simulator
```

Arguments may appear in any order: `sim|device`, `debug|release`,
`run`, `reconfigure`, `legacy`. The first run configures `ios/build`, which takes a few
minutes (it fetches the core's dependencies and builds JUCE's `juceaide`); later
runs only build. The app lands in
`ios/build/<Target>_artefacts/<Config>/Standalone/`. That folder is the same for
the Simulator and a device, so each build replaces the other's `.app` there.

After changing a CMake file, pass `reconfigure`. Xcode does re-run CMake on its
own, but only after it has loaded the old project for the current build, so that
build still runs the old build steps. A post-build copy that has moved goes on
writing to the old place, for instance.

`run` uses a booted Simulator if there is one, otherwise boots
`$SSG_IOS_SIMULATOR` (a UDID) or the first iPad the runtime lists, its largest
current model.

The Simulator runs the host's architecture, so on an Intel Mac the Simulator
build is x86_64 and on Apple silicon it is arm64; `build.sh` passes `uname -m`.

To work in Xcode instead, open `ios/build/SoundshedGuitar.xcodeproj` after the
first configure and pick the `SoundshedGuitar_Standalone` scheme.

**Use a release build for anything to do with audio**, as on Android: a debug
build compiles the DSP unoptimised.

### Signing for a device

The Simulator needs only an ad-hoc signature, so it builds with no Apple
account. A device build needs a development team:

```bash
SSG_IOS_DEVELOPMENT_TEAM=<team id> ios/build.sh device release reconfigure
```

The team id is written into the build tree, so later device builds need only
`ios/build.sh device release`. `-allowProvisioningUpdates` lets Xcode create the
development profile for `com.soundshed.guitar`.
Install with Xcode (Window > Devices and Simulators) or
`xcrun devicectl device install app --device <id> <path to .app>`.

Distribution (TestFlight, the App Store) needs an archive and a distribution
profile; see "Archiving for iOS" in JUCE's `docs/CMake API.md`. Nothing here
sets that up yet.

### iOS 12 (legacy) builds

For devices that stop at iOS 12: the iPad mini 2 and 3, the iPad Air (1st
gen), the iPhone 5s and 6. Their A7 and A8 chips are 64-bit ARMv8.0, and the
DSP uses no later instructions (no dot-product, FP16 or matrix intrinsics), so
the same code runs there.

```bash
ios/build.sh legacy                                          # Simulator, to check it builds and runs
SSG_IOS_DEVELOPMENT_TEAM=<team id> ios/build.sh legacy device release reconfigure
```

`legacy` uses a build tree of its own, `ios/build-legacy`, with
`CMAKE_OSX_DEPLOYMENT_TARGET=12.0` and `GUITARFX_IOS_WEBVIEW_DEPLOYMENT_TARGET=12.0`,
so the app's minimum is 12.0 and the normal tree keeps 15.0 and 16.2.

**It runs.** Soundshed Guitar's UI is drawn by iOS 12's WebKit
(Safari 12.1), which lacks several things the UI uses. `core/ui/ts/legacyWebKit.ts`,
the first module `main.ts` imports, supplies the ones that would stop it:
`matchMedia().addEventListener` (the first, which failed the bootstrap),
`ResizeObserver`, `structuredClone`, `replaceChildren`, `crypto.randomUUID`, and
Pointer Events, made from touch events, without which no knob, slider or drag
responds. Each is installed only where it is missing, so other WebViews are
untouched. On an iPad mini 2 the page boots, taps and drags arrive as pointer
events, and preset changes reach the engine. What it cannot supply is CSS:
there is no `color-mix()`, `:has()`, container queries or flexbox `gap`, so
colours and spacing are off in places.

Its compiled scripts are already Safari 12 syntax (TypeScript targets ES2019).
To check after a change, run each `core/ui/dist/*.js` through
`esbuild --target=safari12 --supported:template-literal=true` and compare it
with `--target=esnext`: any difference is syntax Safari 12 cannot parse. A
failure at startup shows on the splash screen and is written to the session
log as "UI bootstrap error"; `ios-deploy --download` (below) fetches it.

**`std::filesystem` is built into the app.** It is the one thing the code needs
that iOS 12 lacks: Apple's libc++ added it to the system library in iOS 13, and
the core, NAM and the JUCE adapter use it throughout. Below a 13.0 minimum,
`ios/cmake/IosFilesystemBackDeploy.cmake`:

- force-includes `ios/compat/libcxx-filesystem/FilesystemBackDeploy.h` into
  every C++ and Objective-C++ file. It lifts libc++'s "unavailable before iOS
  13" mark from `<filesystem>` alone. Every other mark stays, so anything else
  too new for iOS 12 still fails to compile instead of failing to load;
- builds LLVM 20.1.8's `libcxx/src/filesystem` (the release Xcode 26's libc++
  headers come from) as `guitarfx_libcxx_filesystem`, and links it into every
  target, so the symbols resolve inside the binary.

Precompiled headers are off in a legacy build: clang uses one only when its
`-include` comes first, and Xcode puts the core's after the prelude's.

The build warns that NAM's oversampler calls `os_workgroup_leave` (iOS 14)
without a guard. It is harmless: that call is reached only after
`os_workgroup_join` succeeded inside an `iOS 14.0` availability check.

The C++20 synchronisation library (`std::atomic::wait`, `<semaphore>`,
`<barrier>`) needs iOS 14 and is not used. If it ever is, a legacy build stops
with an "unavailable" error rather than shipping something that cannot load.

**Installing.** Xcode 26 builds and signs for iOS 12 but will not use an iOS 12
device as a destination ("Could not locate Developer Disk Image"), and that has
three consequences, all met putting the (since removed) JUCE Nano on an iPad mini 2:

- **Register the device by hand.** `-allowProvisioningDeviceRegistration` only
  registers a device Xcode accepts as a destination, so add the UDID at
  developer.apple.com (Certificates, IDs & Profiles > Devices). `ios-deploy -c`
  prints it.
- **Then make Xcode drop its cached profile.** The next build may reuse the team
  profile it made before the device was registered (it lives in
  `~/Library/Developer/Xcode/UserData/Provisioning Profiles/`), and the install
  then fails with "A valid provisioning profile for this executable was not
  found". Move that file aside and rebuild; Xcode fetches one that includes the
  device. `security cms -D -i "<app>/embedded.mobileprovision" | grep <UDID>`
  shows whether a build has it.
- **Install with `ios-deploy`, and launch on the device.**
  `ios-deploy --id <UDID> --bundle <path to .app> --nostart` installs
  (`xcrun devicectl` handles iOS 17 and later only). Launching from the Mac
  needs the developer disk image, so open the app on the device. To read its
  logs afterwards, `ios-deploy --id <UDID> --bundle_id <bundle id>
  --download="/Library/Soundshed Guitar" --to <dir>` copies the profile,
  `logs/soundshed-startup.log` and `data/v1/logs/session-log.txt` included.

**Performance.** An A7 is several times slower than anything the normal build
targets. A legacy build defaults to a 256-frame buffer (see "Buffer size"
under Audio); at 64 an iPad mini 2 glitched. Heavier presets may still
want lighter NAM models (nano or feather). Judge it only in a release build on
the device itself.

## How it differs from the desktop builds

### Formats

Only the Standalone is built (`FORMATS` is `Standalone` whenever `SSG_MOBILE` is
on). iOS could take an **AUv3**, which would let the engine run
inside AUM, GarageBand, Logic for iPad and the like. That is a separate piece of
work: an app extension needs its own bundle id, an app group if it is to share
the profile with the app, and an editor that copes with the host's view sizes.

Plugin hosting is off, as on Android. iOS can host AUv3s, but the app has never
been built or tested against that, so the format manager registers nothing.

### Features that are off

| Feature | Why |
| ------- | --- |
| WASM effect host | Wasmtime publishes no iOS C API bundle, and iOS allows no JIT |
| Hosting third-party plugins | See above |
| CLAP / VST3 / AU / AAX | Desktop formats |
| Launch curtain | One full-screen window; the system's launch screen covers start-up |
| Single-instance lock | iOS runs one instance per app anyway |

### The UI ships inside the app bundle

An iOS bundle is flat: the executable sits at the bundle's root, next to its
resources, rather than in `Contents/MacOS`. The post-build step copies the UI to
`<App>.app/ui`, and `PluginProcessorAdapter::locateAssetsRoot` adds the
executable's own directory as a candidate on iOS. The WebView editor takes the
adapter's root before its own guesses, so that one lookup covers both.

It is not `<App>.app/resources/ui`, which every existing lookup would have found
unchanged: a directory called `resources` at the root of an iOS bundle makes
`codesign` take the bundle for a macOS one, and it fails with "code object is
not signed at all", naming whichever file it met first (an app icon, here).

It copies only the folders that ship (`GUITARFX_IOS_UI_DIRS`, the same list as
Android's `stageUiAssets`) plus `index.html` and the generated `build-flags.js`.
The desktop copy takes the whole of `core/ui`, which includes `node_modules`, the
TypeScript sources and the tests: tolerable on disk, but an app bundle carries
every file to the device and signs it.

Unlike Android, nothing has to be unpacked: the bundle is an ordinary read-only
directory, so the engine's factory presets, metronome clicks and layout images
resolve as files in place.

None of Android's WebView workarounds apply. `WKWebView` runs JUCE's integration
script natively (`WKUserScript`), takes a charset in the MIME type, has DOM
storage on, and sizes itself from its frame.

### Paths and the profile

`userApplicationDataDirectory` is the app container's `Library`, and the core's
`HOME/Library` resolves to the same place, so the profile is
`<container>/Library/Soundshed Guitar` and no override is needed.

Each iOS app has its own container, so **Soundshed Guitar and Nano Q do not
share a profile on iOS** the way they do on desktop. Sharing one would need an app
group (`APP_GROUPS_ENABLED`, `APP_GROUP_IDS`) and the profile moved into the
group container.

### Window, orientation and safe area

`MainWindow` calls `setFullScreen (true)` on iOS, the opposite of Android, where
that collapses the window. JUCE's iOS peer then takes the display's user area,
the screen less its safe-area insets, and takes it again on every display change,
so rotation and the iPhone's camera housing are handled by JUCE rather than by
CSS.

| | Soundshed Guitar |
| - | ---------------- |
| Orientations | Landscape only, as on Android |
| iPad multitasking | Off (`REQUIRES_FULL_SCREEN`) |
| Status bar | Hidden |

The status bar is hidden by putting the window in JUCE's kiosk mode, which on
iOS means "full screen, no status bar". `STATUS_BAR_HIDDEN` alone does nothing
at runtime: JUCE turns on view-controller-based status bar appearance, which
makes iOS ignore the plist's `UIStatusBarHidden`, and JUCE's view controller
hides the bar only for the kiosk component. Without it the iPad's clock and
battery sit on top of the header's title and settings button. The plist key
still keeps the bar off the launch screen.

An iPad app that allows Split View must allow every orientation, so the WebView
app's landscape lock and its opting out of multitasking go together.

### Deep links

**Tone-sharing links (`soundshed://`) open the iOS app but do not reach it
yet.**

- The scheme is registered. `juce/CMakeLists.txt` adds `CFBundleURLTypes` to
  the Standalone's generated Info.plist after `juce_add_plugin`, on macOS and
  iOS alike. JUCE's `PLIST_TO_MERGE` would have put it in every format's
  plist, so it isn't used. (It used to be passed as
  `XCODE_EXTRA_PLIST_ENTRIES`, which `juce_add_plugin` silently ignores, so no
  build had the scheme.)
- What's still missing is routing. On macOS JUCE's app delegate takes the
  `kAEGetURL` Apple event and hands the URL to `anotherInstanceStarted` in
  `juce/source/Main.cpp`. On iOS, JUCE runs its app through a scene delegate
  (`JuceAppSceneDelegate` in `juce_Windowing_ios.mm`). It implements neither
  `scene:openURLContexts:` (link opened while the app runs) nor reads
  `connectionOptions.URLContexts` in `scene:willConnectToSession:options:`
  (link that launches the app), and the app delegate has no
  `application:openURL:options:`. So a link brings the app to the front and
  the URL is dropped. A fix needs a JUCE patch, or a category or
  `class_addMethod` on `JuceAppSceneDelegate` that forwards the URL to
  `anotherInstanceStarted`.

## Audio

JUCE's iOS device opens an `AVAudioSession` in `PlayAndRecord` with
mix-with-others, speaker default, AirPlay and Bluetooth A2DP allowed. The
microphone prompt appears when the input is first opened; the text is the
`MICROPHONE_PERMISSION_TEXT` in the CMake files.

### Buffer size

JUCE asks iOS for 256 frames (512 in the Simulator). `createPluginHolder` asks
for **128** instead, as the preferred setup for any open without a saved setup;
a size chosen in Settings > Audio Device wins from then on, as on Android. 64 is
worth trying on a recent device.

An iOS 12 (legacy) build asks for **256** (5.3 ms at 48 kHz). On an iPad mini 2
a 64-frame buffer (1.33 ms) glitched while the load readout was well short of
full: the readout is an average, and one block over its deadline is a dropout,
which the A7's two cores leave little room to absorb. 256 played much better;
128 (2.7 ms) is untested there.

The audio thread's priority is not a lever. JUCE renders inside Core Audio's
RemoteIO callback, on the system's real-time I/O thread, and nothing in the app
waits on another thread from it: parallel DSP is off on iOS, and NAM's
oversampler, which can split work over threads and spin waiting for them, is
never asked to (the core leaves its factor at 1).

### Background audio

Both apps declare the `audio` background mode, so the amp keeps running when
another app (a backing track, a lesson video) comes to the front. Without it iOS
suspends the app and the guitar goes silent the moment it leaves the screen.

### Parallel DSP is off

`kParallelDspSupported` is false on iOS for Android's reason: the render
callback is a time-constraint thread and the helper threads are not. Doing this
properly means the helpers joining the device's audio workgroup
(`os_workgroup`), which JUCE exposes through `AudioWorkgroup`; until then
everything runs on the callback thread.

### Inputs

As on Android, the built-in microphone is no guitar input; a USB or Lightning
audio interface is. iOS routes audio to the most recently connected interface
and JUCE follows route changes, so a hot-plugged interface works without a
restart, unlike on Android.

The Simulator routes the Mac's default input and output and has no low-latency
path: use it to check that the app boots and the UI works, not for tone or
latency.
