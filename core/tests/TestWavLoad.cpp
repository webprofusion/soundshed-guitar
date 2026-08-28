#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "util/Wav.h"

#ifndef GUITARFX_TEST_RESOURCES_DIR
    #error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

namespace fs = std::filesystem;

namespace
{
std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        return {};
    }

    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool ReportCheck(bool condition, const std::string& message)
{
    std::cout << message << (condition ? " PASS" : " FAIL") << '\n';
    return condition;
}

bool TestDecodeRegressionFixture()
{
    const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
    const fs::path wavPath = resourcesDir / "assets" / "wav" / "M1F1-uint8WE-AFsp(1).wav";

    std::cout << "Test: Decode extensible 8-bit stereo WAV fixture\n";
    std::cout << "Fixture: " << wavPath << '\n';

    bool passed = true;
    passed &= ReportCheck(fs::exists(wavPath), "  fixture exists:");

    if (!passed)
    {
        return false;
    }

    const auto bytes = ReadBinaryFile(wavPath);
    passed &= ReportCheck(!bytes.empty(), "  fixture bytes loaded:");

    if (!passed)
    {
        return false;
    }

    const auto decoded = guitarfx::util::DecodePcmWav(bytes);
    passed &= ReportCheck(decoded.has_value(), "  DecodePcmWav accepts WAVE_FORMAT_EXTENSIBLE PCM:");

    if (!decoded)
    {
        return false;
    }

    passed &= ReportCheck(decoded->channels == 2, "  channel count is stereo:");
    passed &= ReportCheck(static_cast<int>(decoded->sampleRate) == 8000, "  sample rate is 8000 Hz:");
    passed &= ReportCheck(decoded->bitsPerSample == 8, "  bit depth is 8-bit:");
    passed &= ReportCheck(decoded->channelSamples.size() == 2, "  decoded channel buffers present:");
    passed &= ReportCheck(!decoded->channelSamples.empty() && decoded->channelSamples[0].size() == 23493,
                          "  decoded frame count matches expected fixture:");

    if (passed)
    {
        const double firstLeft = decoded->channelSamples[0].empty() ? 0.0 : decoded->channelSamples[0].front();
        const double firstRight = decoded->channelSamples[1].empty() ? 0.0 : decoded->channelSamples[1].front();
        std::cout << "  first samples: L=" << firstLeft << ", R=" << firstRight << '\n';
    }

    return passed;
}
} // namespace

int main()
{
    const bool passed = TestDecodeRegressionFixture();
    std::cout << (passed ? "\nWAV decode regression tests PASSED\n" : "\nWAV decode regression tests FAILED\n");
    return passed ? 0 : 1;
}
