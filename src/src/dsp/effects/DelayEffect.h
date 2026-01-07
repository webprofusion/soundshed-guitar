#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>
#include <vector>

namespace namguitar
{
  /**
   * Simple delay effect.
   */
  class DelayEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Allocate for max 2 seconds delay
      const size_t maxSamples = static_cast<size_t>(sampleRate * 2.0);
      mBufferL.resize(maxSamples, 0.0f);
      mBufferR.resize(maxSamples, 0.0f);

      UpdateDelaySamples();
      Reset();
    }

    void Reset() override
    {
      std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
      std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
      mWritePos = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
      const float feedback = static_cast<float>(mFeedback);
      const float wet = static_cast<float>(mMix);
      const float dry = 1.0f - wet;

      for (int i = 0; i < numSamples; ++i)
      {
        // Read positions
        size_t readPos = (mWritePos + mBufferL.size() - mDelaySamples) % mBufferL.size();

        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : 0.0f;

        float delayedL = mBufferL[readPos];
        float delayedR = mBufferR[readPos];

        // Write to buffer with feedback
        mBufferL[mWritePos] = inL + delayedL * feedback;
        mBufferR[mWritePos] = inR + delayedR * feedback;

        // Output mix
        if (outputs[0])
          outputs[0][i] = inL * dry + delayedL * wet;
        if (outputs[1])
          outputs[1][i] = inR * dry + delayedR * wet;

        mWritePos = (mWritePos + 1) % mBufferL.size();
      }
    }

    void SetParam(const std::string& key, double value) override
    {
      if (key == "time" || key == "timeMs")
      {
        mDelayMs = std::clamp(value, 1.0, 2000.0);
        UpdateDelaySamples();
      }
      else if (key == "feedback")
      {
        mFeedback = std::clamp(value, 0.0, 0.95);
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
    }

    void SetConfig(const std::string&, const std::string&) override {}

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
      if (key == "time" || key == "timeMs")
        return mDelayMs;
      if (key == "feedback")
        return mFeedback;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "delay_digital"; }
    [[nodiscard]] std::string GetCategory() const override { return "delay"; }

  private:
    void UpdateDelaySamples()
    {
      mDelaySamples = static_cast<size_t>(mSampleRate * mDelayMs * 0.001);
      if (!mBufferL.empty())
      {
        mDelaySamples = std::min(mDelaySamples, mBufferL.size() - 1);
      }
    }

    std::vector<float> mBufferL;
    std::vector<float> mBufferR;
    size_t mWritePos = 0;
    size_t mDelaySamples = 0;

    double mDelayMs = 300.0;
    double mFeedback = 0.3;
    double mMix = 0.3;
  };

  inline void RegisterDelayEffect()
  {
    EffectTypeInfo info;
    info.type = "delay_digital";
    info.displayName = "Digital Delay";
    info.category = "delay";
    info.description = "Simple digital delay";
    info.requiresResource = false;
    info.parameters = {
      {"timeMs", "Time", 300.0, 1.0, 2000.0, "ms"},
      {"feedback", "Feedback", 0.3, 0.0, 0.95, ""},
      {"mix", "Mix", 0.3, 0.0, 1.0, ""}
    };

    EffectRegistry::Instance().Register("delay_digital", info, []() {
      return std::make_unique<DelayEffect>();
    });
  }

} // namespace namguitar
