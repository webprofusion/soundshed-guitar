#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * Spring reverb using short combs, dispersion allpass, and a bandpass tone stage.
   */
  class SpringReverbEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      const std::array<double, 3> combMsL = {29.7, 37.1, 41.1};
      const std::array<double, 3> combMsR = {30.1, 36.7, 40.3};
      const std::array<double, 2> allpassMs = {6.7, 1.7};

      for (size_t i = 0; i < mCombCount; ++i)
      {
        const size_t lenL = DelayMsToSamples(combMsL[i]);
        const size_t lenR = DelayMsToSamples(combMsR[i]);
        mCombBufferL[i].assign(lenL, 0.0f);
        mCombBufferR[i].assign(lenR, 0.0f);
        mCombPosL[i] = 0;
        mCombPosR[i] = 0;
        mCombFilterStoreL[i] = 0.0f;
        mCombFilterStoreR[i] = 0.0f;
      }

      for (size_t i = 0; i < mAllpassCount; ++i)
      {
        const size_t len = DelayMsToSamples(allpassMs[i]);
        mAllpassBufferL[i].assign(len, 0.0f);
        mAllpassBufferR[i].assign(len, 0.0f);
        mAllpassPosL[i] = 0;
        mAllpassPosR[i] = 0;
      }

      UpdateParameters();
      Reset();
    }

    void Reset() override
    {
      for (size_t i = 0; i < mCombCount; ++i)
      {
        std::fill(mCombBufferL[i].begin(), mCombBufferL[i].end(), 0.0f);
        std::fill(mCombBufferR[i].begin(), mCombBufferR[i].end(), 0.0f);
        mCombFilterStoreL[i] = 0.0f;
        mCombFilterStoreR[i] = 0.0f;
        mCombPosL[i] = 0;
        mCombPosR[i] = 0;
      }

      for (size_t i = 0; i < mAllpassCount; ++i)
      {
        std::fill(mAllpassBufferL[i].begin(), mAllpassBufferL[i].end(), 0.0f);
        std::fill(mAllpassBufferR[i].begin(), mAllpassBufferR[i].end(), 0.0f);
        mAllpassPosL[i] = 0;
        mAllpassPosR[i] = 0;
      }

      mToneS1[0] = mToneS1[1] = 0.0;
      mToneS2[0] = mToneS2[1] = 0.0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!outputs || numSamples <= 0)
        return;

      if (mCombBufferL[0].empty())
      {
        for (int ch = 0; ch < 2; ++ch)
          if (inputs && inputs[ch] && outputs[ch])
            std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
        return;
      }

      if (!mEnabled || mMix <= 0.0)
      {
        if (outputs[0])
        {
          if (inputs && inputs[0])
            std::copy_n(inputs[0], numSamples, outputs[0]);
          else
            std::fill_n(outputs[0], numSamples, 0.0f);
        }
        if (outputs[1])
        {
          if (inputs && inputs[1])
            std::copy_n(inputs[1], numSamples, outputs[1]);
          else if (inputs && inputs[0])
            std::copy_n(inputs[0], numSamples, outputs[1]);
          else
            std::fill_n(outputs[1], numSamples, 0.0f);
        }
        return;
      }

      const float wet = static_cast<float>(mMix);
      const float dry = 1.0f - wet;
      const float feedback = static_cast<float>(mFeedback);
      const float damp = static_cast<float>(mDamp);

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs && inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs && inputs[1] ? inputs[1][i] : 0.0f;
        const float input = (inL + inR) * 0.5f;
        const float driven = ApplyDrive(input);

        float outL = 0.0f;
        float outR = 0.0f;

        for (size_t c = 0; c < mCombCount; ++c)
        {
          const float delayedL = mCombBufferL[c][mCombPosL[c]];
          const float delayedR = mCombBufferR[c][mCombPosR[c]];

          mCombFilterStoreL[c] = delayedL * (1.0f - damp) + mCombFilterStoreL[c] * damp;
          mCombFilterStoreR[c] = delayedR * (1.0f - damp) + mCombFilterStoreR[c] * damp;

          mCombBufferL[c][mCombPosL[c]] = driven + mCombFilterStoreL[c] * feedback;
          mCombBufferR[c][mCombPosR[c]] = driven + mCombFilterStoreR[c] * feedback;

          outL += delayedL;
          outR += delayedR;

          mCombPosL[c] = (mCombPosL[c] + 1) % mCombBufferL[c].size();
          mCombPosR[c] = (mCombPosR[c] + 1) % mCombBufferR[c].size();
        }

        outL *= mCombGain;
        outR *= mCombGain;

        for (size_t a = 0; a < mAllpassCount; ++a)
        {
          const float bufOutL = mAllpassBufferL[a][mAllpassPosL[a]];
          const float bufOutR = mAllpassBufferR[a][mAllpassPosR[a]];

          mAllpassBufferL[a][mAllpassPosL[a]] = outL + bufOutL * mAllpassGain;
          mAllpassBufferR[a][mAllpassPosR[a]] = outR + bufOutR * mAllpassGain;

          outL = bufOutL - outL;
          outR = bufOutR - outR;

          mAllpassPosL[a] = (mAllpassPosL[a] + 1) % mAllpassBufferL[a].size();
          mAllpassPosR[a] = (mAllpassPosR[a] + 1) % mAllpassBufferR[a].size();
        }

        const float tonedL = static_cast<float>(ProcessTone(outL, 0));
        const float tonedR = static_cast<float>(ProcessTone(outR, 1));

        if (outputs[0])
          outputs[0][i] = inL * dry + tonedL * wet;
        if (outputs[1])
          outputs[1][i] = inR * dry + tonedR * wet;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "decay")
      {
        mDecay = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "tone")
      {
        mTone = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "drive")
      {
        mDrive = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
      else if (key == "tension")
      {
        mTension = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "decay")
        return mDecay;
      if (key == "tone")
        return mTone;
      if (key == "drive")
        return mDrive;
      if (key == "mix")
        return mMix;
      if (key == "tension")
        return mTension;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "reverb_spring"; }
    [[nodiscard]] std::string GetCategory() const override { return "reverb"; }

  private:
    static constexpr size_t mCombCount = 3;
    static constexpr size_t mAllpassCount = 2;

    size_t DelayMsToSamples(double ms) const
    {
      const double samples = ms * mSampleRate * 0.001;
      return std::max<size_t>(1, static_cast<size_t>(samples));
    }

    void UpdateParameters()
    {
      mFeedback = 0.45 + mDecay * 0.45;
      mDamp = 0.05 + (1.0 - mTone) * 0.5;
      mDriveGain = 1.0 + mDrive * 4.0;
      mAllpassGain = static_cast<float>(0.35 + mTension * 0.35);

      const double centerHz = 1800.0 + mTone * 2500.0;
      const double q = 0.7 + mTone * 1.2;
      ComputeBandpass(centerHz, q, mToneB0, mToneB1, mToneB2, mToneA1, mToneA2);
    }

    void ComputeBandpass(double freq, double q,
                         double &b0, double &b1, double &b2, double &a1, double &a2) const
    {
      if (mSampleRate <= 0.0)
      {
        b0 = 1.0; b1 = 0.0; b2 = 0.0; a1 = 0.0; a2 = 0.0;
        return;
      }

      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * q);

      const double a0 = 1.0 + alpha;
      b0 = alpha / a0;
      b1 = 0.0;
      b2 = -alpha / a0;
      a1 = -2.0 * cosw0 / a0;
      a2 = (1.0 - alpha) / a0;
    }

    double ProcessTone(double input, int channel)
    {
      const double output = mToneB0 * input + mToneS1[channel];
      mToneS1[channel] = mToneB1 * input - mToneA1 * output + mToneS2[channel];
      mToneS2[channel] = mToneB2 * input - mToneA2 * output;
      return output;
    }

    float ApplyDrive(float sample) const
    {
      const float drive = static_cast<float>(mDriveGain);
      const float norm = static_cast<float>(std::tanh(drive));
      if (norm <= 0.0f)
        return sample;
      return static_cast<float>(std::tanh(sample * drive)) / norm;
    }

    static constexpr double kPi = 3.14159265358979323846;

    std::array<std::vector<float>, mCombCount> mCombBufferL;
    std::array<std::vector<float>, mCombCount> mCombBufferR;
    std::array<size_t, mCombCount> mCombPosL = {};
    std::array<size_t, mCombCount> mCombPosR = {};
    std::array<float, mCombCount> mCombFilterStoreL = {};
    std::array<float, mCombCount> mCombFilterStoreR = {};

    std::array<std::vector<float>, mAllpassCount> mAllpassBufferL;
    std::array<std::vector<float>, mAllpassCount> mAllpassBufferR;
    std::array<size_t, mAllpassCount> mAllpassPosL = {};
    std::array<size_t, mAllpassCount> mAllpassPosR = {};

    std::array<double, 2> mToneS1 = {};
    std::array<double, 2> mToneS2 = {};

    double mDecay = 0.6;
    double mTone = 0.6;
    double mDrive = 0.2;
    double mTension = 0.4;
    double mMix = 0.3;

    double mFeedback = 0.8;
    double mDamp = 0.25;
    double mDriveGain = 1.0;
    double mCombGain = 1.0 / static_cast<double>(mCombCount);
    float mAllpassGain = 0.5f;

    double mToneB0 = 1.0;
    double mToneB1 = 0.0;
    double mToneB2 = 0.0;
    double mToneA1 = 0.0;
    double mToneA2 = 0.0;
  };

  inline void RegisterSpringReverbEffect()
  {
    EffectTypeInfo info;
    info.type = "reverb_spring";
    info.displayName = "Spring Reverb";
    info.category = "reverb";
    info.description = "Spring-style reverb with tonal coloration";
    info.requiresResource = false;
    info.parameters = {
        {"decay", "Decay", 0.6, 0.0, 1.0, ""},
        {"tone", "Tone", 0.6, 0.0, 1.0, ""},
        {"drive", "Drive", 0.2, 0.0, 1.0, ""},
      {"tension", "Tension", 0.4, 0.0, 1.0, ""},
        {"mix", "Mix", 0.3, 0.0, 1.0, ""}};

    EffectRegistry::Instance().Register("reverb_spring", info, []()
                                        { return std::make_unique<SpringReverbEffect>(); });
  }

} // namespace guitarfx
