#pragma once

#include <filesystem>
#include <vector>

namespace nanoq
{
/// The one profile every Soundshed Guitar product shares: settings, soundshed.db, the resource
/// library, riffs and logs. The same folder JUCE's userApplicationDataDirectory gives
/// Soundshed Guitar and Nano (juce/source/ProfileFolder.h), so the products read each
/// other's data.
[[nodiscard]] std::filesystem::path ProfileFolder();

/// With SOUNDSHED_NANOQ_PROFILE set, points the engine's data root at that folder instead (tests
/// and development). Call before the engine is constructed.
void ApplyProfileOverride();

/// The directory holding this plugin's binary (the dll, dylib or so), not the host's.
[[nodiscard]] std::filesystem::path ModuleDirectory();

/// True when this code is the running program rather than a plugin loaded into a host: the
/// standalone app clap-wrapper builds links the plugin into its own executable.
[[nodiscard]] bool RunningAsStandalone();

/// Where a resources/ folder may sit relative to the binary, for every plugin layout.
[[nodiscard]] std::vector<std::filesystem::path> ResourceCandidates();
} // namespace nanoq
