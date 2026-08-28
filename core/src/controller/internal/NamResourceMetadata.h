#pragma once

/**
 * NamResourceMetadata.h — Reading NAM capture metadata and mapping it onto
 * library categories.
 *
 * A .nam file is a JSON header followed by binary weights, so it cannot be
 * handed to a JSON parser whole. The targeted reader here is what the importer,
 * the archive loader and the folder browser all use, so an imported capture is
 * filed the same way whichever route it arrived by.
 */

#include <filesystem>
#include <optional>
#include <string>

namespace guitarfx
{
struct LibraryResource;
}

namespace guitarfx::controller_detail
{

struct NamFileMetadata
{
    std::string fileVersion;
    std::string architecture;
    std::string sampleRate;
    std::string namName;
    std::string modeledBy;
    std::string gearMake;
    std::string gearModel;
    std::string gearType;
    std::string toneType;
    std::string inputLevelDbu;
    std::string outputLevelDbu;
    std::string modelDate;
    std::string trainingFinalLoss;
};

/// Parses the JSON header of a .nam file. Missing or malformed files yield a
/// default-constructed result rather than an error.
[[nodiscard]] NamFileMetadata TryExtractNamMetadata(const std::filesystem::path& namFilePath);

/// Fills in a library resource's descriptive fields from the capture's own metadata.
void EnrichNamResourceMetadata(LibraryResource& resource, const std::filesystem::path& namFilePath);

/// Lowercases and strips separators so category spellings compare equal.
[[nodiscard]] std::string NormalizeCategoryToken(std::string value);

/// Maps a free-form category string onto one of the library's fixed categories.
[[nodiscard]] std::optional<std::string> MapToLibraryCategory(const std::string& rawCategory);

/// Final category for a resource: the requested one when it maps cleanly,
/// otherwise one inferred from the resource's own metadata.
[[nodiscard]] std::string ResolveResourceLibraryCategory(const LibraryResource& resource,
                                                         const std::string& requestedCategory);

/// True for effect types backed by a NAM model.
[[nodiscard]] bool IsNamEffectType(const std::string& type);

/// True for NAM effect types that accept interface calibration input levels.
[[nodiscard]] bool IsNamCalibratableEffectType(const std::string& type);

} // namespace guitarfx::controller_detail
