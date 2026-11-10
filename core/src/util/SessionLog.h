#pragma once

#include <filesystem>
#include <string_view>

namespace guitarfx::util
{
/// The session log under a settings directory: <dir>/logs/session-log.txt.
[[nodiscard]] std::filesystem::path SessionLogPath(const std::filesystem::path& settingsDirectory);

/// Appends one timestamped line to the session log under `settingsDirectory`, or the profile's
/// when that is empty.
///
/// Diagnostics go here, never to stdout or stderr. A GUI app has no console to show those on,
/// and when the app is started by something that captures them and then stops reading, every
/// write blocks once the pipe is full: that hung the Standalone mid preset switch. Lines from
/// different threads never interleave. Not for the audio thread, as it opens a file. A line
/// the profile will not take goes to <temp>/Sepia/session-log.txt instead; one
/// that cannot be written there either is dropped. This never throws.
void AppendSessionLog(const std::filesystem::path& settingsDirectory, std::string_view message);

/// AppendSessionLog to the profile's settings directory (FileSystem::ResolveSettingsDirectory),
/// for code with neither a controller nor a store of its own to log beside.
void AppendSessionLog(std::string_view message);
} // namespace guitarfx::util
