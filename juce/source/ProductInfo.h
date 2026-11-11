#pragma once

// The standalone window's limits and the names Sepia holds on this machine.
// Soundshed Nano Q (nanoq/) shares the engine and the profile but is its own CMake project.

namespace soundshed::product
{
/// The standalone window's size and maximised state.
inline constexpr const char* windowStateFileName = "window-state.json";
inline constexpr int minEditorWidth = 640;
inline constexpr int minEditorHeight = 400;
inline constexpr int preferredEditorWidth = 1600;
inline constexpr int preferredEditorHeight = 1100;

/// The size a standalone window opens at when it has no saved state.
inline constexpr int defaultWindowWidth = 1200;
inline constexpr int defaultWindowHeight = 900;

/// Held for the life of the standalone app, so two copies never run at once: they would
/// fight over the audio device, and the app-wide UI settings would go to whichever wrote last.
inline constexpr const char* standaloneLockName = "SepiaStandaloneApp";

/// Whether the standalone app registers a URL scheme with the system. The upstream app claims
/// soundshed:// for its tone sharing links; Sepia has no such service and must not take the
/// scheme from a Soundshed Guitar installed alongside it.
inline constexpr bool registersUrlScheme = false;
} // namespace soundshed::product
