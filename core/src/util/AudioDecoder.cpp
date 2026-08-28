#include "AudioDecoder.h"
#include "Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// minimp3 — define implementation exactly once in this translation unit.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include <minimp3_ex.h>

namespace
{
// -------------------------------------------------------------------------
// AIFF big-endian read helpers
// -------------------------------------------------------------------------

std::uint16_t ReadU16BE(const std::uint8_t* d)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(d[0]) << 8u) | d[1]);
}

std::uint32_t ReadU32BE(const std::uint8_t* d)
{
    return (static_cast<std::uint32_t>(d[0]) << 24u) | (static_cast<std::uint32_t>(d[1]) << 16u) |
           (static_cast<std::uint32_t>(d[2]) << 8u) | static_cast<std::uint32_t>(d[3]);
}

std::int16_t ReadS16BE(const std::uint8_t* d)
{
    return static_cast<std::int16_t>(ReadU16BE(d));
}

std::int32_t ReadS32BE(const std::uint8_t* d)
{
    return static_cast<std::int32_t>(ReadU32BE(d));
}

// Decode an 80-bit IEEE 754 extended big-endian value (AIFF COMM sampleRate).
double Read80BitExtended(const std::uint8_t* d)
{
    const bool sign = (d[0] & 0x80u) != 0u;
    const std::uint16_t biasedExp = static_cast<std::uint16_t>(((d[0] & 0x7Fu) << 8u) | d[1]);

    std::uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
    {
        mantissa = (mantissa << 8u) | d[2 + i];
    }

    if (biasedExp == 0 && mantissa == 0)
    {
        return 0.0;
    }

    // The 80-bit format has an explicit integer bit.
    // value = mantissa * 2^(biasedExp - 16383 - 63)
    const int exponent = static_cast<int>(biasedExp) - 16383 - 63;
    double result = static_cast<double>(mantissa) * std::pow(2.0, exponent);
    return sign ? -result : result;
}

} // namespace

namespace guitarfx::util
{

// -----------------------------------------------------------------------------
// AIFF / AIFC decoder
// -----------------------------------------------------------------------------

std::optional<DecodedWav> DecodePcmAiff(const std::vector<std::uint8_t>& bytes)
{
    // Minimum: 4 (FORM) + 4 (size) + 4 (AIFF/AIFC)
    if (bytes.size() < 12)
    {
        return std::nullopt;
    }

    if (std::memcmp(bytes.data(), "FORM", 4) != 0)
    {
        return std::nullopt;
    }

    const bool isAifc = std::memcmp(bytes.data() + 8, "AIFC", 4) == 0;
    if (!isAifc && std::memcmp(bytes.data() + 8, "AIFF", 4) != 0)
    {
        return std::nullopt;
    }

    // AIFC compression-type FourCCs
    constexpr std::uint32_t kNONE = (static_cast<std::uint32_t>('N') << 24u) |
                                    (static_cast<std::uint32_t>('O') << 16u) | (static_cast<std::uint32_t>('N') << 8u) |
                                    static_cast<std::uint32_t>('E');
    constexpr std::uint32_t kSowt = (static_cast<std::uint32_t>('s') << 24u) |
                                    (static_cast<std::uint32_t>('o') << 16u) | (static_cast<std::uint32_t>('w') << 8u) |
                                    static_cast<std::uint32_t>('t');
    constexpr std::uint32_t kFl32 = (static_cast<std::uint32_t>('f') << 24u) |
                                    (static_cast<std::uint32_t>('l') << 16u) | (static_cast<std::uint32_t>('3') << 8u) |
                                    static_cast<std::uint32_t>('2');
    constexpr std::uint32_t kFL32 = (static_cast<std::uint32_t>('F') << 24u) |
                                    (static_cast<std::uint32_t>('L') << 16u) | (static_cast<std::uint32_t>('3') << 8u) |
                                    static_cast<std::uint32_t>('2');
    constexpr std::uint32_t kFl64 = (static_cast<std::uint32_t>('f') << 24u) |
                                    (static_cast<std::uint32_t>('l') << 16u) | (static_cast<std::uint32_t>('6') << 8u) |
                                    static_cast<std::uint32_t>('4');
    constexpr std::uint32_t kFL64 = (static_cast<std::uint32_t>('F') << 24u) |
                                    (static_cast<std::uint32_t>('L') << 16u) | (static_cast<std::uint32_t>('6') << 8u) |
                                    static_cast<std::uint32_t>('4');

    std::uint16_t numChannels = 0;
    std::uint32_t numFrames = 0;
    std::uint16_t sampleSize = 0;
    double sampleRate = 0.0;
    std::uint32_t comprType = 0;
    bool commFound = false;

    std::size_t ssndBodyStart = 0; // byte offset of first sample in bytes[]
    std::uint32_t ssndDataSize = 0;
    bool ssndFound = false;

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size())
    {
        const char* idPtr = reinterpret_cast<const char*>(bytes.data() + offset);
        const std::string chunkId(idPtr, idPtr + 4);
        const std::uint32_t chunkSize = ReadU32BE(bytes.data() + offset + 4);
        const std::size_t chunkDataOff = offset + 8;

        if (chunkDataOff + chunkSize > bytes.size())
        {
            break;
        }

        if (chunkId == "COMM")
        {
            if (chunkSize < 18)
            {
                return std::nullopt;
            }
            numChannels = ReadU16BE(bytes.data() + chunkDataOff);
            numFrames = ReadU32BE(bytes.data() + chunkDataOff + 2);
            sampleSize = ReadU16BE(bytes.data() + chunkDataOff + 6);
            sampleRate = Read80BitExtended(bytes.data() + chunkDataOff + 8);

            if (isAifc)
            {
                if (chunkSize < 22)
                {
                    return std::nullopt;
                }
                comprType = ReadU32BE(bytes.data() + chunkDataOff + 18);
            }
            else
            {
                comprType = kNONE; // plain AIFF is always big-endian PCM
            }
            commFound = true;
        }
        else if (chunkId == "SSND")
        {
            if (chunkSize < 8)
            {
                return std::nullopt;
            }
            const std::uint32_t dataOffset = ReadU32BE(bytes.data() + chunkDataOff);
            // blockSize word (chunkDataOff+4) is unused for uncompressed audio
            ssndBodyStart = chunkDataOff + 8 + dataOffset;
            ssndDataSize = chunkSize - 8 - dataOffset;
            ssndFound = true;
        }

        // AIFF chunks are word-aligned (padded to even byte boundary)
        offset = chunkDataOff + chunkSize + (chunkSize & 1u);
    }

    if (!commFound || !ssndFound)
    {
        return std::nullopt;
    }
    if (numChannels == 0 || numFrames == 0 || sampleSize == 0)
    {
        return std::nullopt;
    }
    if (sampleRate <= 0.0)
    {
        return std::nullopt;
    }

    // Validate compression type
    const bool isBigEndianPcm = (comprType == kNONE);
    const bool isLittleEndian = (comprType == kSowt);
    const bool isFloat32 = (comprType == kFl32 || comprType == kFL32);
    const bool isFloat64 = (comprType == kFl64 || comprType == kFL64);

    if (!isBigEndianPcm && !isLittleEndian && !isFloat32 && !isFloat64)
    {
        return std::nullopt;
    }

    // Sanity-check declared bit-depth vs compression type
    if (isLittleEndian && sampleSize != 16)
    {
        return std::nullopt;
    }
    if (isFloat32 && sampleSize != 32)
    {
        return std::nullopt;
    }
    if (isFloat64 && sampleSize != 64)
    {
        return std::nullopt;
    }

    const std::size_t bytesPerSample = static_cast<std::size_t>(sampleSize) / 8u;
    if (bytesPerSample == 0)
    {
        return std::nullopt;
    }

    const std::size_t blockAlign = bytesPerSample * numChannels;
    const std::size_t totalBytes = static_cast<std::size_t>(numFrames) * blockAlign;

    if (ssndBodyStart + totalBytes > bytes.size())
    {
        return std::nullopt;
    }

    DecodedWav wav;
    wav.sampleRate = sampleRate;
    wav.channels = static_cast<int>(numChannels);
    wav.bitsPerSample = static_cast<int>(sampleSize);
    wav.channelSamples.assign(numChannels, std::vector<double>(numFrames, 0.0));

    const std::uint8_t* pcm = bytes.data() + ssndBodyStart;

    for (std::size_t frame = 0; frame < numFrames; ++frame)
    {
        const std::size_t frameBase = frame * blockAlign;
        for (std::uint16_t ch = 0; ch < numChannels; ++ch)
        {
            const std::uint8_t* s = pcm + frameBase + ch * bytesPerSample;
            double sample = 0.0;

            if (isFloat32)
            {
                float v;
                std::memcpy(&v, s, 4);
                sample = static_cast<double>(v);
            }
            else if (isFloat64)
            {
                std::memcpy(&sample, s, 8);
            }
            else if (isLittleEndian)
            {
                // sowt: little-endian signed 16-bit
                const std::int16_t v = static_cast<std::int16_t>(static_cast<std::uint16_t>(s[0]) |
                                                                 (static_cast<std::uint16_t>(s[1]) << 8u));
                sample = static_cast<double>(v) / 32768.0;
            }
            else
            {
                // Big-endian signed PCM
                switch (sampleSize)
                {
                case 8:
                    sample = static_cast<double>(static_cast<std::int8_t>(s[0])) / 128.0;
                    break;
                case 16:
                    sample = static_cast<double>(ReadS16BE(s)) / 32768.0;
                    break;
                case 24: {
                    std::int32_t v = (static_cast<std::int32_t>(s[0]) << 16) | (static_cast<std::int32_t>(s[1]) << 8) |
                                     static_cast<std::int32_t>(s[2]);
                    if (v & 0x800000)
                    {
                        v |= ~0xFFFFFF; // sign-extend
                    }
                    sample = static_cast<double>(v) / 8388608.0;
                    break;
                }
                case 32:
                    sample = static_cast<double>(ReadS32BE(s)) / 2147483648.0;
                    break;
                default:
                    return std::nullopt;
                }
            }

            wav.channelSamples[ch][frame] = std::clamp(sample, -1.0, 1.0);
        }
    }

    return wav;
}

// -----------------------------------------------------------------------------
// MP3 decoder (minimp3)
// -----------------------------------------------------------------------------

std::optional<DecodedWav> DecodeMp3(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 4)
    {
        return std::nullopt;
    }

    mp3dec_t dec;
    mp3dec_init(&dec);

    mp3dec_file_info_t info{};
    const int result = mp3dec_load_buf(&dec, bytes.data(), bytes.size(), &info, nullptr, nullptr);

    if (result != 0 || info.buffer == nullptr || info.samples == 0 || info.channels == 0)
    {
        if (info.buffer)
        {
            free(info.buffer); // NOLINT(*-no-malloc)
        }
        return std::nullopt;
    }

    DecodedWav wav;
    wav.sampleRate = static_cast<double>(info.hz);
    wav.channels = info.channels;
    wav.bitsPerSample = 32; // MINIMP3_FLOAT_OUTPUT produces 32-bit floats

    const std::size_t framesPerChannel = info.samples / static_cast<std::size_t>(info.channels);
    wav.channelSamples.assign(static_cast<std::size_t>(info.channels), std::vector<double>(framesPerChannel, 0.0));

    for (std::size_t frame = 0; frame < framesPerChannel; ++frame)
    {
        for (int ch = 0; ch < info.channels; ++ch)
        {
            const float s = info.buffer[frame * static_cast<std::size_t>(info.channels) + static_cast<std::size_t>(ch)];
            wav.channelSamples[static_cast<std::size_t>(ch)][frame] = std::clamp(static_cast<double>(s), -1.0, 1.0);
        }
    }

    free(info.buffer); // NOLINT(*-no-malloc)
    return wav;
}

// -----------------------------------------------------------------------------
// Format-dispatching decoder
// -----------------------------------------------------------------------------

std::optional<DecodedWav> DecodeAudioBytes(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 4)
    {
        return std::nullopt;
    }

    // WAV: "RIFF" magic
    if (bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F')
    {
        return DecodePcmWav(bytes);
    }

    // AIFF / AIFC: "FORM" magic
    if (bytes[0] == 'F' && bytes[1] == 'O' && bytes[2] == 'R' && bytes[3] == 'M')
    {
        return DecodePcmAiff(bytes);
    }

    // MP3 with ID3 tag
    if (bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3')
    {
        return DecodeMp3(bytes);
    }

    // MP3 without ID3: MPEG sync word (0xFF 0xEx)
    if (bytes[0] == 0xFFu && (bytes[1] & 0xE0u) == 0xE0u)
    {
        return DecodeMp3(bytes);
    }

    return std::nullopt;
}

} // namespace guitarfx::util
