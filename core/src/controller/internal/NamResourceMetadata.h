#pragma once

/**
 * NamResourceMetadata.h — Reading NAM capture metadata and mapping it onto
 * library categories.
 *
 * A .nam file is one JSON document whose weights array is nearly all of it, so
 * it is skimmed rather than parsed (util/JsonSkimmer.h), and only its top-level
 * members are read: an A2 file nests whole submodels, each with its own
 * "architecture" and "metadata". The reader here is what the importer, the
 * archive loader and the folder browser all use, so an imported capture is
 * filed the same way whichever route it arrived by.
 */

#include <filesystem>
#include <iosfwd>
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
    /// "A2" for a SlimmableContainer or an A2-layout WaveNet, "A1" for any other
    /// WaveNet, empty for the rest (LSTM, ConvNet, Linear). What the badges show.
    std::string architectureVersion;
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

/// Store meta key and value recording that every NAM row has been re-read by the
/// current reader. Raise the value when the reader starts reading a field differently
/// and the library re-reads each model once at the next launch.
inline constexpr const char* kNamMetadataRevisionKey = "nam_metadata_revision";
inline constexpr const char* kNamMetadataRevision = "2";

/// Reads a .nam model's top-level fields from a stream. Malformed input yields
/// whatever was read before the fault.
[[nodiscard]] NamFileMetadata ReadNamMetadata(std::istream& in);

/// Reads a .nam file's top-level fields. Missing or malformed files yield a
/// default-constructed result rather than an error.
[[nodiscard]] NamFileMetadata TryExtractNamMetadata(const std::filesystem::path& namFilePath);

/// "A1", "A2" or empty for a top-level architecture name, given whether a WaveNet's
/// layers have the A2 layout.
[[nodiscard]] std::string ClassifyNamArchitectureVersion(const std::string& architecture, bool waveNetHasA2Layers);

enum class NamMetadataRefresh
{
    /// Descriptive fields only where the row has none, so a user's edit stands.
    FillMissing,
    /// Every field the file has replaces the row's: for rows an older reader filled.
    FromFile,
};

/// Fills in a library resource's metadata from the capture's own file. The file's
/// structural fields (architecture and its A1/A2, version, sample rate, gear type)
/// always win; see the definition for what else does.
void EnrichNamResourceMetadata(LibraryResource& resource, const std::filesystem::path& namFilePath,
                               NamMetadataRefresh refresh = NamMetadataRefresh::FillMissing);

/// Store meta key and value recording that every NAM and IR row has its content hash
/// (models/ContentHash.h). Rows imported since carry one from the import; raise the value
/// if the recipe the library uses changes and every row is hashed again at the next launch.
inline constexpr const char* kContentHashRevisionKey = "content_hash_revision";
inline constexpr const char* kContentHashRevision = "1";

/// Sets a NAM or IR resource's "contentHash" metadata from its file, or removes it when the
/// file is missing or unhashable. Other resource types are left alone. True when it changed.
bool RefreshContentHashMetadata(LibraryResource& resource);

/// Lowercases and strips separators so category spellings compare equal.
[[nodiscard]] std::string NormalizeCategoryToken(std::string value);

/// Maps a free-form category string onto one of the library's fixed categories.
[[nodiscard]] std::optional<std::string> MapToLibraryCategory(const std::string& rawCategory);

/// Final category for a resource: the requested one when it maps cleanly,
/// otherwise one inferred from the resource's own metadata. An IR is always
/// filed under "cab" or "reverb" (IrResourceCategory.h), and may be read to decide.
[[nodiscard]] std::string ResolveResourceLibraryCategory(const LibraryResource& resource,
                                                         const std::string& requestedCategory);

/// True for effect types backed by a NAM model.
[[nodiscard]] bool IsNamEffectType(const std::string& type);

/// True for NAM effect types that accept interface calibration input levels.
[[nodiscard]] bool IsNamCalibratableEffectType(const std::string& type);
} // namespace guitarfx::controller_detail
