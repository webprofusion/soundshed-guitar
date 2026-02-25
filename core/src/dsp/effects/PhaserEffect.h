#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
  /**
   * Phaser effect using cascaded all-pass filters with LFO sweep.
   */
  class PhaserEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      Reset();
    }

    void Reset() override
    {
      mPhase = 0.0;
      mFeedbackL = 0.0f;
      mFeedbackR = 0.0f;
      for (auto &s : mStateL)
        s = 0.0f;
      for (auto &s : mStateR)
        s = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      const float rateHz = mRateHz.load(std::memory_order_relaxed);
      const float depth = mDepth.load(std::memory_order_relaxed);
      const float feedback = mFeedback.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);

      const double phaseInc = 2.0 * kPi * rateHz / std::max(1.0, mSampleRate);
      const float center = 700.0f;
      const float range = 1200.0f * depth;

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        const float lfo = static_cast<float>(0.5 * (1.0 + std::sin(mPhase)));
        const float freq = std::clamp(center + range * lfo, 200.0f, 3500.0f);
        const float a = ComputeAllpassCoeff(freq);

        float xL = inL + mFeedbackL * feedback;
        float xR = inR + mFeedbackR * feedback;

        for (std::size_t stage = 0; stage < kStages; ++stage)
        {
          xL = ProcessAllpass(xL, mStateL[stage], a);
          xR = ProcessAllpass(xR, mStateR[stage], a);
        }

        mFeedbackL = xL;
        mFeedbackR = xR;

        const float outL = inL * (1.0f - mix) + xL * mix;
        const float outR = inR * (1.0f - mix) + xR * mix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;

        mPhase += phaseInc;
        if (mPhase >= 2.0 * kPi)
          mPhase -= 2.0 * kPi;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "rate")
      {
        mRateHz.store(static_cast<float>(std::clamp(value, 0.05, 8.0)), std::memory_order_relaxed);
      }
      else if (key == "depth")
      {
        mDepth.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
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
        return mDepth.load(std::memory_order_relaxed);
      if (key == "feedback")
        return mFeedback.load(std::memory_order_relaxed);
      if (key == "mix")
        return mMix.load(std::memory_order_relaxed);
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "phaser"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    static float ProcessAllpass(float x, float &state, float a)
    {
      const float y = -a * x + state;
      state = x + a * y;
      return y;
    }

    float ComputeAllpassCoeff(float freq) const
    {
      const float g = std::tan(static_cast<float>(kPi) * freq / static_cast<float>(mSampleRate));
      return (1.0f - g) / (1.0f + g);
    }

    static constexpr std::size_t kStages = 4;

    std::array<float, kStages> mStateL{};
    std::array<float, kStages> mStateR{};
    float mFeedbackL = 0.0f;
    float mFeedbackR = 0.0f;

    std::atomic<float> mRateHz{0.4f};
    std::atomic<float> mDepth{0.8f};
    std::atomic<float> mFeedback{0.3f};
    std::atomic<float> mMix{0.5f};

    double mPhase = 0.0;
  };

  inline void RegisterPhaserEffect()
  {
    EffectTypeInfo info;
    info.type = "phaser";
    info.displayName = "Phaser";
    info.category = "modulation";
    info.description = "Cascaded all-pass phaser";
    info.requiresResource = false;
    info.parameters = {
      {"rate", "Rate", 0.4, 0.05, 8.0, "Hz"},
      {"depth", "Depth", 0.8, 0.0, 1.0, "amount"},
      {"feedback", "Feedback", 0.3, 0.0, 0.95, "amount"},
      {"mix", "Mix", 0.5, 0.0, 1.0, "amount"}
    };

    EffectRegistry::Instance().Register("phaser", info, []()
      { return std::make_unique<PhaserEffect>(); });
  }

} // namespace guitarfx
