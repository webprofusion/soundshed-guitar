#include "IRManager.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>

namespace namguitar
{
  namespace
  {
    // FourCC codes in WAV files are stored as 4 ASCII characters in sequence.
    // On a little-endian machine, reading these 4 bytes as a uint32_t results
    // in the first character being in the low byte.
    constexpr std::uint32_t MakeFourCC(char a, char b, char c, char d)
    {
      return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
    }

    template <typename T>
    bool ReadValue(std::ifstream &stream, T &value)
    {
      return static_cast<bool>(stream.read(reinterpret_cast<char *>(&value), sizeof(T)));
    }

    // Read a FourCC code as raw bytes (no endianness conversion)
    std::uint32_t ReadFourCC(std::ifstream &stream)
    {
      std::uint32_t value;
      if (!ReadValue(stream, value))
      {
        return 0;
      }
      return value;
    }

    template <typename T>
    T SwapEndian(T value)
    {
      union
      {
        T value;
        std::array<std::uint8_t, sizeof(T)> bytes;
      } source{value}, result;

      for (std::size_t i = 0; i < sizeof(T); ++i)
      {
        result.bytes[i] = source.bytes[sizeof(T) - 1 - i];
      }

      return result.value;
    }

    template <typename T>
    T ReadLittleEndian(std::ifstream &stream)
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

    void DownmixToMono(std::vector<float> &samples, std::uint16_t channels)
    {
      if (channels <= 1)
      {
        return;
      }

      const std::size_t frames = samples.size() / channels;
      std::vector<float> mono(frames, 0.0f);
      for (std::size_t frame = 0; frame < frames; ++frame)
      {
        float sum = 0.0f;
        for (std::uint16_t channel = 0; channel < channels; ++channel)
        {
          sum += samples[frame * channels + channel];
        }
        mono[frame] = sum / static_cast<float>(channels);
      }

      samples = std::move(mono);
    }

    void ResampleLinear(std::vector<float> &samples, double sourceRate, double targetRate)
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

  } // namespace

  bool IRManager::LoadImpulseResponse(const std::filesystem::path &filePath, double targetSampleRate)
  {
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream)
    {
      return false;
    }

    if (!ParseWavFile(stream, targetSampleRate))
    {
      return false;
    }

    mCurrentIR = filePath;
    return true;
  }

  std::optional<std::filesystem::path> IRManager::CurrentImpulseResponse() const
  {
    return mCurrentIR;
  }

  const std::vector<float> &IRManager::Impulse() const noexcept
  {
    return mImpulse;
  }

  bool IRManager::HasImpulse() const noexcept
  {
    return !mImpulse.empty();
  }

  bool IRManager::ParseWavFile(std::ifstream &stream, double targetSampleRate)
  {
    if (!ParseRiffHeader(stream))
    {
      return false;
    }

    std::uint16_t audioFormat = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t blockAlign = 0;
    bool dataLoaded = false;

    while (stream && !dataLoaded)
    {
      const std::uint32_t chunkId = ReadFourCC(stream);
      const std::uint32_t chunkSize = ReadLittleEndian<std::uint32_t>(stream);

      if (!stream)
      {
        break;
      }

      switch (chunkId)
      {
      case MakeFourCC('f', 'm', 't', ' '):
        if (!ParseFmtChunk(stream, audioFormat, channels, sampleRate, bitsPerSample, blockAlign, chunkSize))
        {
          return false;
        }
        break;
      case MakeFourCC('d', 'a', 't', 'a'):
        dataLoaded = ParseDataChunk(stream, bitsPerSample, channels, chunkSize, targetSampleRate, sampleRate);
        break;
      default:
        stream.seekg(chunkSize, std::ios::cur);
        break;
      }
    }

    return dataLoaded;
  }

  bool IRManager::ParseRiffHeader(std::ifstream &stream)
  {
    const auto riff = ReadFourCC(stream);
    const auto fileSize = ReadLittleEndian<std::uint32_t>(stream);
    const auto wave = ReadFourCC(stream);

    (void)fileSize;

    return riff == MakeFourCC('R', 'I', 'F', 'F') && wave == MakeFourCC('W', 'A', 'V', 'E');
  }

  bool IRManager::ParseFmtChunk(std::ifstream &stream, std::uint16_t &audioFormat, std::uint16_t &channels,
                                std::uint32_t &sampleRate, std::uint16_t &bitsPerSample, std::uint16_t &blockAlign,
                                std::uint32_t chunkSize)
  {
    audioFormat = ReadLittleEndian<std::uint16_t>(stream);
    channels = ReadLittleEndian<std::uint16_t>(stream);
    sampleRate = ReadLittleEndian<std::uint32_t>(stream);
    const auto byteRate = ReadLittleEndian<std::uint32_t>(stream);
    blockAlign = ReadLittleEndian<std::uint16_t>(stream);
    bitsPerSample = ReadLittleEndian<std::uint16_t>(stream);

    if (chunkSize > 16)
    {
      stream.seekg(chunkSize - 16, std::ios::cur);
    }

    (void)byteRate;

    return audioFormat == 1 || audioFormat == 3; // PCM or IEEE float
  }

  bool IRManager::ParseDataChunk(std::ifstream &stream, std::uint16_t bitsPerSample, std::uint16_t channels,
                                 std::uint32_t dataSize, double targetSampleRate, std::uint32_t sourceSampleRate)
  {
    if (channels == 0)
    {
      return false;
    }

    const std::size_t totalSamples = dataSize / (bitsPerSample / 8);
    std::vector<float> samples(totalSamples);

    if (bitsPerSample == 16)
    {
      for (std::size_t i = 0; i < totalSamples; ++i)
      {
        const std::int16_t value = ReadLittleEndian<std::int16_t>(stream);
        samples[i] = static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int16_t>::max());
      }
    }
    else if (bitsPerSample == 24)
    {
      for (std::size_t i = 0; i < totalSamples; ++i)
      {
        std::array<std::uint8_t, 3> bytes{};
        stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        std::int32_t value = (static_cast<std::int32_t>(bytes[2]) << 24) | (static_cast<std::int32_t>(bytes[1]) << 16) | (static_cast<std::int32_t>(bytes[0]) << 8);
        value >>= 8;                                         // align to 24 bits signed
        samples[i] = static_cast<float>(value) / 8388608.0f; // 2^23
      }
    }
    else if (bitsPerSample == 32)
    {
      for (std::size_t i = 0; i < totalSamples; ++i)
      {
        const std::uint32_t raw = ReadLittleEndian<std::uint32_t>(stream);
        float asFloat;
        std::memcpy(&asFloat, &raw, sizeof(float));
        samples[i] = asFloat;
      }
    }
    else
    {
      stream.seekg(dataSize, std::ios::cur);
      return false;
    }

    DownmixToMono(samples, channels);
    ResampleLinear(samples, static_cast<double>(sourceSampleRate), targetSampleRate);

    mImpulse = std::move(samples);
    mImpulseSampleRate = targetSampleRate;
    return true;
  }

} // namespace namguitar
