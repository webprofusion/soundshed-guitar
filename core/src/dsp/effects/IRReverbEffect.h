#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/IRTypes.h"
#include "dsp/IRWavLoader.h"
#include "dsp/RealtimeConvolver.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace guitarfx
{
  /**
   * Impulse Response reverb effect.
   * Supports mono, stereo, and true-stereo (4-channel) IR WAV files.
   */
  class IRReverbEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      mInputBufferL.resize(static_cast<std::size_t>(maxBlockSize));
      mInputBufferR.resize(static_cast<std::size_t>(maxBlockSize));
      mOutputBufferLL.resize(static_cast<std::size_t>(maxBlockSize));
      mOutputBufferRR.resize(static_cast<std::size_t>(maxBlockSize));
      mOutputBufferLR.resize(static_cast<std::size_t>(maxBlockSize));
      mOutputBufferRL.resize(static_cast<std::size_t>(maxBlockSize));

      if (!mImpulseLL.empty())
      {
        InitializeConvolvers();
      }
    }

    void Reset() override
    {
      mConvolverLL.Reset();
      mConvolverRR.Reset();
      mConvolverLR.Reset();
      mConvolverRL.Reset();
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!mEnabled || !mConvolverLL.IsInitialized() || !mConvolverRR.IsInitialized())
      {
        if (outputs[0] && inputs[0])
          std::copy_n(inputs[0], numSamples, outputs[0]);
        if (outputs[1] && inputs[1])
          std::copy_n(inputs[1], numSamples, outputs[1]);
        return;
      }

      for (int i = 0; i < numSamples; ++i)
      {
        mInputBufferL[i] = inputs[0] ? inputs[0][i] : 0.0;
        mInputBufferR[i] = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0);
      }

      mConvolverLL.Process(mInputBufferL.data(), mOutputBufferLL.data(), numSamples);
      mConvolverRR.Process(mInputBufferR.data(), mOutputBufferRR.data(), numSamples);

      if (mHasTrueStereo)
      {
        mConvolverLR.Process(mInputBufferR.data(), mOutputBufferLR.data(), numSamples);
        mConvolverRL.Process(mInputBufferL.data(), mOutputBufferRL.data(), numSamples);
      }

      const float wetGain = static_cast<float>(mMix * mOutputGain);
      const float dryGain = static_cast<float>((1.0 - mMix));

      for (int i = 0; i < numSamples; ++i)
      {
        const float dryL = inputs[0] ? inputs[0][i] : 0.0f;
        const float dryR = inputs[1] ? inputs[1][i] : dryL;

        const double wetL = mOutputBufferLL[i] + (mHasTrueStereo ? mOutputBufferLR[i] : 0.0);
        const double wetR = mOutputBufferRR[i] + (mHasTrueStereo ? mOutputBufferRL[i] : 0.0);

        if (outputs[0])
          outputs[0][i] = static_cast<float>(wetL) * wetGain + dryL * dryGain;
        if (outputs[1])
          outputs[1][i] = static_cast<float>(wetR) * wetGain + dryR * dryGain;
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
        if (!mImpulseLL.empty())
          InitializeConvolvers();
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
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path &resourcePath) override
    {
      if (!LoadWavFile(resourcePath))
        return false;

      mIRPath = resourcePath;
      return InitializeConvolvers();
    }

    [[nodiscard]] bool HasResource() const override
    {
      return mConvolverLL.IsInitialized() && mConvolverRR.IsInitialized();
    }

    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mIRPath; }

    [[nodiscard]] std::string GetType() const override { return "reverb_ir"; }
    [[nodiscard]] std::string GetCategory() const override { return "reverb"; }

  private:
    static size_t FindEnergyTruncationPoint(const std::vector<float> &a, const std::vector<float> &b,
                                            const std::vector<float> *c, const std::vector<float> *d,
                                            float threshold = 0.001f)
    {
      if (a.empty() || b.empty())
        return 0;

      const std::size_t frames = std::min(a.size(), b.size());
      double totalEnergy = 0.0;

      for (std::size_t i = 0; i < frames; ++i)
      {
        totalEnergy += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        totalEnergy += static_cast<double>(b[i]) * static_cast<double>(b[i]);
        if (c && d)
        {
          totalEnergy += static_cast<double>((*c)[i]) * static_cast<double>((*c)[i]);
          totalEnergy += static_cast<double>((*d)[i]) * static_cast<double>((*d)[i]);
        }
      }

      if (totalEnergy < 1e-10)
        return frames;

      const double targetEnergy = totalEnergy * (1.0 - static_cast<double>(threshold));
      double cumulativeEnergy = 0.0;

      for (std::size_t i = 0; i < frames; ++i)
      {
        cumulativeEnergy += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        cumulativeEnergy += static_cast<double>(b[i]) * static_cast<double>(b[i]);
        if (c && d)
        {
          cumulativeEnergy += static_cast<double>((*c)[i]) * static_cast<double>((*c)[i]);
          cumulativeEnergy += static_cast<double>((*d)[i]) * static_cast<double>((*d)[i]);
        }
        if (cumulativeEnergy >= targetEnergy)
          return std::min(i + 256, frames);
      }

      return frames;
    }

    static std::vector<float> TruncateAndFade(const std::vector<float> &input, std::size_t length)
    {
      if (input.empty() || length == 0)
        return {};

      const std::size_t truncLength = std::min(length, input.size());
      std::vector<float> truncated(input.begin(), input.begin() + truncLength);

      constexpr std::size_t kFadeLength = 64;
      if (truncLength > kFadeLength)
      {
        for (std::size_t i = 0; i < kFadeLength; ++i)
        {
          const float fadeGain = static_cast<float>(kFadeLength - 1 - i) / static_cast<float>(kFadeLength - 1);
          truncated[truncLength - kFadeLength + i] *= fadeGain;
        }
      }

      return truncated;
    }

    std::size_t GetMinimumImpulseLength() const
    {
      if (mHasTrueStereo)
        return std::min({mImpulseLL.size(), mImpulseLR.size(), mImpulseRL.size(), mImpulseRR.size()});
      return std::min(mImpulseLL.size(), mImpulseRR.size());
    }

    std::size_t GetTruncationLength() const
    {
      const std::size_t minLength = GetMinimumImpulseLength();
      if (minLength == 0)
        return 0;

      if (mQuality == IRQuality::Full)
        return minLength;

      const size_t maxSamples = GetMaxIRSamples(mQuality, mSampleRate);
      if (maxSamples == 0 || minLength <= maxSamples)
        return minLength;

      const std::size_t energyTrunc = FindEnergyTruncationPoint(
          mImpulseLL, mImpulseRR, mHasTrueStereo ? &mImpulseLR : nullptr, mHasTrueStereo ? &mImpulseRL : nullptr, 0.001f);

      return std::min({minLength, maxSamples, energyTrunc});
    }

    bool LoadWavFile(const std::filesystem::path &path)
    {
      IRWavData data;
      if (!irwav::LoadWavFile(path, data))
        return false;

      mIRSampleRate = data.sampleRate;
      mIRChannels = data.channels;

      if (data.channels >= 4)
      {
        irwav::SplitToQuad(data, mImpulseLL, mImpulseLR, mImpulseRL, mImpulseRR);
        mHasTrueStereo = !mImpulseLL.empty() && !mImpulseLR.empty() && !mImpulseRL.empty() && !mImpulseRR.empty();
      }
      else
      {
        irwav::SplitToStereo(data, mImpulseLL, mImpulseRR);
        mImpulseLR.clear();
        mImpulseRL.clear();
        mHasTrueStereo = false;
      }

      return !mImpulseLL.empty() && !mImpulseRR.empty();
    }

    bool InitializeConvolvers()
    {
      if (mImpulseLL.empty() || mImpulseRR.empty() || mMaxBlockSize == 0)
        return false;

      const std::size_t truncLength = GetTruncationLength();
      if (truncLength == 0)
        return false;

      std::vector<float> processedLL = TruncateAndFade(mImpulseLL, truncLength);
      std::vector<float> processedRR = TruncateAndFade(mImpulseRR, truncLength);
      std::vector<float> processedLR;
      std::vector<float> processedRL;

      if (mHasTrueStereo)
      {
        processedLR = TruncateAndFade(mImpulseLR, truncLength);
        processedRL = TruncateAndFade(mImpulseRL, truncLength);
      }

      if (std::abs(mIRSampleRate - mSampleRate) > 1.0)
      {
        irwav::ResampleLinear(processedLL, mIRSampleRate, mSampleRate);
        irwav::ResampleLinear(processedRR, mIRSampleRate, mSampleRate);
        if (mHasTrueStereo)
        {
          irwav::ResampleLinear(processedLR, mIRSampleRate, mSampleRate);
          irwav::ResampleLinear(processedRL, mIRSampleRate, mSampleRate);
        }
      }

      if (!mConvolverLL.SetImpulse(processedLL, mMaxBlockSize))
        return false;
      if (!mConvolverRR.SetImpulse(processedRR, mMaxBlockSize))
        return false;

      if (mHasTrueStereo)
      {
        if (!mConvolverLR.SetImpulse(processedLR, mMaxBlockSize))
          return false;
        if (!mConvolverRL.SetImpulse(processedRL, mMaxBlockSize))
          return false;
      }

      return true;
    }

    RealtimeConvolver mConvolverLL;
    RealtimeConvolver mConvolverRR;
    RealtimeConvolver mConvolverLR;
    RealtimeConvolver mConvolverRL;

    std::vector<float> mImpulseLL;
    std::vector<float> mImpulseLR;
    std::vector<float> mImpulseRL;
    std::vector<float> mImpulseRR;

    std::filesystem::path mIRPath;
    double mIRSampleRate = 48000.0;
    std::uint16_t mIRChannels = 0;
    bool mHasTrueStereo = false;

    std::vector<double> mInputBufferL;
    std::vector<double> mInputBufferR;
    std::vector<double> mOutputBufferLL;
    std::vector<double> mOutputBufferRR;
    std::vector<double> mOutputBufferLR;
    std::vector<double> mOutputBufferRL;

    double mMix = 1.0;
    double mOutputGain = 1.0;
    IRQuality mQuality = IRQuality::Standard;
  };

  inline void RegisterIRReverbEffect()
  {
    EffectTypeInfo info;
    info.type = "reverb_ir";
    info.displayName = "IR Reverb";
    info.category = "reverb";
    info.description = "Impulse response reverb";
    info.requiresResource = true;
    info.resourceType = "ir";
    info.parameters = {
        {"mix", "Mix", 1.0, 0.0, 1.0, ""},
        {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"},
        {"quality", "Quality", 1.0, 0.0, 3.0, ""}};

    EffectRegistry::Instance().Register("reverb_ir", info, []()
                                        { return std::make_unique<IRReverbEffect>(); });
  }

} // namespace guitarfx
