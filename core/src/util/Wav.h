#pragma once

#include <cstdint>
#include <optional>
#include <vector>

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

} // namespace guitarfx::util
