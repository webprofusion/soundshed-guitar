#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * Chorus effect using modulated delay lines.
   */
  class ChorusEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      const double maxDelayMs = 50.0;
      mBufferSize = static_cast<int>(std::ceil(maxDelayMs * sampleRate / 1000.0)) + 2;
      mDelayBufferL.assign(mBufferSize, 0.0f);
      mDelayBufferR.assign(mBufferSize, 0.0f);
      mWriteIndex = 0;
    }

    void Reset() override
    {
      std::fill(mDelayBufferL.begin(), mDelayBufferL.end(), 0.0f);
      std::fill(mDelayBufferR.begin(), mDelayBufferR.end(), 0.0f);
      mWriteIndex = 0;
      mPhase = 0.0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (mBufferSize == 0)
        return;

      const float rateHz = mRateHz.load(std::memory_order_relaxed);
      const float depthMs = mDepthMs.load(std::memory_order_relaxed);
      const float delayMs = mDelayMs.load(std::memory_order_relaxed);
      const float feedback = mFeedback.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);

      const double phaseInc = 2.0 * kPi * rateHz / std::max(1.0, mSampleRate);

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        const float lfoL = static_cast<float>(std::sin(mPhase));
        const float lfoR = static_cast<float>(std::sin(mPhase + kHalfPi));

        const float modL = 0.5f * (1.0f + lfoL);
        const float modR = 0.5f * (1.0f + lfoR);

        const float delayMsL = delayMs + depthMs * modL;
        const float delayMsR = delayMs + depthMs * modR;

        const float delayedL = ReadDelay(mDelayBufferL, delayMsL);
        const float delayedR = ReadDelay(mDelayBufferR, delayMsR);

        mDelayBufferL[mWriteIndex] = inL + delayedL * feedback;
        mDelayBufferR[mWriteIndex] = inR + delayedR * feedback;

        const float outL = inL * (1.0f - mix) + delayedL * mix;
        const float outR = inR * (1.0f - mix) + delayedR * mix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;

        AdvanceWriteIndex();
        mPhase += phaseInc;
        if (mPhase >= 2.0 * kPi)
          mPhase -= 2.0 * kPi;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "rate")
      {
        mRateHz.store(static_cast<float>(std::clamp(value, 0.1, 10.0)), std::memory_order_relaxed);
      }
      else if (key == "depth")
      {
        mDepthMs.store(static_cast<float>(std::clamp(value, 0.0, 20.0)), std::memory_order_relaxed);
      }
      else if (key == "delay")
      {
        mDelayMs.store(static_cast<float>(std::clamp(value, 1.0, 30.0)), std::memory_order_relaxed);
      }
      else if (key == "feedback")
      {
        mFeedback.store(static_cast<float>(std::clamp(value, 0.0, 0.95)), std::memory_order_relaxed);
      }
      else if (key == "mix")
      {
        mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "rate")
        return mRateHz.load(std::memory_order_relaxed);
      if (key == "depth")
        return mDepthMs.load(std::memory_order_relaxed);
      if (key == "delay")
        return mDelayMs.load(std::memory_order_relaxed);
      if (key == "feedback")
        return mFeedback.load(std::memory_order_relaxed);
      if (key == "mix")
        return mMix.load(std::memory_order_relaxed);
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "chorus"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kHalfPi = 1.57079632679489661923;

    float ReadDelay(const std::vector<float> &buffer, float delayMs)
    {
      const float delaySamples = delayMs * static_cast<float>(mSampleRate) / 1000.0f;
      float readIndex = static_cast<float>(mWriteIndex) - delaySamples;
      if (readIndex < 0.0f)
        readIndex += static_cast<float>(mBufferSize);

      const int index0 = static_cast<int>(readIndex);
      const int index1 = (index0 + 1) % mBufferSize;
      const float frac = readIndex - static_cast<float>(index0);

      return buffer[index0] * (1.0f - frac) + buffer[index1] * frac;
    }

    void AdvanceWriteIndex()
    {
      mWriteIndex = (mWriteIndex + 1) % mBufferSize;
    }

    std::vector<float> mDelayBufferL;
    std::vector<float> mDelayBufferR;
    int mBufferSize = 0;
    int mWriteIndex = 0;

    std::atomic<float> mRateHz{1.2f};
    std::atomic<float> mDepthMs{12.0f};
    std::atomic<float> mDelayMs{18.0f};
    std::atomic<float> mFeedback{0.1f};
    std::atomic<float> mMix{0.4f};

    double mPhase = 0.0;
  };

  inline void RegisterChorusEffect()
  {
    EffectTypeInfo info;
    info.type = "chorus";
    info.displayName = "Chorus";
    info.category = "modulation";
    info.description = "Modulated delay chorus";
    info.requiresResource = false;
    info.parameters = {
      {"rate", "Rate", 1.2, 0.1, 10.0, "Hz"},
      {"depth", "Depth", 12.0, 0.0, 20.0, "ms"},
      {"delay", "Delay", 18.0, 1.0, 30.0, "ms"},
      {"feedback", "Feedback", 0.1, 0.0, 0.95, "amount"},
      {"mix", "Mix", 0.4, 0.0, 1.0, "amount"}
    };

    EffectRegistry::Instance().Register("chorus", info, []()
      { return std::make_unique<ChorusEffect>(); });
  }

} // namespace guitarfx
