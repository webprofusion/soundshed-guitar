#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace guitarfx
{
enum class SampleRateConversionQuality;
}

namespace guitarfx::util
{
struct DecodedWav
{
    double sampleRate = 0.0;
    int channels = 0;
    int bitsPerSample = 0;
    std::vector<std::vector<double>> channelSamples;
};

[[nodiscard]] std::optional<DecodedWav> DecodePcmWav(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] std::vector<std::vector<float>> ConvertToSampleRate(const DecodedWav& wav, double targetRate);
[[nodiscard]] std::vector<std::vector<float>> ConvertToSampleRate(const DecodedWav& wav, double targetRate,
                                                                  guitarfx::SampleRateConversionQuality quality);

/// Encodes interleaved stereo 16-bit PCM WAV bytes. Samples are clamped to
/// [-1, 1]. Returns an empty vector if the channels are empty, mismatched in
/// length, or the sample rate is not positive.
[[nodiscard]] std::vector<std::uint8_t> EncodeStereo16BitWav(const std::vector<float>& left,
                                                             const std::vector<float>& right, int sampleRate);

/// Writes the bytes produced by EncodeStereo16BitWav to disk, creating parent
/// directories as needed. Returns false on invalid input or any I/O failure.
[[nodiscard]] bool WriteStereo16BitWav(const std::filesystem::path& path, const std::vector<float>& left,
                                       const std::vector<float>& right, int sampleRate);

/// Header fields of a WAV file, read without decoding sample data. Used by
/// the folder browser to surface IR/cab metadata cheaply.
struct WavHeaderInfo
{
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerSample = 0;
    double durationSec = 0.0;
    bool valid = false;
};

/// Reads just the header of a WAV file. `valid` is false when the file is
/// missing or is not a readable RIFF/WAVE container.
[[nodiscard]] WavHeaderInfo ProbeWavHeader(const std::filesystem::path& wavFilePath);
} // namespace guitarfx::util
