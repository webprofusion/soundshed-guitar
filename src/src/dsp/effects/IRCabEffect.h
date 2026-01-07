#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/RealtimeConvolver.h"
#include <filesystem>
#include <vector>
#include <fstream>

namespace namguitar
{
  /**
   * Impulse Response cabinet simulation effect.
   * Uses convolution to apply a cabinet IR to the signal.
   */
  class IRCabEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

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
    }

    void Process(float** inputs, float** outputs, int numSamples) override
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

    void SetParam(const std::string& key, double value) override
    {
      if (key == "mix")
        mMix = std::clamp(value, 0.0, 1.0);
      else if (key == "outputGain")
        mOutputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      else if (key == "enabled")
        mEnabled = value > 0.5;
    }

    void SetConfig(const std::string&, const std::string&) override {}

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
      if (key == "mix")
        return mMix;
      if (key == "outputGain")
        return 20.0 * std::log10(mOutputGain);
      if (key == "enabled")
        return mEnabled ? 1.0 : 0.0;
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path& resourcePath) override
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
    bool LoadWavFile(const std::filesystem::path& path)
    {
      std::ifstream file(path, std::ios::binary);
      if (!file)
        return false;

      // Read WAV header
      char riff[4];
      file.read(riff, 4);
      if (std::memcmp(riff, "RIFF", 4) != 0)
        return false;

      file.seekg(8, std::ios::beg);
      char wave[4];
      file.read(wave, 4);
      if (std::memcmp(wave, "WAVE", 4) != 0)
        return false;

      // Find fmt chunk
      while (file)
      {
        char chunkId[4];
        uint32_t chunkSize;
        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(&chunkSize), 4);

        if (std::memcmp(chunkId, "fmt ", 4) == 0)
        {
          uint16_t audioFormat;
          file.read(reinterpret_cast<char*>(&audioFormat), 2);
          if (audioFormat != 1 && audioFormat != 3) // PCM or IEEE float
            return false;

          uint16_t numChannels;
          file.read(reinterpret_cast<char*>(&numChannels), 2);

          uint32_t sampleRate;
          file.read(reinterpret_cast<char*>(&sampleRate), 4);
          mIRSampleRate = static_cast<double>(sampleRate);

          file.seekg(6, std::ios::cur); // Skip byte rate and block align

          uint16_t bitsPerSample;
          file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
          mBitsPerSample = bitsPerSample;
          mAudioFormat = audioFormat;

          file.seekg(chunkSize - 16, std::ios::cur); // Skip rest of fmt chunk
        }
        else if (std::memcmp(chunkId, "data", 4) == 0)
        {
          // Found data chunk
          size_t numSamples;
          if (mAudioFormat == 3) // IEEE float
          {
            numSamples = chunkSize / sizeof(float);
            std::vector<float> rawSamples(numSamples);
            file.read(reinterpret_cast<char*>(rawSamples.data()), chunkSize);
            mImpulse = std::move(rawSamples);
          }
          else if (mBitsPerSample == 16)
          {
            numSamples = chunkSize / sizeof(int16_t);
            std::vector<int16_t> rawSamples(numSamples);
            file.read(reinterpret_cast<char*>(rawSamples.data()), chunkSize);
            mImpulse.resize(numSamples);
            for (size_t i = 0; i < numSamples; ++i)
              mImpulse[i] = static_cast<float>(rawSamples[i]) / 32768.0f;
          }
          else if (mBitsPerSample == 24)
          {
            numSamples = chunkSize / 3;
            mImpulse.resize(numSamples);
            for (size_t i = 0; i < numSamples; ++i)
            {
              uint8_t bytes[3];
              file.read(reinterpret_cast<char*>(bytes), 3);
              int32_t value = (static_cast<int32_t>(bytes[2]) << 16) |
                              (static_cast<int32_t>(bytes[1]) << 8) |
                              static_cast<int32_t>(bytes[0]);
              if (value & 0x800000) value |= 0xFF000000; // Sign extend
              mImpulse[i] = static_cast<float>(value) / 8388608.0f;
            }
          }
          else if (mBitsPerSample == 32)
          {
            numSamples = chunkSize / sizeof(int32_t);
            std::vector<int32_t> rawSamples(numSamples);
            file.read(reinterpret_cast<char*>(rawSamples.data()), chunkSize);
            mImpulse.resize(numSamples);
            for (size_t i = 0; i < numSamples; ++i)
              mImpulse[i] = static_cast<float>(rawSamples[i]) / 2147483648.0f;
          }
          else
          {
            return false;
          }

          return !mImpulse.empty();
        }
        else
        {
          file.seekg(chunkSize, std::ios::cur);
        }
      }

      return false;
    }

    bool InitializeConvolvers()
    {
      if (mImpulse.empty() || mMaxBlockSize == 0)
        return false;

      // Resample IR if needed
      std::vector<float> resampledIR;
      if (std::abs(mIRSampleRate - mSampleRate) > 1.0)
      {
        // Simple linear interpolation resampling
        double ratio = mIRSampleRate / mSampleRate;
        size_t newSize = static_cast<size_t>(mImpulse.size() / ratio);
        resampledIR.resize(newSize);

        for (size_t i = 0; i < newSize; ++i)
        {
          double srcPos = i * ratio;
          size_t srcIdx = static_cast<size_t>(srcPos);
          double frac = srcPos - srcIdx;

          if (srcIdx + 1 < mImpulse.size())
            resampledIR[i] = static_cast<float>(mImpulse[srcIdx] * (1.0 - frac) + mImpulse[srcIdx + 1] * frac);
          else if (srcIdx < mImpulse.size())
            resampledIR[i] = mImpulse[srcIdx];
        }
      }
      else
      {
        resampledIR = mImpulse;
      }

      // Initialize both convolvers with the same IR
      if (!mConvolverL.SetImpulse(resampledIR, mMaxBlockSize))
        return false;
      if (!mConvolverR.SetImpulse(resampledIR, mMaxBlockSize))
        return false;

      return true;
    }

    RealtimeConvolver mConvolverL;
    RealtimeConvolver mConvolverR;

    std::vector<float> mImpulse;
    std::filesystem::path mIRPath;
    double mIRSampleRate = 48000.0;
    uint16_t mBitsPerSample = 16;
    uint16_t mAudioFormat = 1; // PCM

    std::vector<double> mInputBufferL;
    std::vector<double> mInputBufferR;
    std::vector<double> mOutputBufferL;
    std::vector<double> mOutputBufferR;

    double mMix = 1.0;
    double mOutputGain = 1.0;
    bool mEnabled = true;
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
      {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"}
    };

    EffectRegistry::Instance().Register("cab_ir", info, []() {
      return std::make_unique<IRCabEffect>();
    });
  }

} // namespace namguitar
