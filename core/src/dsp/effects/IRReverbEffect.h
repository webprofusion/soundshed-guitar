#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/IRTypes.h"
#include "dsp/IRWavLoader.h"
#include "dsp/RealtimeConvolver.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

      // CRITICAL: Clear filter state BEFORE updating filter coefficients to prevent transients
      // when stale filter state is processed with new sample-rate-dependent coefficients.
      mToneStateL = 0.0f;
      mToneStateR = 0.0f;

      ApplyPendingQuality();
      UpdateToneFilter();

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
      mToneStateL = 0.0f;
      mToneStateR = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      // Clamp to allocated buffer size to prevent out-of-bounds writes
      numSamples = std::min(numSamples, mMaxBlockSize);

      if (!mEnabled || !mConvolverLL.IsInitialized() || !mConvolverRR.IsInitialized()
          || mRebuilding.load(std::memory_order_acquire))
      {
        // Bypass: copy input to output, falling back L→R if R is null
        if (outputs[0])
        {
          if (inputs[0])
            std::copy_n(inputs[0], numSamples, outputs[0]);
          else
            std::fill_n(outputs[0], numSamples, 0.0f);
        }
        if (outputs[1])
        {
          if (inputs[1])
            std::copy_n(inputs[1], numSamples, outputs[1]);
          else if (inputs[0])
            std::copy_n(inputs[0], numSamples, outputs[1]);
          else
            std::fill_n(outputs[1], numSamples, 0.0f);
        }
        return;
      }

      for (int i = 0; i < numSamples; ++i)
      {
        mInputBufferL[i] = inputs[0] ? inputs[0][i] : 0.0f;
        mInputBufferR[i] = inputs[1] ? inputs[1][i] : (inputs[0] ? inputs[0][i] : 0.0f);
      }

      mConvolverLL.Process(mInputBufferL.data(), mOutputBufferLL.data(), numSamples);
      mConvolverRR.Process(mInputBufferR.data(), mOutputBufferRR.data(), numSamples);

      if (mHasTrueStereo)
      {
        mConvolverLR.Process(mInputBufferR.data(), mOutputBufferLR.data(), numSamples);
        mConvolverRL.Process(mInputBufferL.data(), mOutputBufferRL.data(), numSamples);
      }

      const double mix = mMix.load(std::memory_order_relaxed);
      const float wetGain = static_cast<float>(mix * mOutputGain.load(std::memory_order_relaxed));
      const float dryGain = static_cast<float>(1.0 - mix);
      const float toneCoef = mToneCoef.load(std::memory_order_relaxed);

      for (int i = 0; i < numSamples; ++i)
      {
        const float dryL = inputs[0] ? inputs[0][i] : 0.0f;
        const float dryR = inputs[1] ? inputs[1][i] : dryL;

        float wetL = static_cast<float>(mOutputBufferLL[i] + (mHasTrueStereo ? mOutputBufferLR[i] : 0.0f));
        float wetR = static_cast<float>(mOutputBufferRR[i] + (mHasTrueStereo ? mOutputBufferRL[i] : 0.0f));

        if (toneCoef < 1.0f)
        {
          mToneStateL += toneCoef * (wetL - mToneStateL);
          mToneStateR += toneCoef * (wetR - mToneStateR);
          wetL = mToneStateL;
          wetR = mToneStateR;
        }

        if (outputs[0])
          outputs[0][i] = wetL * wetGain + dryL * dryGain;
        if (outputs[1])
          outputs[1][i] = wetR * wetGain + dryR * dryGain;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "mix")
        mMix.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
      else if (key == "outputGain")
        mOutputGain.store(std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0), std::memory_order_relaxed);
      else if (key == "enabled")
        mEnabled = value > 0.5;
      else if (key == "tone")
      {
        mTone.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        UpdateToneFilter();
      }
      else if (key == "quality")
      {
        const int q = static_cast<int>(std::clamp(value, 0.0, 3.0));
        mPendingQuality.store(q, std::memory_order_release);
        // Reinitialise immediately so quality changes take effect without requiring
        // a prepareToPlay() call. Only safe when called from non-audio thread (UI interaction).
        if (HasResource())
        {
          ApplyPendingQuality();
          InitializeConvolvers();
        }
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "mix")
        return mMix.load(std::memory_order_relaxed);
      if (key == "tone")
        return mTone.load(std::memory_order_relaxed);
      if (key == "outputGain")
        return 20.0 * std::log10(mOutputGain.load(std::memory_order_relaxed));
      if (key == "enabled")
        return mEnabled ? 1.0 : 0.0;
      if (key == "quality")
      {
        const int pending = mPendingQuality.load(std::memory_order_acquire);
        return pending >= 0 ? static_cast<double>(pending) : static_cast<double>(mQuality);
      }
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path &resourcePath) override
    {
      if (!std::filesystem::exists(resourcePath))
      {
        std::cerr << "[IRReverbEffect] ERROR: IR file not found: " << resourcePath << "\n";
        return false;
      }

      if (!LoadWavFile(resourcePath))
      {
        std::cerr << "[IRReverbEffect] ERROR: Failed to load/parse IR WAV file: " << resourcePath << "\n";
        return false;
      }

      mIRPath = resourcePath;
      ApplyPendingQuality();

      if (!InitializeConvolvers())
      {
        std::cerr << "[IRReverbEffect] ERROR: Failed to initialize convolvers for: " << resourcePath << "\n";
        return false;
      }

      return true;
    }

    [[nodiscard]] bool HasResource() const override
    {
      return mConvolverLL.IsInitialized() && mConvolverRR.IsInitialized();
    }

    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mIRPath; }

    [[nodiscard]] int GetLatencySamples() const override
    {
      return mConvolverLL.IsInitialized() ? mConvolverLL.GetLatency() : 0;
    }

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

    static std::vector<float> TruncateAndFade(const std::vector<float> &input, std::size_t length,
                                               std::size_t fadeLen = 2048)
    {
      if (input.empty() || length == 0)
        return {};

      const std::size_t truncLength = std::min(length, input.size());
      std::vector<float> truncated(input.begin(), input.begin() + truncLength);

      // Use a long fade (~42ms at 48kHz) to prevent Gibbs-phenomenon ringing
      // when the reverb tail is still active at the truncation point.
      const std::size_t effectiveFade = std::min(fadeLen, truncLength);
      if (effectiveFade > 1)
      {
        for (std::size_t i = 0; i < effectiveFade; ++i)
        {
          const float fadeGain = static_cast<float>(effectiveFade - 1 - i) / static_cast<float>(effectiveFade - 1);
          truncated[truncLength - effectiveFade + i] *= fadeGain;
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

      // GetMaxReverbIRSamples returns a limit expressed in *playback-rate* samples.
      // The raw impulse response is at mIRSampleRate, so convert to the same units
      // before comparing, otherwise 96kHz IRs get half the expected truncation length.
      const size_t maxSamplesPlayback = GetMaxReverbIRSamples(mQuality, mSampleRate);
      const size_t maxSamples = (maxSamplesPlayback > 0 && mSampleRate > 1.0)
          ? static_cast<size_t>(static_cast<double>(maxSamplesPlayback) * mIRSampleRate / mSampleRate)
          : maxSamplesPlayback;

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
      {
        std::cerr << "[IRReverbEffect] Failed to parse WAV data from: " << path << "\n";
        return false;
      }

      if (data.channels < 1)
      {
        std::cerr << "[IRReverbEffect] ERROR: IR file has no audio channels: " << path << "\n";
        return false;
      }

      mIRSampleRate = data.sampleRate;
      mIRChannels = data.channels;

      if (data.channels >= 4)
      {
        irwav::SplitToQuad(data, mImpulseLL, mImpulseLR, mImpulseRL, mImpulseRR);
        mHasTrueStereo = !mImpulseLL.empty() && !mImpulseLR.empty() && !mImpulseRL.empty() && !mImpulseRR.empty();
        
        if (!mHasTrueStereo)
        {
          std::cerr << "[IRReverbEffect] WARNING: 4-channel IR file has empty channels: " << path << "\n";
        }
      }
      else
      {
        irwav::SplitToStereo(data, mImpulseLL, mImpulseRR);
        mImpulseLR.clear();
        mImpulseRL.clear();
        mHasTrueStereo = false;
      }

      if (mImpulseLL.empty() || mImpulseRR.empty())
      {
        std::cerr << "[IRReverbEffect] ERROR: IR file missing required stereo channels: " << path << "\n";
        return false;
      }

      return true;
    }

    bool InitializeConvolvers()
    {
      if (mImpulseLL.empty() || mImpulseRR.empty() || mMaxBlockSize == 0)
        return false;

      // Signal the audio thread to bypass (dry copy) while convolvers are being rebuilt.
      mRebuilding.store(true, std::memory_order_release);

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
        irwav::ResampleSinc(processedLL, mIRSampleRate, mSampleRate);
        irwav::ResampleSinc(processedRR, mIRSampleRate, mSampleRate);
        if (mHasTrueStereo)
        {
          irwav::ResampleSinc(processedLR, mIRSampleRate, mSampleRate);
          irwav::ResampleSinc(processedRL, mIRSampleRate, mSampleRate);
        }
      }

      // Normalize by peak amplitude across all channels so that un-normalized reverb IRs
      // (which may have large early-reflection amplitudes) don't saturate the convolver output.
      float peak = 0.0f;
      for (float s : processedLL) peak = std::max(peak, std::fabs(s));
      for (float s : processedRR) peak = std::max(peak, std::fabs(s));
      if (mHasTrueStereo)
      {
        for (float s : processedLR) peak = std::max(peak, std::fabs(s));
        for (float s : processedRL) peak = std::max(peak, std::fabs(s));
      }
      if (peak > 1e-6f)
      {
        const float inv = 1.0f / peak;
        for (float &s : processedLL) s *= inv;
        for (float &s : processedRR) s *= inv;
        if (mHasTrueStereo)
        {
          for (float &s : processedLR) s *= inv;
          for (float &s : processedRL) s *= inv;
        }
      }

      if (!mConvolverLL.SetImpulse(processedLL, mMaxBlockSize))
      {
        mRebuilding.store(false, std::memory_order_release);
        return false;
      }
      if (!mConvolverRR.SetImpulse(processedRR, mMaxBlockSize))
      {
        mRebuilding.store(false, std::memory_order_release);
        return false;
      }

      if (mHasTrueStereo)
      {
        if (!mConvolverLR.SetImpulse(processedLR, mMaxBlockSize))
        {
          mRebuilding.store(false, std::memory_order_release);
          return false;
        }
        if (!mConvolverRL.SetImpulse(processedRL, mMaxBlockSize))
        {
          mRebuilding.store(false, std::memory_order_release);
          return false;
        }
      }

      mRebuilding.store(false, std::memory_order_release);
      return true;
    }

    void ApplyPendingQuality()
    {
      const int pending = mPendingQuality.exchange(-1, std::memory_order_acq_rel);
      if (pending >= 0)
      {
        mQuality = static_cast<IRQuality>(pending);
      }
    }

    void UpdateToneFilter()
    {
      const float t = mTone.load(std::memory_order_relaxed);
      if (t >= 1.0f)
      {
        mToneCoef.store(1.0f, std::memory_order_relaxed);
        return;
      }
      const float minHz = 1500.0f;
      const float maxHz = 20000.0f;
      const float cutoff = minHz + (maxHz - minHz) * t;
      const float x = static_cast<float>(2.0 * 3.14159265358979323846 * cutoff / std::max(1.0, mSampleRate));
      mToneCoef.store(1.0f - std::exp(-x), std::memory_order_relaxed);
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

    std::vector<float> mInputBufferL;
    std::vector<float> mInputBufferR;
    std::vector<float> mOutputBufferLL;
    std::vector<float> mOutputBufferRR;
    std::vector<float> mOutputBufferLR;
    std::vector<float> mOutputBufferRL;

    std::atomic<double> mMix{0.3};
    std::atomic<double> mOutputGain{1.0};
    IRQuality mQuality = IRQuality::Standard;
    std::atomic<int> mPendingQuality{-1};
    std::atomic<float> mTone{1.0f};
    std::atomic<float> mToneCoef{1.0f};
    // Set true during convolver rebuild to let the audio thread bypass safely.
    std::atomic<bool> mRebuilding{false};
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;
  };

  inline void RegisterIRReverbEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbIr;
    info.aliases = {"reverb_ir"};
    info.displayName = "Convolution Reverb";
    info.category = "reverb";
    info.description = "Impulse response reverb";
    info.requiresResource = true;
    info.resourceType = "ir";
    info.parameters = {
        {"mix", "Mix", 0.3, 0.0, 1.0, "amount"},
        {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"},
        {"tone", "Tone", 1.0, 0.0, 1.0, "amount"},
        {"quality", "Quality", 3.0, 0.0, 3.0, "enum", "", false, 1.0, {"Economy", "Standard", "High", "Full"}}};

    EffectRegistry::Instance().Register(info.type, info, []()
                                        { return std::make_unique<IRReverbEffect>(); });
  }

} // namespace guitarfx
