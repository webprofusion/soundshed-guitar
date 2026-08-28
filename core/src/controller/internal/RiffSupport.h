#pragma once

/**
 * RiffSupport.h — Where riff takes live on disk.
 *
 * Takes are stored relative to the riff library folder so a library can be
 * moved or synced between machines; they are resolved back to absolute paths
 * at playback time. Both directions are here so they cannot disagree.
 */

#include <filesystem>

namespace guitarfx::controller_detail
{

inline constexpr const char* kRiffLibraryPathSettingKey = "riffLibrary.path";
inline constexpr const char* kRiffLibraryDefaultFolder = "riff-library";
inline constexpr const char* kRiffLibraryDocumentId = "riff-library";

/// Resolves a stored (usually library-relative) take path to an absolute one.
[[nodiscard]] std::filesystem::path ResolveRiffTakePathForRuntime(const std::filesystem::path& storedPath,
                                                                  const std::filesystem::path& libraryPath);

/// Converts a runtime path back to the library-relative form written to the index.
[[nodiscard]] std::filesystem::path BuildRiffTakePathForStorage(const std::filesystem::path& runtimePath,
                                                                const std::filesystem::path& libraryPath);

} // namespace guitarfx::controller_detail
