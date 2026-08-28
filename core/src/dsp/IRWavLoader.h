#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace guitarfx
{
struct IRWavData
{
    std::vector<float> samples; // Interleaved
    std::uint16_t channels = 0;
    double sampleRate = 0.0;
};

namespace irwav
{
template <typename T> inline bool ReadValue(std::ifstream& stream, T& value)
{
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

template <typename T> inline T SwapEndian(T value)
{
    union {
        T value;
        std::array<std::uint8_t, sizeof(T)> bytes;
    } source{value}, result;

    for (std::size_t i = 0; i < sizeof(T); ++i)
    {
        result.bytes[i] = source.bytes[sizeof(T) - 1 - i];
    }

    return result.value;
}

template <typename T> inline T ReadLittleEndian(std::ifstream& stream)
{
    T value;
    if (!ReadValue(stream, value))
    {
        return {};
    }

    if constexpr (std::endian::native == std::endian::big)
    {
        value = SwapEndian(value);
    }

    return value;
}

template <typename T> inline T ReadBigEndian(std::ifstream& stream)
{
    T value;
    if (!ReadValue(stream, value))
    {
        return {};
    }

    if constexpr (std::endian::native == std::endian::little)
    {
        value = SwapEndian(value);
    }

    return value;
}

inline std::uint32_t ReadFourCC(std::ifstream& stream)
{
    std::uint32_t value;
    if (!ReadValue(stream, value))
    {
        return 0;
    }
    return value;
}

constexpr std::uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

inline bool LoadWavFile(const std::filesystem::path& path, IRWavData& out)
{
    out = {};

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    if (ReadFourCC(file) != MakeFourCC('R', 'I', 'F', 'F'))
    {
        return false;
    }
    ReadLittleEndian<std::uint32_t>(file); // file size
    if (ReadFourCC(file) != MakeFourCC('W', 'A', 'V', 'E'))
    {
        return false;
    }

    std::uint16_t audioFormat = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    bool fmtLoaded = false;
    bool dataLoaded = false;

    while (file && !dataLoaded)
    {
        const std::uint32_t chunkId = ReadFourCC(file);
        const std::uint32_t chunkSize = ReadLittleEndian<std::uint32_t>(file);

        if (!file)
        {
            break;
        }

        if (chunkId == MakeFourCC('f', 'm', 't', ' '))
        {
            audioFormat = ReadLittleEndian<std::uint16_t>(file);
            channels = ReadLittleEndian<std::uint16_t>(file);
            sampleRate = ReadLittleEndian<std::uint32_t>(file);
            ReadLittleEndian<std::uint32_t>(file); // byte rate
            ReadLittleEndian<std::uint16_t>(file); // block align
            bitsPerSample = ReadLittleEndian<std::uint16_t>(file);

            if (audioFormat == 65534 && chunkSize >= 40)
            {
                ReadLittleEndian<std::uint16_t>(file);               // cbSize
                ReadLittleEndian<std::uint16_t>(file);               // validBitsPerSample
                ReadLittleEndian<std::uint32_t>(file);               // channelMask
                audioFormat = ReadLittleEndian<std::uint16_t>(file); // SubFormat
                file.seekg(14, std::ios::cur);                       // Skip rest of GUID
                if (chunkSize > 40)
                {
                    file.seekg(chunkSize - 40, std::ios::cur);
                }
            }
            else if (chunkSize > 16)
            {
                file.seekg(chunkSize - 16, std::ios::cur);
            }

            if (audioFormat != 1 && audioFormat != 3)
            {
                return false;
            }

            fmtLoaded = true;
        }
        else if (chunkId == MakeFourCC('d', 'a', 't', 'a'))
        {
            if (!fmtLoaded || channels == 0 || bitsPerSample == 0)
            {
                return false;
            }

            const std::size_t bytesPerSample = bitsPerSample / 8;
            if (bytesPerSample == 0)
            {
                return false;
            }

            const std::size_t totalSamples = chunkSize / bytesPerSample;
            if (totalSamples == 0)
            {
                return false;
            }

            std::vector<float> samples(totalSamples);

            if (bitsPerSample == 16)
            {
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    const std::int16_t value = ReadLittleEndian<std::int16_t>(file);
                    samples[i] =
                        static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int16_t>::max());
                }
            }
            else if (bitsPerSample == 24)
            {
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    std::array<std::uint8_t, 3> bytes{};
                    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                    std::int32_t value = (static_cast<std::int32_t>(bytes[2]) << 24) |
                                         (static_cast<std::int32_t>(bytes[1]) << 16) |
                                         (static_cast<std::int32_t>(bytes[0]) << 8);
                    value >>= 8;
                    samples[i] = static_cast<float>(value) / 8388608.0f;
                }
            }
            else if (bitsPerSample == 32)
            {
                if (audioFormat == 3)
                {
                    for (std::size_t i = 0; i < totalSamples; ++i)
                    {
                        const std::uint32_t raw = ReadLittleEndian<std::uint32_t>(file);
                        float asFloat;
                        std::memcpy(&asFloat, &raw, sizeof(float));
                        samples[i] = asFloat;
                    }
                }
                else
                {
                    for (std::size_t i = 0; i < totalSamples; ++i)
                    {
                        const std::int32_t value = ReadLittleEndian<std::int32_t>(file);
                        samples[i] =
                            static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int32_t>::max());
                    }
                }
            }
            else
            {
                return false;
            }

            if (!file)
            {
                return false;
            }

            out.samples = std::move(samples);
            out.channels = channels;
            out.sampleRate = static_cast<double>(sampleRate);
            dataLoaded = true;
        }
        else
        {
            file.seekg(chunkSize, std::ios::cur);
        }
    }

    return dataLoaded && out.channels > 0 && !out.samples.empty();
}

inline void DownmixToMono(const IRWavData& data, std::vector<float>& mono)
{
    mono.clear();
    if (data.samples.empty())
    {
        return;
    }

    if (data.channels <= 1)
    {
        mono = data.samples;
        return;
    }

    const std::size_t frames = data.samples.size() / data.channels;
    mono.assign(frames, 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        float sum = 0.0f;
        const std::size_t base = frame * data.channels;
        for (std::uint16_t channel = 0; channel < data.channels; ++channel)
        {
            sum += data.samples[base + channel];
        }
        mono[frame] = sum / static_cast<float>(data.channels);
    }
}

inline void SplitToStereo(const IRWavData& data, std::vector<float>& left, std::vector<float>& right)
{
    left.clear();
    right.clear();
    if (data.samples.empty())
    {
        return;
    }

    if (data.channels <= 1)
    {
        left = data.samples;
        right = data.samples;
        return;
    }

    const std::size_t frames = data.samples.size() / data.channels;
    left.resize(frames, 0.0f);
    right.resize(frames, 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const std::size_t base = frame * data.channels;
        left[frame] = data.samples[base];
        right[frame] = data.samples[base + 1];
    }
}

inline void SplitToQuad(const IRWavData& data, std::vector<float>& ll, std::vector<float>& lr, std::vector<float>& rl,
                        std::vector<float>& rr)
{
    ll.clear();
    lr.clear();
    rl.clear();
    rr.clear();
    if (data.samples.empty() || data.channels < 4)
    {
        return;
    }

    const std::size_t frames = data.samples.size() / data.channels;
    ll.resize(frames, 0.0f);
    lr.resize(frames, 0.0f);
    rl.resize(frames, 0.0f);
    rr.resize(frames, 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const std::size_t base = frame * data.channels;
        ll[frame] = data.samples[base];
        lr[frame] = data.samples[base + 1];
        rl[frame] = data.samples[base + 2];
        rr[frame] = data.samples[base + 3];
    }
}

inline void ResampleLinear(std::vector<float>& samples, double sourceRate, double targetRate)
{
    if (std::abs(sourceRate - targetRate) < 1.0)
    {
        return;
    }

    const double ratio = targetRate / sourceRate;
    const std::size_t newSize = static_cast<std::size_t>(std::ceil(samples.size() * ratio));
    std::vector<float> resampled(newSize, 0.0f);

    for (std::size_t i = 0; i < newSize; ++i)
    {
        const double sourceIndex = static_cast<double>(i) / ratio;
        const std::size_t indexA = static_cast<std::size_t>(sourceIndex);
        const std::size_t indexB = std::min(indexA + 1, samples.size() - 1);
        const double frac = sourceIndex - static_cast<double>(indexA);
        resampled[i] = static_cast<float>((1.0 - frac) * samples[indexA] + frac * samples[indexB]);
    }
    samples = std::move(resampled);
}

/**
 * Windowed-sinc polyphase resampler for IR load-time sample-rate conversion.
 * Uses a 128-tap Blackman-windowed sinc filter (~-74 dB alias rejection), which
 * eliminates the ~-13 dB stopband limitation of the linear interpolation approach.
 * Not real-time safe (allocates); call only at load time.
 */
inline void ResampleSinc(std::vector<float>& samples, double sourceRate, double targetRate)
{
    if (std::abs(sourceRate - targetRate) < 1.0)
    {
        return;
    }

    constexpr int kHalfTaps = 64; // filter spans ±64 taps → 128-tap kernel
    constexpr double kPi = 3.14159265358979323846;

    const double ratio = targetRate / sourceRate;
    const double cutoff = std::min(ratio, 1.0); // anti-alias cutoff at min(source, target) rate
    const std::size_t newSize = static_cast<std::size_t>(std::ceil(static_cast<double>(samples.size()) * ratio));
    if (newSize == 0)
    {
        samples.clear();
        return;
    }

    std::vector<float> resampled(newSize);
    const int srcLen = static_cast<int>(samples.size());

    for (std::size_t i = 0; i < newSize; ++i)
    {
        const double srcPos = static_cast<double>(i) / ratio;
        const int center = static_cast<int>(srcPos);
        const double frac = srcPos - static_cast<double>(center);

        double out = 0.0;
        double norm = 0.0;

        for (int t = -kHalfTaps; t <= kHalfTaps; ++t)
        {
            const int idx = center + t;
            if (idx < 0 || idx >= srcLen)
            {
                continue;
            }

            // Fractional sinc argument
            const double x = static_cast<double>(t) - frac;
            const double xScaled = x * cutoff;

            double sinc;
            if (std::fabs(xScaled) < 1e-10)
            {
                sinc = 1.0;
            }
            else
            {
                sinc = std::sin(kPi * xScaled) / (kPi * xScaled);
            }

            // Blackman window indexed by integer tap position: w = 0.42 - 0.5cos(2πn/N) + 0.08cos(4πn/N)
            const double normPos = static_cast<double>(t + kHalfTaps) / static_cast<double>(2 * kHalfTaps);
            const double window = 0.42 - 0.5 * std::cos(2.0 * kPi * normPos) + 0.08 * std::cos(4.0 * kPi * normPos);

            const double tap = sinc * window;
            out += static_cast<double>(samples[idx]) * tap;
            norm += tap;
        }

        resampled[i] = static_cast<float>(norm > 1e-10 ? out / norm : out);
    }

    samples = std::move(resampled);
}

// -------------------------------------------------------------------------
// Read an 80-bit IEEE 754 extended big-endian value from a stream.
// Used to decode the AIFF COMM chunk sample-rate field.
// -------------------------------------------------------------------------
inline double Read80BitExtendedFromStream(std::ifstream& stream)
{
    std::array<std::uint8_t, 10> buf{};
    if (!stream.read(reinterpret_cast<char*>(buf.data()), 10))
    {
        return 0.0;
    }

    const bool sign = (buf[0] & 0x80u) != 0u;
    const std::uint16_t biasedExp = static_cast<std::uint16_t>(((buf[0] & 0x7Fu) << 8u) | buf[1]);

    std::uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
    {
        mantissa = (mantissa << 8u) | buf[2 + i];
    }

    if (biasedExp == 0 && mantissa == 0)
    {
        return 0.0;
    }

    // value = mantissa * 2^(biasedExp - 16383 - 63)
    const int exponent = static_cast<int>(biasedExp) - 16383 - 63;
    double result = static_cast<double>(mantissa) * std::pow(2.0, exponent);
    return sign ? -result : result;
}

// -------------------------------------------------------------------------
// LoadAiffFile — loads an AIFF or AIFC file into IRWavData.
// Supports big-endian PCM (AIFF, AIFC/NONE) and byte-swapped PCM
// (AIFC/sowt).  Float variants (fl32/FL32, fl64/FL64) are accepted for
// AIFC but uncommon in IR packs.
// -------------------------------------------------------------------------
inline bool LoadAiffFile(const std::filesystem::path& path, IRWavData& out)
{
    out = {};

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    // FORM header (12 bytes)
    std::array<char, 4> formId{}, formType{};
    if (!file.read(formId.data(), 4))
    {
        return false;
    }
    if (std::strncmp(formId.data(), "FORM", 4) != 0)
    {
        return false;
    }

    ReadBigEndian<std::uint32_t>(file); // total file size (unused here)

    if (!file.read(formType.data(), 4))
    {
        return false;
    }
    const bool isAifc = std::strncmp(formType.data(), "AIFC", 4) == 0;
    if (!isAifc && std::strncmp(formType.data(), "AIFF", 4) != 0)
    {
        return false;
    }

    // FourCC helpers
    constexpr auto MakeFourCCBE = [](char a, char b, char c, char d) -> std::uint32_t {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24u) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16u) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8u) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(d));
    };
    const std::uint32_t kNONE = MakeFourCCBE('N', 'O', 'N', 'E');
    const std::uint32_t kSowt = MakeFourCCBE('s', 'o', 'w', 't');
    const std::uint32_t kFl32 = MakeFourCCBE('f', 'l', '3', '2');
    const std::uint32_t kFL32 = MakeFourCCBE('F', 'L', '3', '2');
    const std::uint32_t kFl64 = MakeFourCCBE('f', 'l', '6', '4');
    const std::uint32_t kFL64 = MakeFourCCBE('F', 'L', '6', '4');

    std::uint16_t channels = 0;
    std::uint32_t numFrames = 0;
    std::uint16_t bitsPerSample = 0;
    double sampleRate = 0.0;
    std::uint32_t comprType = kNONE;
    bool fmtLoaded = false;
    bool dataLoaded = false;

    while (file && !dataLoaded)
    {
        std::array<char, 4> chunkIdBuf{};
        if (!file.read(chunkIdBuf.data(), 4))
        {
            break;
        }
        const std::uint32_t chunkSize = ReadBigEndian<std::uint32_t>(file);
        if (!file)
        {
            break;
        }

        const std::string chunkId(chunkIdBuf.data(), 4);
        const auto chunkStart = file.tellg();

        if (chunkId == "COMM")
        {
            channels = ReadBigEndian<std::uint16_t>(file);
            numFrames = ReadBigEndian<std::uint32_t>(file);
            bitsPerSample = ReadBigEndian<std::uint16_t>(file);
            sampleRate = Read80BitExtendedFromStream(file);

            if (isAifc)
            {
                comprType = ReadBigEndian<std::uint32_t>(file);
            }
            else
            {
                comprType = kNONE;
            }

            fmtLoaded = true;
        }
        else if (chunkId == "SSND")
        {
            if (!fmtLoaded || channels == 0 || bitsPerSample == 0)
            {
                return false;
            }

            const bool isBigEndianPcm = (comprType == kNONE);
            const bool isLittleEndian = (comprType == kSowt);
            const bool isFloat32 = (comprType == kFl32 || comprType == kFL32);
            const bool isFloat64 = (comprType == kFl64 || comprType == kFL64);

            if (!isBigEndianPcm && !isLittleEndian && !isFloat32 && !isFloat64)
            {
                return false;
            }
            if (isLittleEndian && bitsPerSample != 16)
            {
                return false;
            }
            if (isFloat32 && bitsPerSample != 32)
            {
                return false;
            }
            if (isFloat64 && bitsPerSample != 64)
            {
                return false;
            }

            ReadBigEndian<std::uint32_t>(file); // dataOffset (unused for uncompressed)
            ReadBigEndian<std::uint32_t>(file); // blockSize  (unused for uncompressed)

            const std::size_t bytesPerSample = static_cast<std::size_t>(bitsPerSample) / 8u;
            if (bytesPerSample == 0)
            {
                return false;
            }

            const std::size_t totalSamples = static_cast<std::size_t>(numFrames) * static_cast<std::size_t>(channels);
            if (totalSamples == 0)
            {
                return false;
            }

            std::vector<float> samples(totalSamples);

            if (isFloat32)
            {
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    const std::uint32_t raw = ReadBigEndian<std::uint32_t>(file);
                    float v;
                    std::memcpy(&v, &raw, sizeof(float));
                    samples[i] = v;
                }
            }
            else if (isFloat64)
            {
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    std::array<std::uint8_t, 8> buf{};
                    file.read(reinterpret_cast<char*>(buf.data()), 8);
                    // Big-endian 64-bit float — swap bytes on little-endian host
                    if constexpr (std::endian::native == std::endian::little)
                    {
                        std::reverse(buf.begin(), buf.end());
                    }
                    double d;
                    std::memcpy(&d, buf.data(), 8);
                    samples[i] = static_cast<float>(d);
                }
            }
            else if (isLittleEndian)
            {
                // sowt: 16-bit little-endian signed
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    const std::int16_t value = ReadLittleEndian<std::int16_t>(file);
                    samples[i] =
                        static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int16_t>::max());
                }
            }
            else if (bitsPerSample == 16)
            {
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    const std::int16_t value = ReadBigEndian<std::int16_t>(file);
                    samples[i] =
                        static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int16_t>::max());
                }
            }
            else if (bitsPerSample == 24)
            {
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    std::array<std::uint8_t, 3> buf{};
                    file.read(reinterpret_cast<char*>(buf.data()), 3);
                    // Big-endian 24-bit signed integer
                    std::int32_t value = (static_cast<std::int32_t>(buf[0]) << 24) |
                                         (static_cast<std::int32_t>(buf[1]) << 16) |
                                         (static_cast<std::int32_t>(buf[2]) << 8);
                    value >>= 8; // arithmetic right-shift to sign-extend
                    samples[i] = static_cast<float>(value) / 8388608.0f;
                }
            }
            else if (bitsPerSample == 32)
            {
                for (std::size_t i = 0; i < totalSamples; ++i)
                {
                    const std::int32_t value = ReadBigEndian<std::int32_t>(file);
                    samples[i] =
                        static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int32_t>::max());
                }
            }
            else
            {
                return false;
            }

            if (!file)
            {
                return false;
            }

            out.samples = std::move(samples);
            out.channels = channels;
            out.sampleRate = sampleRate;
            dataLoaded = true;
        }
        else
        {
            // Skip unknown chunk; AIFF chunks are word-aligned
            const std::streamoff skip = static_cast<std::streamoff>(chunkSize) + (chunkSize & 1u ? 1 : 0);
            file.seekg(static_cast<std::streamoff>(chunkStart) + skip);
        }
    }

    return dataLoaded && out.channels > 0 && !out.samples.empty();
}

// -------------------------------------------------------------------------
// LoadAudioFile — dispatches to LoadWavFile or LoadAiffFile based on the
// file extension.  Use this in place of LoadWavFile where AIFF support
// is desired (IR cab/reverb loading).
// -------------------------------------------------------------------------
inline bool LoadAudioFile(const std::filesystem::path& path, IRWavData& out)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".aif" || ext == ".aiff")
    {
        return LoadAiffFile(path, out);
    }

    return LoadWavFile(path, out);
}

} // namespace irwav
} // namespace guitarfx
