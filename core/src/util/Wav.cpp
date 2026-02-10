#include "Wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
    std::uint32_t ReadUint32LE(const std::uint8_t* data)
    {
        return static_cast<std::uint32_t>(data[0])
             | (static_cast<std::uint32_t>(data[1]) << 8u)
             | (static_cast<std::uint32_t>(data[2]) << 16u)
             | (static_cast<std::uint32_t>(data[3]) << 24u);
    }

    std::uint16_t ReadUint16LE(const std::uint8_t* data)
    {
        return static_cast<std::uint16_t>(data[0])
             | (static_cast<std::uint16_t>(data[1]) << 8u);
    }
}

namespace guitarfx::util
{

std::optional<DecodedWav> DecodePcmWav(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 44) return std::nullopt;
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return std::nullopt;

    std::size_t offset = 12;
    std::uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0, blockAlign = 0;
    std::uint32_t sampleRate = 0, dataSize = 0;
    std::size_t dataOffset = 0;

    while (offset + 8 <= bytes.size())
    {
        const char* ch = reinterpret_cast<const char*>(bytes.data() + offset);
        const std::string chunkId(ch, ch + 4);
        const std::uint32_t chunkSize = ReadUint32LE(bytes.data() + offset + 4);
        const std::size_t chunkDataStart = offset + 8;
        if (chunkDataStart + chunkSize > bytes.size()) return std::nullopt;

        if (chunkId == "fmt ")
        {
            audioFormat = ReadUint16LE(bytes.data() + chunkDataStart);
            channels = ReadUint16LE(bytes.data() + chunkDataStart + 2);
            sampleRate = ReadUint32LE(bytes.data() + chunkDataStart + 4);
            blockAlign = ReadUint16LE(bytes.data() + chunkDataStart + 12);
            bitsPerSample = ReadUint16LE(bytes.data() + chunkDataStart + 14);
        }
        else if (chunkId == "data")
        {
            dataOffset = chunkDataStart;
            dataSize = chunkSize;
            break;
        }
        offset = chunkDataStart + chunkSize + (chunkSize % 2);
    }

    if (audioFormat == 0 || channels == 0 || sampleRate == 0 || bitsPerSample == 0 || blockAlign == 0 || dataOffset == 0)
        return std::nullopt;

    const std::size_t bytesPerSample = static_cast<std::size_t>(bitsPerSample) / 8;
    if (bytesPerSample == 0) return std::nullopt;

    const std::size_t frameCount = dataSize / blockAlign;
    if (frameCount == 0) return std::nullopt;

    DecodedWav wav;
    wav.sampleRate = static_cast<double>(sampleRate);
    wav.channels = static_cast<int>(channels);
    wav.bitsPerSample = static_cast<int>(bitsPerSample);
    wav.channelSamples.assign(static_cast<std::size_t>(channels), std::vector<double>(frameCount, 0.0));

    const bool isFloat = (audioFormat == 3);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const std::size_t frameOffset = dataOffset + frame * blockAlign;
        for (std::size_t ch = 0; ch < static_cast<std::size_t>(channels); ++ch)
        {
            const std::size_t so = frameOffset + ch * bytesPerSample;
            if (so + bytesPerSample > dataOffset + dataSize) return std::nullopt;

            double sample = 0.0;
            if (isFloat)
            {
                if (bitsPerSample == 32) { float v; std::memcpy(&v, bytes.data() + so, 4); sample = v; }
                else if (bitsPerSample == 64) { std::memcpy(&sample, bytes.data() + so, 8); }
                else return std::nullopt;
            }
            else
            {
                switch (bitsPerSample)
                {
                case 8:  sample = (static_cast<double>(bytes[so]) - 128.0) / 128.0; break;
                case 16: sample = static_cast<double>(static_cast<std::int16_t>(ReadUint16LE(bytes.data() + so))) / 32768.0; break;
                case 24: {
                    std::int32_t v = static_cast<std::int32_t>(bytes[so])
                                   | (static_cast<std::int32_t>(bytes[so + 1]) << 8)
                                   | (static_cast<std::int32_t>(bytes[so + 2]) << 16);
                    if (v & 0x800000) v |= ~0xFFFFFF;
                    sample = static_cast<double>(v) / 8388608.0;
                    break;
                }
                case 32: sample = static_cast<double>(static_cast<std::int32_t>(ReadUint32LE(bytes.data() + so))) / 2147483648.0; break;
                default: return std::nullopt;
                }
            }
            wav.channelSamples[ch][frame] = std::clamp(sample, -1.0, 1.0);
        }
    }
    return wav;
}

std::vector<std::vector<float>> ConvertToSampleRate(const DecodedWav& wav, double targetRate)
{
    if (wav.channelSamples.empty() || wav.channelSamples.front().empty()) return {};
    const double sourceRate = wav.sampleRate > 0.0 ? wav.sampleRate : targetRate;
    if (sourceRate <= 0.0) return {};

    const std::size_t channelCount = wav.channelSamples.size();
    const std::size_t sourceFrames = wav.channelSamples.front().size();
    std::vector<std::vector<float>> output(channelCount);

    if (targetRate <= 0.0 || std::fabs(sourceRate - targetRate) < 1e-6)
    {
        for (std::size_t c = 0; c < channelCount; ++c)
        {
            const auto& src = wav.channelSamples[std::min(c, wav.channelSamples.size() - 1)];
            output[c].resize(sourceFrames);
            for (std::size_t f = 0; f < sourceFrames; ++f)
                output[c][f] = static_cast<float>(std::clamp(src[f], -1.0, 1.0));
        }
        return output;
    }

    const double ratio = targetRate / sourceRate;
    const std::size_t destFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(sourceFrames * ratio)));
    for (std::size_t c = 0; c < channelCount; ++c)
    {
        const auto& src = wav.channelSamples[std::min(c, wav.channelSamples.size() - 1)];
        output[c].resize(destFrames);
        for (std::size_t f = 0; f < destFrames; ++f)
        {
            const double pos = (static_cast<double>(f) * sourceRate) / targetRate;
            const std::size_t i0 = std::min<std::size_t>(static_cast<std::size_t>(pos), sourceFrames - 1);
            const std::size_t i1 = std::min(i0 + 1, sourceFrames - 1);
            const double frac = std::clamp(pos - static_cast<double>(i0), 0.0, 1.0);
            output[c][f] = static_cast<float>(std::clamp(src[i0] + (src[i1] - src[i0]) * frac, -1.0, 1.0));
        }
    }
    return output;
}

} // namespace guitarfx::util
