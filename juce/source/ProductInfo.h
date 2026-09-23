#pragma once

// What differs between the two products built from this source tree: Soundshed Guitar (the
// WebView UI) and Soundshed Guitar Nano (the native, WebView-free UI for small displays).
// SOUNDSHED_NATIVE_UI comes from juce/CMakeLists.txt. Everything else - the engine, the
// profile, the settings - is shared (docs/plans/native-ui.md).

#ifndef SOUNDSHED_NATIVE_UI
 #define SOUNDSHED_NATIVE_UI 0
#endif

namespace soundshed::product
{
#if SOUNDSHED_NATIVE_UI
inline constexpr bool isNano = true;

/// The standalone window's size and maximised state. Per product: the two open at very
/// different sizes, so sharing one file would hand each the other's window.
inline constexpr const char* windowStateFileName = "window-state-nano.json";

// Nano runs from phone-sized screens up: 640x360 in landscape and 360x640 in portrait are
// both supported, so the smallest window is square.
inline constexpr int minEditorWidth = 360;
inline constexpr int minEditorHeight = 360;
inline constexpr int preferredEditorWidth = 960;
inline constexpr int preferredEditorHeight = 600;

/// Soundshed Guitar owns the soundshed:// links (they open tone sharing, which Nano leaves out).
inline constexpr bool registersProtocolHandler = false;
#else
inline constexpr bool isNano = false;
inline constexpr const char* windowStateFileName = "window-state.json";
inline constexpr int minEditorWidth = 640;
inline constexpr int minEditorHeight = 400;
inline constexpr int preferredEditorWidth = 1600;
inline constexpr int preferredEditorHeight = 1100;
inline constexpr bool registersProtocolHandler = true;
#endif

/// The size a standalone window opens at when it has no saved state.
inline constexpr int defaultWindowWidth = isNano ? preferredEditorWidth : 1200;
inline constexpr int defaultWindowHeight = isNano ? preferredEditorHeight : 900;

/// Held for the life of either standalone app, so Soundshed Guitar and Soundshed Guitar Nano
/// never run at once: they would fight over the audio device, and the app-wide UI settings
/// would go to whichever wrote last. JUCE's own single-instance check keys on the app name,
/// which differs between the two, so it cannot catch this.
inline constexpr const char* standaloneLockName = "SoundshedGuitarStandaloneApp";
} // namespace soundshed::product
