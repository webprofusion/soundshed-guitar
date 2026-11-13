# Soundshed Nano Q on Android

Status: **in progress, 2026-11-13.** Follows the iOS port ([nano-q-iOS.md](nano-q-iOS.md)), which
it copies wherever Android allows. See Progress.

Soundshed Guitar's Android app is the JUCE plugin in a WebView ([android-build.md](../android-build.md)).
Nano Q is the native UI (QPlug + Elements, no JUCE, no WebView), and until now it had no
Android build. This plan is that build: the same CLAP plugin the desktop builds, linked into an
app that hosts it, draws its editor on the GPU and plays it through Oboe.

## What the port needs

| Layer | Desktop (Linux) | Android |
|---|---|---|
| Window host | Elements' GTK/X11/Wayland hosts | `nanoq/android/elements_host/`: a `SurfaceView` surface, EGL + GLES3; touches become Elements' mouse events, a Choreographer frame callback polls and draws, the clipboard is in-process, a hardware keyboard types |
| Drawing | Artist's Skia backend | The same, Ganesh on GLES3, with a Skia built for Android (`build-skia.ps1`). An offscreen surface keeps the window; only dirty rects are redrawn into it, then it is copied to the window's framebuffer |
| Fonts | fontconfig | The bundled fonts, scanned from a directory (`SkFontMgr_New_Custom_Directory`): `patches/elements.patch` |
| QPlug's view glue | `q_plug/src/linux/host_view.cpp` | `nanoq/android/q_plug_host_view.cpp`. QPlug sees an X11 window API on Android; the "window" it is handed is the Elements host's `android::window*` |
| App and audio | clap-wrapper's standalone, RtAudio and RtMidi | `nanoq/android/native/`: a JNI library that hosts the CLAP plugin linked in (`ClapHost`, as on iOS) and plays it through an Oboe full-duplex stream (`AudioIO`) |
| Formats | CLAP, VST3, standalone | The app only (`patches/q.patch`: no clap-wrapper, no formats) |
| Build | Ninja, `make_clapfirst_plugins` | Gradle + the NDK's CMake, `nanoq/android/` (`deploy.bat build`) |

### The app

`NanoQActivity` (Java) is one full-screen `SurfaceView`. It unpacks `assets/nanoq` (fonts, factory
presets, engine data: staged by `cmake/NanoQAndroid.cmake`) to `files/resources` once per install,
then hands `libnanoq.so` the resource, profile and state paths. Everything native runs on the
activity's main thread except the audio callback: surface and input events as they arrive, and a
Choreographer frame callback that runs the plugin's main-thread callbacks, polls the audio, and
draws.

Environment the plugin reads, set by `nativeCreate` before the plugin is made:
`SOUNDSHED_NANOQ_RESOURCES` (Paths.cpp, the Elements host's resource path),
`SOUNDSHED_NANOQ_PROFILE` (engine data), `HOME` and `XDG_CONFIG_HOME` (QPlug's user presets).

### Audio

Oboe (prefab, `com.google.oboe:oboe`) `FullDuplexStream`: a low-latency exclusive float stereo
output, and an input at the output's rate with the Unprocessed preset. The plugin is activated with
a block of the output burst rounded up to a power of two (at least 64), and callbacks are split to
that size. Without the microphone permission the app plays output only. A disconnected stream
(headphones, a USB interface) is restarted from the frame callback.

On big.LITTLE phones AAudio runs the callback on the little cores. `AudioIO` uses Soundshed
Guitar's `AudioThreadPlacement` (`juce/source/AndroidAudioThread.cpp`, no JUCE in it): ADPF
performance hints where the phone has them, else pinning the thread to the big cores, re-checked a
few times a second. `adb shell setprop debug.soundshed.audio.cores off|auto|big|prime` overrides it
(log tag `SoundshedGuitar`). The `NanoQ` log reports the DSP load every 10 s: the average, and the
worst single callback against its own frames.

### Touch

As on iOS: the first finger is the left button, a 0.5 s hold is a right click, two fingers scroll,
and a second tap within 0.35 s and 24 units is a double click. Nano Q's own long press, drag
scrolling and on-screen keyboard follow the touch build (`__ANDROID__` in `LongPress.h`,
`DragScroll.cpp` and `OnScreenKeyboard.cpp`). Back closes what is open (as Escape), then sends the
app to the background.

The UI's logical scale is the display density, capped so the window is at least 480 units tall.

## Carrying the changes to Cycfi's libraries

As for iOS: patches applied after FetchContent downloads each library, by
`ios/cmake/ApplyPatch.cmake` (skipped when already applied).

- `android/patches/q.patch` (q): on Android, no clap-wrapper and no plugin formats; clap is an
  interface library; no PkgConfig.
- `android/patches/elements.patch` (Elements' Artist): an Android Skia branch in Artist's CMake
  (Skia's static harfbuzz and freetype, EGL/GLESv3), and fonts from files in `font.cpp`.

Both are CRLF, as the fetched sources are on Windows.

## Building and running

Skia, once (Docker, ~10 minutes; the NDK and vcpkg are fetched inside the container):

```powershell
nanoq\android\build-skia.ps1
```

It installs `nanoq/android/skia/arm64-android-nanoq` (ignored by git). Then, with a phone connected:

```powershell
nanoq\android\deploy.bat build
```

Follow the log with `adb logcat -s NanoQ`.

Requirements: the Android SDK with NDK 27.3.13750724 and CMake 3.31.6 (as Soundshed Guitar's
Android build), Docker for Skia. arm64-v8a only.

## Progress

- 2026-11-13: the build, host, audio and app written. Skia built in Docker (~8 min). The release
  APK builds (55 MB; `libnanoq.so` 24 MB).
- 2026-11-13, Galaxy S21 (SM-G991B, Android 15, Mali-G78): runs. The UI draws at 1067×480 units
  (2.25 scale), touch opens editors and turns knobs, and the demo riff plays. Oboe opens full duplex
  at 48 kHz, burst 96, block 128, exclusive both ways. Playing "Black Album (Full Rig)" (two NAM
  models): with placement off the callback ran on a little core at 59-66% average load, worst
  callbacks 164-221% (dropouts); with placement on (no ADPF on this phone, so pinned to cores
  4-7) 34-37%, worst 64-67%. The UI thread is ~4%.

## Not done yet

- Network: no HTTP transport on Android (`HttpNone`), so the Tones page cannot sign in or download.
- WASM effects are off (as on iOS).
- MIDI (USB MIDI through `android.media.midi`).
- Audio device choice and buffer size in the app's settings.
- x86_64 and 32-bit ARM.
- A soft keyboard from the system IME (Nano Q's on-screen keyboard is used instead).
- Samsung's clock floor (`ClockFloor.java` in Soundshed Guitar's Android app), which keeps the big
  cores from clocking down between callbacks.
