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
  local.properties        Machine-local SDK path (not checked in)
  app/
    build.gradle.kts      App module: NDK/CMake wiring, JUCE Java sources, UI assets
    src/main/AndroidManifest.xml
    src/main/java/com/soundshed/guitar/
      SoundshedApp.java   Unpacks the web UI from APK assets on first run
      MainActivity.java   JuceActivity subclass; requests microphone access
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

That maps to `CMAKE_BUILD_TYPE=Release`, which brings `-O3`, `-ffast-math` and
LTO, and it also drops `JUCE_FORCE_DEBUG` (see below). The APK lands in
`android/app/build/outputs/apk/release/`.

The release variant is signed with the **debug** keystore so it can be installed
and played through directly. That is a testing convenience, not a shipping
configuration: distribution needs a real keystore and the `signingConfig` line in
`app/build.gradle.kts` replaced.

Note `juce/CMakeLists.txt` sets `JUCE_FORCE_DEBUG=1` for every platform except
Android. It exists so Windows release builds keep their WebView2 DevTools, but
it also leaves JUCE's assertions and debug-only paths compiled in — including on
the audio thread — so on Android it is off and a release build is genuinely a
release build.

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
adb logcat -s SoundshedGuitar juce
```

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

The two call sites are in the vendored `JuceWebViewClasses.java`:
`Client.onPageFinished`, because the values are inline styles that a navigation
discards and JUCE always navigates once after creating the view; and the
`onSizeChanged` override, which is what rotation looks like from there.

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
