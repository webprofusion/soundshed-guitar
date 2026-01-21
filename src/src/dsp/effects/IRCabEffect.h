#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/IRTypes.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace guitarfx
{
  /**
   * Impulse Response cabinet simulation effect.
   * Uses convolution to apply a cabinet IR to the signal.
   * Supports multi-channel WAV files, WAVE_FORMAT_EXTENSIBLE, and quality-based truncation.
   */
  class IRCabEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      UpdateAirCoefficients();
      ResetAirState();

      mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferR.resize(static_cast<size_t>(maxBlockSize));

      // Reinitialize convolvers if we have an IR loaded
      if (!mImpulse.empty())
      {
        InitializeConvolvers();
      }
    }

    void Reset() override
    {
      mConvolverL.Reset();
      mConvolverR.Reset();
      ResetAirState();
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!mEnabled || !mConvolverL.IsInitialized())
      {
        // Bypass
        if (outputs[0] && inputs[0])
          std::copy_n(inputs[0], numSamples, outputs[0]);
        if (outputs[1] && inputs[1])
          std::copy_n(inputs[1], numSamples, outputs[1]);
        return;
      }

      // Copy to double buffers
      for (int i = 0; i < numSamples; ++i)
      {
        mInputBufferL[i] = inputs[0] ? inputs[0][i] : 0.0;
        mInputBufferR[i] = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0);
      }

      // Process through convolvers
      mConvolverL.Process(mInputBufferL.data(), mOutputBufferL.data(), numSamples);
      mConvolverR.Process(mInputBufferR.data(), mOutputBufferR.data(), numSamples);

      // Apply Air EQ (post-convolution, pre-mix)
      if (mAirActive)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          mOutputBufferL[i] = ProcessAirSample(mOutputBufferL[i], 0);
          mOutputBufferR[i] = ProcessAirSample(mOutputBufferR[i], 1);
        }
      }

      // Apply wet/dry mix and output gain
      float wetGain = static_cast<float>(mMix * mOutputGain);
      float dryGain = static_cast<float>((1.0 - mMix));

      for (int i = 0; i < numSamples; ++i)
      {
        float dryL = inputs[0] ? inputs[0][i] : 0.0f;
        float dryR = inputs[1] ? inputs[1][i] : dryL;

        if (outputs[0])
          outputs[0][i] = static_cast<float>(mOutputBufferL[i]) * wetGain + dryL * dryGain;
        if (outputs[1])
          outputs[1][i] = static_cast<float>(mOutputBufferR[i]) * wetGain + dryR * dryGain;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "mix")
        mMix = std::clamp(value, 0.0, 1.0);
      else if (key == "outputGain")
        mOutputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      else if (key == "enabled")
        mEnabled = value > 0.5;
      else if (key == "quality")
      {
        int q = static_cast<int>(std::clamp(value, 0.0, 3.0));
        mQuality = static_cast<IRQuality>(q);
        // Reinitialize convolvers with new quality setting
        if (!mImpulse.empty())
          InitializeConvolvers();
      }
      else if (key == "air")
      {
        mAir = std::clamp(value, 0.0, 1.0);
        UpdateAirCoefficients();
      }
      else if (key == "airMode")
      {
        const int mode = static_cast<int>(std::clamp(value, 0.0, 2.0));
        mAirMode = static_cast<AirMode>(mode);
        ResetAirState();
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "mix")
        return mMix;
      if (key == "outputGain")
        return 20.0 * std::log10(mOutputGain);
      if (key == "enabled")
        return mEnabled ? 1.0 : 0.0;
      if (key == "quality")
        return static_cast<double>(mQuality);
      if (key == "air")
        return mAir;
      if (key == "airMode")
        return static_cast<double>(mAirMode);
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path &resourcePath) override
    {
      // Load WAV file
      if (!LoadWavFile(resourcePath))
        return false;

      mIRPath = resourcePath;
      return InitializeConvolvers();
    }

    [[nodiscard]] bool HasResource() const override { return mConvolverL.IsInitialized(); }
    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mIRPath; }

    [[nodiscard]] std::string GetType() const override { return "cab_ir"; }
    [[nodiscard]] std::string GetCategory() const override { return "cab"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    // WAV parsing utilities
    template <typename T>
    static bool ReadValue(std::ifstream &stream, T &value)
    {
      return static_cast<bool>(stream.read(reinterpret_cast<char *>(&value), sizeof(T)));
    }

    template <typename T>
    static T SwapEndian(T value)
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
    static T ReadLittleEndian(std::ifstream &stream)
    {
      T value;
      if (!ReadValue(stream, value))
        return {};

      if constexpr (std::endian::native == std::endian::big)
        value = SwapEndian(value);

      return value;
    }

    static std::uint32_t ReadFourCC(std::ifstream &stream)
    {
      std::uint32_t value;
      if (!ReadValue(stream, value))
        return 0;
      return value;
    }

    static constexpr std::uint32_t MakeFourCC(char a, char b, char c, char d)
    {
      return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
    }

    static void DownmixToMono(std::vector<float> &samples, std::uint16_t channels)
    {
      if (channels <= 1)
        return;

      const std::size_t frames = samples.size() / channels;
      std::vector<float> mono(frames, 0.0f);
      for (std::size_t frame = 0; frame < frames; ++frame)
      {
        float sum = 0.0f;
        for (std::uint16_t channel = 0; channel < channels; ++channel)
          sum += samples[frame * channels + channel];
        mono[frame] = sum / static_cast<float>(channels);
      }
      samples = std::move(mono);
    }

    static void ResampleLinear(std::vector<float> &samples, double sourceRate, double targetRate)
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

    // Find optimal truncation point based on energy threshold
    static size_t FindEnergyTruncationPoint(const std::vector<float> &samples, float threshold = 0.001f)
    {
      if (samples.empty())
        return 0;

      double totalEnergy = 0.0;
      for (const float s : samples)
        totalEnergy += static_cast<double>(s) * static_cast<double>(s);

      if (totalEnergy < 1e-10)
        return samples.size();

      const double targetEnergy = totalEnergy * (1.0 - static_cast<double>(threshold));
      double cumulativeEnergy = 0.0;

      for (size_t i = 0; i < samples.size(); ++i)
      {
        cumulativeEnergy += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
        if (cumulativeEnergy >= targetEnergy)
          return std::min(i + 256, samples.size()); // Add small buffer after threshold
      }
      return samples.size();
    }

    // Get processed (potentially truncated) IR based on quality setting
    std::vector<float> GetProcessedImpulse() const
    {
      if (mImpulse.empty())
        return {};

      // For Full quality, return the complete IR
      if (mQuality == IRQuality::Full)
        return mImpulse;

      const size_t maxSamples = GetMaxIRSamples(mQuality, mSampleRate);
      if (maxSamples == 0 || mImpulse.size() <= maxSamples)
        return mImpulse;

      // Find smart truncation point based on energy
      const size_t energyTruncPoint = FindEnergyTruncationPoint(mImpulse, 0.001f);
      const size_t truncLength = std::min({mImpulse.size(), maxSamples, energyTruncPoint});

      // Apply fade-out to avoid clicks (last 64 samples)
      std::vector<float> truncated(mImpulse.begin(), mImpulse.begin() + truncLength);

      constexpr size_t kFadeLength = 64;
      if (truncLength > kFadeLength)
      {
        for (size_t i = 0; i < kFadeLength; ++i)
        {
          const float fadeGain = static_cast<float>(kFadeLength - 1 - i) / static_cast<float>(kFadeLength - 1);
          truncated[truncLength - kFadeLength + i] *= fadeGain;
        }
      }
      return truncated;
    }

    bool LoadWavFile(const std::filesystem::path &path)
    {
      std::ifstream file(path, std::ios::binary);
      if (!file)
        return false;

      // Parse RIFF header
      if (ReadFourCC(file) != MakeFourCC('R', 'I', 'F', 'F'))
        return false;
      ReadLittleEndian<std::uint32_t>(file); // file size
      if (ReadFourCC(file) != MakeFourCC('W', 'A', 'V', 'E'))
        return false;

      std::uint16_t audioFormat = 0;
      std::uint16_t channels = 0;
      std::uint32_t sampleRate = 0;
      std::uint16_t bitsPerSample = 0;
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

          // Handle WAVE_FORMAT_EXTENSIBLE (0xFFFE)
          if (audioFormat == 65534 && chunkSize >= 40)
          {
            ReadLittleEndian<std::uint16_t>(file); // cbSize
            ReadLittleEndian<std::uint16_t>(file); // validBitsPerSample
            ReadLittleEndian<std::uint32_t>(file); // channelMask
            audioFormat = ReadLittleEndian<std::uint16_t>(file); // SubFormat (actual format)
            file.seekg(14, std::ios::cur); // Skip rest of GUID
            if (chunkSize > 40)
              file.seekg(chunkSize - 40, std::ios::cur);
          }
          else if (chunkSize > 16)
          {
            file.seekg(chunkSize - 16, std::ios::cur);
          }

          if (audioFormat != 1 && audioFormat != 3) // PCM or IEEE float
            return false;

          mIRSampleRate = static_cast<double>(sampleRate);
        }
        else if (chunkId == MakeFourCC('d', 'a', 't', 'a'))
        {
          if (channels == 0 || bitsPerSample == 0)
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
            if (audioFormat == 3) // IEEE float
            {
              for (std::size_t i = 0; i < totalSamples; ++i)
              {
                const std::uint32_t raw = ReadLittleEndian<std::uint32_t>(file);
                float asFloat;
                std::memcpy(&asFloat, &raw, sizeof(float));
                samples[i] = asFloat;
              }
            }
            else // 32-bit PCM
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

          // Downmix multi-channel to mono
          DownmixToMono(samples, channels);

          mImpulse = std::move(samples);
          dataLoaded = true;
        }
        else
        {
          file.seekg(chunkSize, std::ios::cur);
        }
      }

      return dataLoaded;
    }

    bool InitializeConvolvers()
    {
      if (mImpulse.empty() || mMaxBlockSize == 0)
        return false;

      // Get processed (potentially truncated) IR
      std::vector<float> processedIR = GetProcessedImpulse();

      // Resample IR if needed
      if (std::abs(mIRSampleRate - mSampleRate) > 1.0)
        ResampleLinear(processedIR, mIRSampleRate, mSampleRate);

      // Initialize both convolvers with the same IR
      if (!mConvolverL.SetImpulse(processedIR, mMaxBlockSize))
        return false;
      if (!mConvolverR.SetImpulse(processedIR, mMaxBlockSize))
        return false;

      return true;
    }

    enum class AirMode
    {
      OptionA_Shelf,
      OptionB_Peak,
      OptionC_ShelfPlusPeak,
    };

    static double ProcessBiquad(double input, double b0, double b1, double b2,
                                double a1, double a2, double &s1, double &s2)
    {
      const double output = b0 * input + s1;
      s1 = b1 * input - a1 * output + s2;
      s2 = b2 * input - a2 * output;
      return output;
    }

    void ComputeHighShelf(double freq, double slope, double gainDb,
                          double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double sqrtA = std::sqrt(A);
      const double alpha = sinw0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);

      const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
      b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
      b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
      a2 = ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    }

    void ComputePeakingEQ(double freq, double Q, double gainDb,
                          double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * Q);

      const double a0 = 1.0 + alpha / A;
      b0 = (1.0 + alpha * A) / a0;
      b1 = (-2.0 * cosw0) / a0;
      b2 = (1.0 - alpha * A) / a0;
      a1 = (-2.0 * cosw0) / a0;
      a2 = (1.0 - alpha / A) / a0;
    }

    void UpdateAirCoefficients()
    {
      if (mSampleRate <= 0.0)
      {
        mAirActive = false;
        return;
      }

      const double clamped = std::clamp(mAir, 0.0, 1.0);
      if (clamped <= 0.0001)
      {
        mAirActive = false;
        return;
      }

      // Option A (default): subtle high-shelf lift in the air band.
      const double shelfGainDb = clamped * 6.0; // 0..+6 dB
      const double shelfFreq = 9000.0;
      const double shelfSlope = 0.7;
      ComputeHighShelf(shelfFreq, shelfSlope, shelfGainDb, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2);

      // Option B (optional): high-mid presence peak.
      const double peakGainDb = clamped * 4.0; // 0..+4 dB
      const double peakFreq = 4500.0;
      const double peakQ = 1.2;
      ComputePeakingEQ(peakFreq, peakQ, peakGainDb, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2);

      // Option C (optional): shelf + peak combined (handled in ProcessAirSample).
      mAirActive = true;
    }

    void ResetAirState()
    {
      for (int ch = 0; ch < 2; ++ch)
      {
        mAirShelfS1[ch] = mAirShelfS2[ch] = 0.0;
        mAirPeakS1[ch] = mAirPeakS2[ch] = 0.0;
      }
    }

    double ProcessAirSample(double input, int channel)
    {
      switch (mAirMode)
      {
      case AirMode::OptionA_Shelf:
        return ProcessBiquad(input, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2,
                             mAirShelfS1[channel], mAirShelfS2[channel]);
      case AirMode::OptionB_Peak:
        return ProcessBiquad(input, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2,
                             mAirPeakS1[channel], mAirPeakS2[channel]);
      case AirMode::OptionC_ShelfPlusPeak:
      default:
      {
        const double shelf = ProcessBiquad(input, mAirShelfB0, mAirShelfB1, mAirShelfB2, mAirShelfA1, mAirShelfA2,
                                           mAirShelfS1[channel], mAirShelfS2[channel]);
        return ProcessBiquad(shelf, mAirPeakB0, mAirPeakB1, mAirPeakB2, mAirPeakA1, mAirPeakA2,
                             mAirPeakS1[channel], mAirPeakS2[channel]);
      }
      }
    }

    RealtimeConvolver mConvolverL;
    RealtimeConvolver mConvolverR;

    std::vector<float> mImpulse;      // Original IR samples (mono, at original sample rate)
    std::filesystem::path mIRPath;
    double mIRSampleRate = 48000.0;

    std::vector<double> mInputBufferL;
    std::vector<double> mInputBufferR;
    std::vector<double> mOutputBufferL;
    std::vector<double> mOutputBufferR;

    double mMix = 1.0;
    double mOutputGain = 1.0;
    bool mEnabled = true;
    IRQuality mQuality = IRQuality::Standard;

    double mAir = 0.0;
    AirMode mAirMode = AirMode::OptionA_Shelf;
    bool mAirActive = false;

    // Air filter coefficients
    double mAirShelfB0 = 0, mAirShelfB1 = 0, mAirShelfB2 = 0, mAirShelfA1 = 0, mAirShelfA2 = 0;
    double mAirPeakB0 = 0, mAirPeakB1 = 0, mAirPeakB2 = 0, mAirPeakA1 = 0, mAirPeakA2 = 0;

    // Air filter state (per channel)
    std::array<double, 2> mAirShelfS1 = {}, mAirShelfS2 = {};
    std::array<double, 2> mAirPeakS1 = {}, mAirPeakS2 = {};
  };

  inline void RegisterIRCabEffect()
  {
    EffectTypeInfo info;
    info.type = "cab_ir";
    info.displayName = "IR Cabinet";
    info.category = "cab";
    info.description = "Impulse response cabinet simulation";
    info.requiresResource = true;
    info.resourceType = "ir"; // .wav IR files
    info.parameters = {
        {"mix", "Mix", 1.0, 0.0, 1.0, ""},
        {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"},
      {"air", "Air", 0.0, 0.0, 1.0, "amount"},
      {"airMode", "Air Mode", 0.0, 0.0, 2.0, "enum"},
        {"quality", "Quality", 1.0, 0.0, 3.0, ""}}; // 0=Economy, 1=Standard, 2=High, 3=Full

    EffectRegistry::Instance().Register("cab_ir", info, []()
                                        { return std::make_unique<IRCabEffect>(); });
  }

} // namespace guitarfx
