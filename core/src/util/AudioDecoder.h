#pragma once

#include "Wav.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace guitarfx::util
{
/// Detects the audio format from magic bytes and delegates to the appropriate
/// decoder.  Supports WAV (RIFF/WAVE), AIFF/AIFC, and MP3 (ID3 tag or raw
/// sync-word frames).  Returns nullopt for unrecognised formats or decode
/// failures.
[[nodiscard]] std::optional<DecodedWav> DecodeAudioBytes(const std::vector<std::uint8_t>& bytes);

/// Decodes AIFF or AIFC audio from raw bytes.
/// Supported AIFC compression types:
///   NONE  — uncompressed big-endian PCM (same as plain AIFF)
///   sowt  — little-endian 16-bit PCM (common Logic Pro export)
///   fl32/FL32 — 32-bit IEEE float
///   fl64/FL64 — 64-bit IEEE float
/// Returns nullopt for unsupported compression types or malformed data.
[[nodiscard]] std::optional<DecodedWav> DecodePcmAiff(const std::vector<std::uint8_t>& bytes);

/// Decodes MP3 audio from raw bytes using minimp3.
/// Returns nullopt if the bytes do not contain a valid MP3 stream.
[[nodiscard]] std::optional<DecodedWav> DecodeMp3(const std::vector<std::uint8_t>& bytes);
} // namespace guitarfx::util
