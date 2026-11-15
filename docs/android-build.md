# Android Build

The Android app is the JUCE **Standalone** target packaged as an APK. It runs
the same `SoundshedGuitarCore` DSP engine and the same web UI as the desktop
builds — only the shell around them differs.

## Layout

```
android/
  CMakeLists.txt          Native entry point Gradle points at; a shim over juce/
  cmake/HostToolchain.cmake  Finds MSVC so JUCE can build juceaide (Windows hosts)
  settings.gradle.kts     Gradle project definition
  build.gradle.kts        Root build; pins the Android Gradle Plugin
  gradle.properties       Build-wide flags, including which ABIs to build
  deploy.bat              Builds (on request), installs and launches the APK on a connected phone
  local.properties        Machine-local SDK path (not checked in)
  app/
    build.gradle.kts      App module: NDK/CMake wiring, JUCE Java sources, UI assets
    src/main/AndroidManifest.xml
    src/main/java/com/soundshed/guitar/
      SoundshedApp.java   Unpacks the web UI from APK assets on first run
      MainActivity.java   Plain Activity; asks for microphone access and sustained performance
```

## Prerequisites

| Component        | Version used            | Notes                                            |
| ---------------- | ----------------------- | ------------------------------------------------ |
| Android SDK      | platforms 35 and 36     | `compileSdk`/`targetSdk` are 36, `minSdk` is 29   |
| Android NDK      | `27.3.13750724`         | Pinned in `app/build.gradle.kts` (`ndkVersion`)   |
| CMake            | `3.31.6`                | JUCE needs ≥ 3.25, so the SDK default 3.22 is too old |
| Gradle           | `9.7.1` (via wrapper)   | Needed for JDK 25 support                         |
| AGP              | `9.3.2`                 |                                                   |
| JDK              | 17+ (JDK 25 works)      | Android Studio's bundled JBR is fine              |
| MSVC (Windows)   | any with the C++ x64 tools | Host compiler for JUCE's `juceaide` — see below |

Install the SDK pieces with the bundled command-line tools. Note the `;` in
package names must reach the tool intact — use `android.exe` directly rather
than `sdkmanager.bat`, whose cmd wrapper splits on it:

```bash
"$ANDROID_HOME/cmdline-tools/latest/bin/android.exe" sdk install 'ndk;27.3.13750724' 'cmake;3.31.6'
```

## Building

```bash
cd android && ./gradlew assembleDebug
```

The APK lands in `android/app/build/outputs/apk/debug/`.

It is large: roughly 114 MB for a debug build of both ABIs, because the UI tree
is ~47 MB of assets (`demo/` and `images/` are ~15 MB each) and each
`libjuce_jni.so` is ~35 MB unstripped. A release build of a single ABI is far
smaller. If the size ever needs attacking, the assets are the place to start —
and note that they are unpacked to the data directory at runtime, so they cost
that space twice on device.

**Use a release build for anything to do with audio.** A debug build compiles
the DSP at `-O0`; on a phone that is not merely slower but unusable — glitching,
dropouts, latency nowhere near what the hardware can do. Judge nothing about
tone or latency from a debug build.

```bash
cd android && ./gradlew assembleRelease -Pssg.abis=arm64-v8a
```

That configures CMake as `Release`, which brings `-O3`, `-ffast-math` and
LTO. The APK lands in
`android/app/build/outputs/apk/release/`.

The build type is set explicitly in `app/build.gradle.kts`. Left to itself,
AGP configures CMake as `RelWithDebInfo` for any non-debuggable variant —
CMake's `-O2 -g` — and JUCE attaches its LTO flags and its `-O3` to the
Release configuration only, so a release build would silently ship without
them. AGP loses nothing by the override: the NDK toolchain adds `-g` to every
configuration, and AGP strips the packaged library itself.

The release variant is signed with the **debug** keystore so it can be installed
and played through directly. That is a testing convenience, not a shipping
configuration: distribution needs a real keystore and the `signingConfig` line in
`app/build.gradle.kts` replaced.

`ssg.abis` picks the ABIs. The native project is large, so building one ABI is
much faster while iterating:

```bash
./gradlew assembleDebug -Pssg.abis=x86_64
```

Use `x86_64` for the emulator and `arm64-v8a` for a real phone; the default in
`gradle.properties` is `arm64-v8a`.

Install and launch:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.soundshed.guitar/.MainActivity
adb logcat -s SoundshedGuitar JUCE
```

On Windows, `deploy.bat [release|debug] [build] [nolaunch] [abi=<abi>]` does the
first two steps against the first connected phone or tablet, skipping emulators,
and finds `adb` from `local.properties`. The release APK is the default, and the
arguments may appear in any order.

Add `build` (or `rebuild`) to run the Gradle build first, so one command covers
the whole edit-run loop:

```bat
deploy.bat build
```

It builds for the ABI the connected device reports, falling back to `arm64-v8a`
if it cannot be read; `abi=x86_64` overrides that (quote the argument to pass a
comma-separated list, since cmd splits on commas). A failed build stops before
the install.

## How it differs from the desktop builds

### Formats

`juce/CMakeLists.txt` builds only the Standalone format when
`CMAKE_SYSTEM_NAME` is `Android`. There is no plugin host on Android to load
AU/VST3/AAX into, and JUCE emits each format as a separate shared library.

### The shared library is called `juce_jni`

JUCE's Java bootstrap (`com.rmsl.juce.Java`) calls
`System.loadLibrary("juce_jni")`, so `android/CMakeLists.txt` renames the
standalone target's output and redirects it into the directory AGP passes as
`CMAKE_LIBRARY_OUTPUT_DIRECTORY`. Without both fixups the library would be
named after `PRODUCT_NAME` and land in JUCE's `*_artefacts` tree, where Gradle
would never find it.

### Features that are off

| Feature                      | Why                                                              |
| ---------------------------- | ---------------------------------------------------------------- |
| WASM effect host             | Wasmtime publishes no Android C API bundle                        |
| Hosting VST3/AU/LV2 plugins  | No desktop plugin format exists on Android; the format manager compiles with nothing registered |
| ASIO                         | Windows-only; Android uses Oboe (AAudio/OpenSL ES) instead        |
| AVX2                         | x86-only, and the Android targets are arm64/x86_64                |

### The web UI ships as APK assets

On desktop, CMake copies `core/ui` next to the built binary and the editor
reads `<exe dir>/resources/ui/...`. Android has no directory next to the
binary, so Gradle's `stageUiAssets` task packages the same tree into the APK
under `ui/`, and `SoundshedApp` unpacks it to `<dataDir>/resources` before JUCE
starts.

Unpacking rather than reading assets in place is deliberate: the editor is not
the only consumer of that tree — the factory preset pack
(`core/ui/presets/factory/*.soundshed.presets`), metronome clicks and effect
layout images are all resolved as ordinary files by the framework-agnostic
core. Unpacking once keeps every one of those paths working unchanged. A stamp
file written by Gradle is compared on each launch, so it is a first-run and
post-upgrade cost only.

The stamp is a SHA-256 over the staged tree, not the version string. That
matters during development: the version stays put while the UI changes
underneath it, and a version-derived stamp leaves the device serving a stale
unpacked copy that looks correct in every build output — a genuinely confusing
way to lose an afternoon.

### Paths

`FileSystem::SetPlatformRootOverride()` hands the core its sandbox path at
startup. Android has no `HOME` to derive a settings root from, so without this
the core would fall back to a relative `settings` directory.

### JUCE's Java sources

`app/build.gradle.kts` adds JUCE's own `native/java` and `native/javaopt`
directories to the app's source set rather than copying them (which is what the
Projucer does), so there is no second copy to keep in sync when JUCE is
updated. `juce_gui_extra`'s `javaopt` directory is deliberately excluded: it
holds Firebase push-notification services this app does not use.

## Android WebView quirks the port had to work around

Four of these only show up at runtime, and three of them present as "the app
starts and then shows a blank screen", so they are worth knowing about.

**The app entry point is found with `dlsym`.** Android has no `main()`; JUCE
looks up `juce_CreateApplication` in the loaded library at runtime
(`juce_Messaging_android.cpp`). That means the symbol has to survive two things
this project's build would otherwise do to it: `Main.cpp` lives in a static
library nothing references, so the linker drops the whole archive member
(`android/CMakeLists.txt` forces it back with `--undefined=`), and the build
compiles with `-fvisibility=hidden`, which would keep it out of the dynamic
symbol table (`Main.cpp` declares it with default visibility explicitly).

**The window must be sized after it is visible.** The editor hosts the WebView
through JUCE's `AndroidViewComponent`, which positions a real
`android.webkit.WebView` from the JUCE component's screen bounds — and can only
do that once the component has a peer. `MainWindow` therefore fills the display
after `setVisible()`, not before. It also does not call `setFullScreen()`: on
Android that collapses the window to 0x0, and the peer already covers the
activity.

**MIME types must not carry a charset.** Android's `WebResourceResponse` takes
the encoding as a separate argument and documents that the MIME type "must not
include a charset parameter". Serving `text/html; charset=utf-8` from the
resource provider makes the WebView treat the page as an unknown type and
render the markup as plain text, so `getMimeForExtension` returns a bare
`text/html` on Android.

**The WebView needs DOM storage and a definite size.** Both are fixed in
`android/app/src/main/java/com/rmsl/juce/JuceWebViewClasses.java`, a patched
copy of a JUCE file — see below.

**The page must bootstrap JUCE's JavaScript layer itself.** This is the one
that looks like a feature bug rather than a platform bug, so it is worth
understanding properly.

On desktop, JUCE injects its integration script natively before any page script
runs — WebView2's `AddScriptToExecuteOnDocumentCreated`, `WKUserScript` on Apple
platforms, WebKitGTK's user content manager. `android.webkit.WebView` has no
equivalent API. JUCE's fallback is to hand the scripts to the page through
`window.__JUCE__.getAndroidUserScripts()` and expect the page to `eval` them
(see `lowLevelIntegrationsScript` in `juce_WebBrowserComponent.cpp`, whose first
statement is exactly that eval).

Nothing does that automatically. Without it `window.__JUCE__` stays the bare
Java interface object — just `getAndroidUserScripts` and `postMessage` — so
`window.__JUCE__.backend` and `window.IPlugSendMsg` never exist and **the UI
cannot send a single message to the engine**. The app still loads, renders and
looks healthy, because the UI's static assets come through the resource
provider over `fetch()` rather than the bridge; what you get is a fully drawn
app with an empty preset library and controls that do nothing.

`core/ui/index.template.html` now runs that eval in a guarded inline script
before any other script. The guard keys off `getAndroidUserScripts` being
defined, so it is inert on every other platform.

**`build-flags.js` has to be generated for Android too.** `index.html` loads it
unconditionally, and on desktop CMake generates it from
`juce/cmake/ui-build-flags.js.in`. Nothing in `core/ui` produces it, so the
Gradle `stageUiAssets` task writes it into the staged assets (mirroring
`GUITARFX_ENABLE_JAM`, overridable with `-Pssg.jam=false`).

## The vendored `JuceWebViewClasses.java`

JUCE resolves its Java classes through the app's class loader before falling
back to the copy it embeds as bytecode (`JNIClassBase::initialise`), so a class
of the same name in the app wins. Copying JUCE's `java/` sources into the app
project is the mechanism JUCE expects — it is what the Projucer does — but it
does mean this one file is a fork.

Two changes, both marked with `Soundshed patch` comments:

1. **`setDomStorageEnabled(true)`.** Android WebView leaves DOM storage off by
   default, which makes `window.localStorage` *null* rather than empty. The UI
   reads it during startup, so without this the app dies on its splash screen
   with `TypeError: Cannot read properties of null (reading 'getItem')`.

2. **An `onSizeChanged` override that pins `LayoutParams` and measures.** JUCE
   adds the WebView with no `LayoutParams`, so it inherits `WRAP_CONTENT`, and
   `ComponentPeerView.onLayout` is empty so nothing ever measures it. A
   `WRAP_CONTENT` WebView is the case Android documents as unsupported for
   percentage heights. The symptom is deeply misleading: the page loads and
   renders, JS reports a correct `window.innerHeight`, but every percentage and
   `vh`/`dvh` height resolves to 0 — so `html, body { height: 100% }` collapses
   the whole UI and the app shows a blank screen with a perfectly healthy page
   inside it.

The build guards against silent drift: `app/build.gradle.kts` pins the SHA-256
of the upstream file, and `checkVendoredJuceWebView` (wired into `preBuild`)
fails if JUCE's copy changes, so the patch gets re-applied deliberately on a
JUCE upgrade.

## The `juceaide` host-compiler requirement

JUCE builds a helper tool, `juceaide`, during the configure step by re-invoking
CMake with the same generator but no toolchain file, so that it comes out as a
*host* binary. Gradle always drives the Android build with the Ninja generator,
and Ninja plus MSVC only works when the MSVC environment variables are already
set — which they are not unless the build was launched from a Developer Command
Prompt.

Requiring that would be a poor contract, and Android Studio would never satisfy
it. `android/cmake/HostToolchain.cmake` instead locates the Visual Studio
installation with `vswhere`, runs `vcvars64.bat`, and imports the resulting
`PATH`/`INCLUDE`/`LIB` into the configure run. JUCE's nested configure inherits
that environment and finds a host compiler on its own. It is a no-op when a
host compiler is already reachable, and on non-Windows hosts.

## Why `minSdk` is 29

API 29 (Android 10) is a hard floor, not a preference. JUCE 8's Android font
backend, `juce_Fonts_android.cpp`, is built on the `AFontMatcher` API added in
API 29. JUCE guards its own calls with `__builtin_available (android 29, *)`,
but the `AFontMatcher_destroy` and `AFont_close` deleters are instantiated
through `unique_ptr` outside any such guard, so the module does not compile at
all below 29.

Little is lost: API 29 is also where the AAudio low-latency path is mature.

## Network access

The manifest declares `INTERNET` and `ACCESS_NETWORK_STATE`. The UI reaches
`api-guitar.soundshed.com` (tone sharing), `www.tone3000.com` (model search) and
`www.googleapis.com` (YouTube), all over `fetch()` from the WebView.

Worth knowing because the failure is misleading: without `INTERNET` every
request fails as `TypeError: Failed to fetch`, which reads like a CORS or
endpoint problem and sends you looking in entirely the wrong place. The page's
origin is the synthetic `https://juce.backend`, which makes a CORS theory all
the more plausible — and wrong.

Cleartext traffic stays disabled (the targetSdk 28+ default). Every endpoint
above is HTTPS.

## Orientation

The activity is locked to landscape (`android:screenOrientation="landscape"` in
the manifest). The UI is a desktop layout and portrait does not give it enough
width to be usable — in portrait the footer drops nearly every control into its
overflow panel and the signal chain has room for barely two nodes.

Locked, not sensor-driven: the app will not rotate at all. If the 180° flip is
ever wanted — which is reasonable, since it decides which end the interface
cable comes out of — `sensorLandscape` is the one-word change.

## Safe-area insets

The app draws edge to edge — from targetSdk 35 Android enforces it, so opting
out is not a reliable fix — which puts the status bar over the header and the
navigation bar over the footer. The footer is the one that actually breaks
things: the nav bar does not merely obscure those controls, it takes their taps.

`env(safe-area-inset-*)` is no help. Chromium only populates it on platforms
that feed insets to the renderer, and Android WebView is not one of them; the
values read as zero.

So `SafeAreaInsets` reads the real insets (system bars plus display cutout),
converts them from physical to CSS pixels, and sets them on the page as
`--ssg-safe-top` / `-bottom` / `-left` / `-right`.
`core/ui/css/variables.css` declares all four as `0px`, so every other platform
is unaffected, and `.icon-bar` and `.footer-bar` add them to their own padding —
the bars keep their spacing and simply grow to cover the system bars, so
backgrounds still run to the screen edge while the controls sit clear.

It is driven from the **WebView**, not the activity, and that is not a stylistic
choice. With no native parent supplied, JUCE does not add its peer view to the
activity's content view: it goes through `WindowManager.addView` as a window of
its own (`juce_Windowing_android.cpp`). The WebView is therefore unreachable by
walking `getWindow().getDecorView()`, and an `OnApplyWindowInsetsListener` on
the decor view is doubly useless — `DecorView` does not dispatch to a listener
set on itself, so that callback never arrives at all.

The call sites are in the vendored `JuceWebViewClasses.java`:
`Client.onPageFinished`, because the values are inline styles that a navigation
discards and JUCE always navigates once after creating the view; the
`onSizeChanged` override, which is what rotation looks like from there; and
`SafeAreaInsets.watch` in the constructor, for insets that change while the
view keeps its size. That last one matters more than it sounds: an app launched
or resumed behind the lock screen reads a top inset of 0, because the lock screen
owns the status bar then, and the status bar appears over the header on unlock
without the view being resized. `watch` follows both the view's insets listener
and its window's layout passes, and only pushes values that changed.

## Audio

Oboe ships with JUCE and `JUCE_USE_ANDROID_OBOE` defaults to 1, so the app gets
AAudio with an OpenSL ES fallback.

### The capture input preset

Oboe defaults its capture `InputPreset` to `VoiceRecognition`, which is wrong
for an amp sim: it routes the signal through the device's voice capture path,
which on Samsung hardware in particular means AGC, noise suppression and
band-limiting — all in front of the DSP, and all adding latency. Android's
`Unprocessed` preset is what instrument input wants.

Setting it is more awkward than it should be. JUCE opens the stream in
`AndroidOboeAudioIODevice::open()` with a local `oboe::AudioStreamBuilder` and
never calls `setInputPreset()`; there is no JUCE config flag for it, and
`oboe::DefaultStreamValues` covers only sample rate, burst size and channel
count. The file is inside the JUCE submodule, so it cannot be forked here the
way `JuceWebViewClasses.java` can. The only remaining lever is the default
member initialiser in Oboe's own header.

`android/oboe-overlay/oboe/AudioStreamBase.h` is therefore a copy of that header
with one value changed, forced to the front of the include path by
`android/CMakeLists.txt` (directory scope, so Oboe's own sources and JUCE's
compile against the same value). `checkVendoredOboeHeader` pins the upstream
hash and fails the build if it moves.

**Known limitation.** Because this is a compile-time default it cannot query
`AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED` and fall back
deliberately, which is what a proper implementation would do. `Unprocessed` is a
*request* — AAudio substitutes another source on devices that do not implement
it rather than failing to open — so the failure mode is a silent fallback, not a
broken stream. Doing it properly needs JUCE to expose the preset; that is an
upstream change or a JUCE fork, not something this repository can reach.

Check what a device actually negotiated with:

```bash
adb shell dumpsys audio | grep -i soundshed
```

Latency depends entirely on the hardware path. The built-in microphone on a
phone is not a usable guitar input; a USB or Bluetooth audio interface is. The
manifest declares `android.hardware.usb.host`, `android.hardware.audio.low_latency`
and `android.hardware.audio.pro` as optional features so the app installs
everywhere but can be filtered on capability.

The emulator has no low-latency audio path and routes the host microphone, so
treat it as a way to check that the app boots and the UI works, not as a way to
judge tone or latency.

### USB interfaces with no 16-bit format (the MMAP rule)

On the Galaxy S21 (Android 15) AAudio's MMAP path, which runs through the
primary HAL rather than the USB HAL, opens a USB interface at 16-bit PCM
whatever formats the interface offers. An interface that only offers 24-in-32
(our POD Go firmware, "POGO": `AudioDeviceInfo.getEncodings()` is `[4, 22]`,
float and PCM_32BIT) then plays distorted and records full-scale noise. Android's
ordinary (non-MMAP) path converts properly and sounds clean, at a cost of a
192-frame burst and a 256-frame block, about 2 ms more each way.

So both apps follow one rule. If any attached USB audio device lists encodings
and none of them is PCM_16, the MMAP path is off (`AAudio_setMMapPolicy(NEVER)`,
reached through `dlsym` from `libaaudio.so`). Otherwise MMAP policy is `AUTO`.
The policy is process-wide and applies to streams opened afterwards, so the
audio device is reopened when the decision changes:

- `android/shared/java/com/soundshed/audio/UsbAudioPath.java` watches devices
  with an `AudioDeviceCallback` and tells the native side. Both apps add
  `android/shared/java` to their Java sources.
- `juce/source/AndroidUsbAudioPath.{h,cpp}` sets the policy and calls a handler.
  The WebView app's handler (`juce/source/Main.cpp`) closes and restarts the JUCE
  audio device. Nano Q's (`nanoq/android/native/NanoQJni.cpp`) calls
  `AudioIO::Restart()`.

The log says which way it went (`adb logcat -s SoundshedGuitar`): "USB audio:
MMAP path off for USB-Audio - POGO output [4, 22] (no 16-bit format)". Nano Q
also logs whether each stream opened MMAP. `adb shell dumpsys media.audio_flinger`
lists `Mmap` threads only when the MMAP path is in use. The `aaudio.mmap_policy`
system property can't be set on a retail phone, so the app has to do this itself.

An interface that adds a 16-bit alternate setting gets the MMAP path back
automatically. JUCE's Oboe device list is still read only once, at startup.

### Buffer size

JUCE's default on Android is 40 ms of output buffering on a "low latency"
device — twenty 96-frame bursts on typical hardware — which is a media-app
default, not an amp-sim one. `juce/source/Main.cpp` asks for two bursts
instead, 192 frames, and hands the value to the device two ways.

As the holder's *preferred setup*, the default applies to any open that has no
saved setup to restore. That includes the first run, where JUCE defers the open
until the microphone permission is granted — long after the main window has
been constructed. A saved setup wins over the default from then on; Settings →
Audio Device writes one as soon as anything changes there, so a size chosen in
the app sticks across launches even when Android kills it rather than letting
it exit.

The property is different: while it is set it is enforced whenever a device is
open, saved setup or not, which is what makes tuning without a rebuild
possible. Clear it to hand control back to the settings:

```bash
adb shell setprop debug.soundshed.audio.buffer 384
adb shell am force-stop com.soundshed.guitar   # then relaunch
adb shell setprop debug.soundshed.audio.buffer '""'   # clear it again
```

`0` keeps JUCE's default.

Two things about what the number means. It is the output stream's buffering
depth, not the processing block: JUCE's Oboe device never sets
frames-per-callback, so AAudio delivers one hardware burst per callback
whatever is requested, and raising the value only adds jitter margin. And two
bursts is Oboe's own recommendation for a low-latency stream — one playing,
one being filled — so treat anything smaller as an experiment.

### Audio settings

JUCE's Audio/MIDI Settings dialog is a window JUCE draws itself, and on Android
it cannot share the screen with the WebView the whole UI lives in. The app has
its own controls instead, in Settings → Audio Device and MIDI Devices
(`juce/source/StandaloneAudioSettings.cpp`; the protocol is in
[user-interface.md](user-interface.md#audio-device)). Three things there matter
on a phone in particular:

- **Input mute.** JUCE's holder starts every mobile launch with the input muted
  (its "Feedback Loop: Mute audio input" guard), and only saves the choice on
  desktop, so without these settings the amp heard nothing until the dialog was
  found. Now a device pair is muted only if it looks like a built-in microphone
  playing into a built-in speaker (Oboe names them "… built-in microphone" and
  "… built-in speaker"), and a mute set by hand is remembered for that pair.
- **Record permission.** If it is refused the holder opens the device with no
  input at all. The settings show an alert that asks again, and reopen the input
  once it is granted, including when it is granted from the system settings
  while the app keeps running. After two refusals Android stops asking, and only
  the system settings can grant it.
- **Saved at once.** JUCE writes the device setup on a clean exit; these
  settings write it on every change.

**Known limitation.** JUCE's Oboe driver reads the list of audio devices once,
when the app starts (`OboeAudioIODeviceType::scanForDevices` is empty). A USB
interface plugged in later is not listed until the app is restarted, so connect
it before launching.

### Parallel DSP is off

The core can spread stereo work and parallel graph branches over helper
threads. On Android that is disabled at compile time — `kParallelDspSupported`
in `core/src/dsp/RealtimeParallel.h` — and everything runs on the audio
thread. The AAudio callback is a real-time thread while the helpers are
ordinary ones, so a callback spinning on a helper the scheduler has parked on a
little core, or behind the callback on its own core, is a missed deadline; and
at one-burst blocks the hand-off costs about as much as the work.

### Audio thread placement

Android chooses the cores for the AAudio callback, and on big.LITTLE phones it can choose
badly. On a Galaxy S21 (Exynos 2100) the callback, a `SCHED_FIFO` thread in the top-app cpuset,
never left the four Cortex-A55 little cores, while the A78 and X1 cores sat idle at their lowest
clock. The cost of that, measured on the phone with `FusedWaveNetBenchmark` pinned to each core
type (an A2 model, 96-frame blocks):

| Core          | NAM core | Fused WaveNet |
| ------------- | -------- | ------------- |
| Cortex-A55    | 895 µs   | 463 µs        |
| Cortex-A78    | 188 µs   | 93 µs         |
| Cortex-X1     | 145 µs   | 65 µs         |

The fused engine is about 2x on every core. The cluster makes a much bigger difference.

`juce/source/AndroidAudioThread.{h,cpp}` places the callback thread on its first block (and again
whenever AAudio creates a new thread):

- **ADPF.** Where the device opens a performance hint session (API 33+, and only if the vendor
  implements it), the session gets each block's budget and actual work time, and the scheduler
  picks clocks and cores from those. The S21 does not implement it
  (`adb shell dumpsys performance_hint` reports `HAL Support: false`), and many phones do not.
  The functions are looked up at runtime because `minSdk` is 29. Oboe 1.10 has its own wrapper
  (`AudioStream::setPerformanceHintEnabled`), but JUCE owns the stream and never calls it.
- **Affinity.** Without a session, the thread is pinned to every core faster than the slowest
  class, by the kernel's `cpu_capacity` (cpu4–7 on the S21), never by hard-coded core numbers.
  Android moves the app between cpusets as it comes and goes: `top-app` (0–7) in front,
  `background` (0–2 on the S21) behind the lock screen or another app. The kernel narrows a pin to
  what the cpuset allows, and the 5.4 kernel on the S21 throws it away on every move, resetting
  the thread to the new cpuset. So the pin is read back a few times a second (one cheap system
  call) and set again when it has been lost; while the cpuset holds no big core at all, it waits.

On the S21, with the "Convoluted Acoustic Sim" preset, median DSP load went from 77% to 24–29%,
p90 from 127% to 38–47%, and the peak from 190% to about 60%. The NAM node fell from 752 µs to
232–291 µs. Pinning to the X1 alone was no better: it stayed at its minimum clock.

`debug.soundshed.audio.cores` changes the policy without a rebuild, for A/B checks:

```bash
adb shell setprop debug.soundshed.audio.cores off     # old behaviour: the scheduler's choice
adb shell setprop debug.soundshed.audio.cores big     # ADPF if any, and the big cores
adb shell setprop debug.soundshed.audio.cores prime   # ADPF if any, and the fastest class only
adb shell setprop debug.soundshed.audio.cores '""'    # back to the default (auto)
adb shell am force-stop com.soundshed.guitar          # then relaunch
```

The choice is logged once per thread, and each re-pin after a cpuset move:

```
SoundshedGuitar: Audio thread 7212: performance hint unavailable
SoundshedGuitar: Audio thread 7212: pinned to cores 4,5,6,7
SoundshedGuitar: Audio thread 7212: cannot pin, cpuset allows cores 0,1,2
```

To see it working, `ps -T -p <pid> -o TID,PSR,CMD` shows the `AAudio_1` thread's last core, and
the release build's WebView exposes `webview_devtools_remote_<pid>`, so
`tools/agent-ui-debug/cdp-tool.mjs` can read `dspPerformance` through `adb forward`.

### Big-core clock floor (Samsung)

Pinning the callback to the big cores was half of it. On the S21 the `energy_step` governor
(Samsung's EMS) then holds the A78 cluster at 533–832 MHz, because one 96-frame block every
2 ms at about a fifth of the budget looks idle to it. It only raises a cluster's clock once a
core there is roughly 70–75% busy. At those clocks the same DSP takes two to three times as
long, and the worst block in each UI frame (`peakDspLoadPercent`) sits near 90% of the
deadline. The S21 has no ADPF and no sustained performance mode, and the governor accepts but
ignores per-thread `uclamp_min`. Battery saver is not the cause either: it caps the big cores
at 2080 MHz, which the callback never reaches, and the clocks collapse the same way with it off.

What the governor does honour is a minimum-frequency request, and Samsung's Galaxy Performance
SDK lets any foreground app make one. The phone carries the SDK itself:
`/system/framework/perfsdk.jar`, the `perfsdkserver` daemon, and
`libperfsdk.performance.samsung.so`. That library is a public vendor library, so the manifest
declares it with `<uses-native-library android:required="false">`. `ClockFloor.java` loads the
SDK classes from the system jar rather than bundling them. While the activity is resumed, it
asks for a CPU minimum (`CustomParams.TYPE_CPU_MIN`) and renews it every 500 ms, because the SDK
refuses requests longer than 1 s. On pause it drops the floor. On a phone without the jar it
does nothing.

The SDK's five levels map, on the S21, to 1248, 1456, 1768, 1768 and 1768 MHz on the A78 and
X1 clusters, and they hold the A55s at 1170 MHz. The default is level 1. Measured with
"Convoluted Acoustic Sim", 20 s per run, screen on, battery saver off (µs p50 unless noted;
peak is the p50 of each UI frame's worst block):

| Clock floor              | A78 MHz p50 | IR Reverb | NAM     | DSP total | Load p50 | Peak p50 |
| ------------------------ | ----------- | --------- | ------- | --------- | -------- | -------- |
| none                     | 728–832     | 148       | 340–348 | 673–693   | 34–35%   | 89–92%   |
| level 0 (1248 MHz)       | 1248        | 98        | 175     | 354       | 18%      | 43%      |
| level 1 (1456 MHz)       | 1456        | 90–93     | 149–154 | 316–317   | 16%      | 39–41%   |
| level 2 (1768 MHz)       | 1768        | 83–86     | 124–125 | 268–269   | 13%      | 32–34%   |

With battery saver on, the default floor held as well (DSP total 231 µs, peak 27%, against
487 µs and 86% without the floor).

Power, estimated from the phone's own power profile
(`dumpsys batterystats --power-profile`, mA per active A78 core at each clock): the callback
costs about 48 mA with no floor (35% duty at 728 MHz) and about 32 mA at level 1 (16% duty at
1456 MHz). The work finishes sooner at the higher clock, so the floor is roughly neutral by
that model. The model leaves out the extra leakage of idle cores at the higher voltage, and the
A55s held at 1170 MHz add a few mA for the UI threads. Over three minutes per mode the AP and
skin temperatures (`dumpsys thermalservice`) showed no sustained rise. Battery drain itself
cannot be read while the phone is on USB.

Rejected: a keep-warm spin. Spinning at the end of each block until 70% or 90% of the budget
has passed does lift the clock (to about 1248 and 1872 MHz), and so does a helper thread
spinning on a sibling A78. But because the governor wants a core 70–75% busy, any spin costs
most of a core: about +77 mA for 1.25 GHz, +180–280 mA (6–7% of the 3880 mAh battery per hour)
for 1.9–2.1 GHz, against roughly nothing for the SDK floor. Below 70% duty, the spin buys no
clock at all.

`debug.soundshed.audio.clock` changes it without a rebuild:

```bash
adb shell setprop debug.soundshed.audio.clock off       # no floor
adb shell setprop debug.soundshed.audio.clock sperf:2   # level 0-4
adb shell setprop debug.soundshed.audio.clock '""'      # back to the default (level 1)
adb shell am force-stop com.soundshed.guitar            # then relaunch
```

The log says what happened:

```
SoundshedGuitar: Clock floor: Samsung performance SDK, level 1
SoundshedGuitar: Clock floor: Samsung performance SDK did not initialise
```

"Did not initialise" can appear once right after a quick restart, while `perfsdkserver` is still
tearing down the previous process's session. The floor tries again every 2 s, up to four more
times. To see the floor itself, read
`/sys/devices/system/cpu/cpufreq/policy4/scaling_min_freq`: it reads 533000 without the floor
and the level's frequency with it.

### Sustained performance mode

`MainActivity` asks for Android's sustained performance mode
(`Window.setSustainedPerformanceMode`) when the device reports support. The
callback load is steady, and on the default governor the cores ramp up under
it and back down between callbacks; the ramp is where two-burst deadlines get
missed. The mode pins the clocks to a level the device can hold without
throttling — below peak, but constant. It applies while the activity's window
is visible. The log says which way it went:

```
SoundshedGuitar: Sustained performance mode enabled
SoundshedGuitar: Sustained performance mode not supported on this device
```
