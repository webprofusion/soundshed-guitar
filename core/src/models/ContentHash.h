#pragma once

/**
 * ContentHash.h — fingerprints of model files that ignore their metadata.
 *
 * A NAM capture or an IR can reach the library many ways: a Tone3000 import, a folder, a
 * preset archive, a renamed copy. The engine's own file hash (ModelHasher, FNV-1a of every
 * byte) changes when anyone edits a name or a calibration level, so it cannot say that two
 * files are the same model. A content hash covers only what is run:
 *
 *  - NAM: "nam-content-v1:" + hex SHA-256 of "nam-content-v1\0" and then, for each leaf model
 *    depth-first, its weight count as a u64 LE and each weight rounded to a float32 LE, -0 as
 *    +0. A SlimmableContainer's leaves are its submodels from the smallest max_value up; a
 *    Sequential's are its models in order. This is the Neural Amp Modeler trainer's recipe
 *    (nam/models/content_hash.py), written into newer files as metadata.weights_hash.
 *  - IR: "ir-content-v1:" + hex SHA-256 of "ir-content-v1\0" and the WAV file's data chunk.
 *
 * Tone3000 gives each of its models the same value as `content_hash` and looks models up by
 * it, which is how a local file finds its Tone3000 tone. The Soundshed API computes it too
 * (api/src/lib/content-hash.ts in soundshed-guitar-private), from the same test vectors.
 */

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace guitarfx
{
/// The library metadata key a NAM or IR resource's content hash is kept under.
inline constexpr const char* kContentHashMetadataKey = "contentHash";

/// Whether `value` is a content hash as Tone3000 accepts one: "nam-content-v1:" or
/// "ir-content-v1:" and 64 lowercase hex digits.
[[nodiscard]] bool IsContentHash(std::string_view value);

/// The weights hash of a .nam model read from a stream, or nullopt when the document is
/// malformed or the model is unhashable (a leaf with no weights, a container with no children
/// or two submodels with the same max_value, a weight that is not a finite float32).
[[nodiscard]] std::optional<std::string> NamWeightsHash(std::istream& in);

/// The IR content hash of a WAV file's bytes, or nullopt when they are not a RIFF/WAVE file
/// with a data chunk.
[[nodiscard]] std::optional<std::string> IrContentHash(std::span<const std::uint8_t> wavBytes);

/// The content hash of a library file of the given resource type ("nam" or "ir"), or nullopt
/// for other types and for files that are missing, unreadable or unhashable.
[[nodiscard]] std::optional<std::string> ContentHashOfFile(const std::string& resourceType,
                                                           const std::filesystem::path& filePath);
} // namespace guitarfx
