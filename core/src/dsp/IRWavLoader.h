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
    template <typename T>
    inline bool ReadValue(std::ifstream &stream, T &value)
    {
      return static_cast<bool>(stream.read(reinterpret_cast<char *>(&value), sizeof(T)));
    }

    template <typename T>
    inline T SwapEndian(T value)
    {
      union
      {
        T value;
        std::array<std::uint8_t, sizeof(T)> bytes;
      } source{value}, result;

      for (std::size_t i = 0; i < sizeof(T); ++i)
        result.bytes[i] = source.bytes[sizeof(T) - 1 - i];

      return result.value;
    }

    template <typename T>
    inline T ReadLittleEndian(std::ifstream &stream)
    {
      T value;
      if (!ReadValue(stream, value))
        return {};

      if constexpr (std::endian::native == std::endian::big)
        value = SwapEndian(value);

      return value;
    }

    inline std::uint32_t ReadFourCC(std::ifstream &stream)
    {
      std::uint32_t value;
      if (!ReadValue(stream, value))
        return 0;
      return value;
    }

    constexpr std::uint32_t MakeFourCC(char a, char b, char c, char d)
    {
      return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
    }

    inline bool LoadWavFile(const std::filesystem::path &path, IRWavData &out)
    {
      out = {};

      std::ifstream file(path, std::ios::binary);
      if (!file)
        return false;

      if (ReadFourCC(file) != MakeFourCC('R', 'I', 'F', 'F'))
        return false;
      ReadLittleEndian<std::uint32_t>(file); // file size
      if (ReadFourCC(file) != MakeFourCC('W', 'A', 'V', 'E'))
        return false;

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
          break;

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
            ReadLittleEndian<std::uint16_t>(file); // cbSize
            ReadLittleEndian<std::uint16_t>(file); // validBitsPerSample
            ReadLittleEndian<std::uint32_t>(file); // channelMask
            audioFormat = ReadLittleEndian<std::uint16_t>(file); // SubFormat
            file.seekg(14, std::ios::cur); // Skip rest of GUID
            if (chunkSize > 40)
              file.seekg(chunkSize - 40, std::ios::cur);
          }
          else if (chunkSize > 16)
          {
            file.seekg(chunkSize - 16, std::ios::cur);
          }

          if (audioFormat != 1 && audioFormat != 3)
            return false;

          fmtLoaded = true;
        }
        else if (chunkId == MakeFourCC('d', 'a', 't', 'a'))
        {
          if (!fmtLoaded || channels == 0 || bitsPerSample == 0)
            return false;

          const std::size_t bytesPerSample = bitsPerSample / 8;
          if (bytesPerSample == 0)
            return false;

          const std::size_t totalSamples = chunkSize / bytesPerSample;
          if (totalSamples == 0)
            return false;

          std::vector<float> samples(totalSamples);

          if (bitsPerSample == 16)
          {
            for (std::size_t i = 0; i < totalSamples; ++i)
            {
              const std::int16_t value = ReadLittleEndian<std::int16_t>(file);
              samples[i] = static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int16_t>::max());
            }
          }
          else if (bitsPerSample == 24)
          {
            for (std::size_t i = 0; i < totalSamples; ++i)
            {
              std::array<std::uint8_t, 3> bytes{};
              file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
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
                samples[i] = static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::int32_t>::max());
              }
            }
          }
          else
          {
            return false;
          }

          if (!file)
            return false;

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

    inline void DownmixToMono(const IRWavData &data, std::vector<float> &mono)
    {
      mono.clear();
      if (data.samples.empty())
        return;

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
          sum += data.samples[base + channel];
        mono[frame] = sum / static_cast<float>(data.channels);
      }
    }

    inline void SplitToStereo(const IRWavData &data, std::vector<float> &left, std::vector<float> &right)
    {
      left.clear();
      right.clear();
      if (data.samples.empty())
        return;

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

    inline void SplitToQuad(const IRWavData &data, std::vector<float> &ll, std::vector<float> &lr,
                            std::vector<float> &rl, std::vector<float> &rr)
    {
      ll.clear();
      lr.clear();
      rl.clear();
      rr.clear();
      if (data.samples.empty() || data.channels < 4)
        return;

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

    inline void ResampleLinear(std::vector<float> &samples, double sourceRate, double targetRate)
    {
      if (std::abs(sourceRate - targetRate) < 1.0)
        return;

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

  } // namespace irwav
} // namespace guitarfx
