#pragma once

// The standalone window's limits and the names Soundshed Guitar holds on this machine.
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
inline constexpr const char* standaloneLockName = "SoundshedGuitarStandaloneApp";

/// Windows: where the installer keeps its choices, under HKLM for a machine-wide install and HKCU
/// for a per-user one (juce/packaging/installer.iss, InstallPathsRegKey: Publisher, ProductName).
inline constexpr const char* installerRegistryKey = "Software\\Soundshed\\Soundshed Guitar\\Installer";
} // namespace soundshed::product
