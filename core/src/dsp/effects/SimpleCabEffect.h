#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <array>
#include <cmath>

namespace guitarfx
{
  namespace
  {
    constexpr double kPi = 3.14159265358979323846;
  }

  /**
   * Simple cabinet simulation using cascaded biquad filters.
   * No IR required - provides a lightweight alternative to convolution.
   */
  class SimpleCabEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int /*maxBlockSize*/) override
    {
      mSampleRate = sampleRate;
      UpdateCoefficients();
      Reset();
    }

    void Reset() override
    {
      for (int ch = 0; ch < 2; ++ch)
      {
        mHPState1[ch] = mHPState2[ch] = 0.0;
        mLPState1[ch] = mLPState2[ch] = 0.0;
        mPeakState1[ch] = mPeakState2[ch] = 0.0;
      }
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!mEnabled)
      {
        // Bypass
        if (outputs[0] && inputs[0])
          std::copy_n(inputs[0], numSamples, outputs[0]);
        if (outputs[1] && inputs[1])
          std::copy_n(inputs[1], numSamples, outputs[1]);
        return;
      }

      for (int i = 0; i < numSamples; ++i)
      {
        for (int ch = 0; ch < 2; ++ch)
        {
          float *in = (ch == 0) ? inputs[0] : inputs[1];
          float *out = (ch == 0) ? outputs[0] : outputs[1];

          if (!in || !out)
            continue;

          double sample = static_cast<double>(in[i]);

          // High-pass (removes sub-bass)
          double hp = ProcessBiquad(sample, mHPB0, mHPB1, mHPB2, mHPA1, mHPA2,
                                    mHPState1[ch], mHPState2[ch]);

          // Low-pass (rolls off highs)
          double lp = ProcessBiquad(hp, mLPB0, mLPB1, mLPB2, mLPA1, mLPA2,
                                    mLPState1[ch], mLPState2[ch]);

          // Presence peak (speaker resonance)
          double filtered = ProcessBiquad(lp, mPeakB0, mPeakB1, mPeakB2, mPeakA1, mPeakA2,
                                          mPeakState1[ch], mPeakState2[ch]);

          // Mix dry/wet
          double mixed = sample * (1.0 - mMix) + filtered * mMix;
          out[i] = static_cast<float>(mixed);
        }
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "bass")
      {
        mBass = std::clamp(value, 0.0, 1.0);
        UpdateCoefficients();
      }
      else if (key == "presence")
      {
        mPresence = std::clamp(value, 0.0, 1.0);
        UpdateCoefficients();
      }
      else if (key == "brightness")
      {
        mBrightness = std::clamp(value, 0.0, 1.0);
        UpdateCoefficients();
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
      else if (key == "enabled")
      {
        mEnabled = value > 0.5;
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "bass")
        return mBass;
      if (key == "presence")
        return mPresence;
      if (key == "brightness")
        return mBrightness;
      if (key == "mix")
        return mMix;
      if (key == "enabled")
        return mEnabled ? 1.0 : 0.0;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "cab_simple"; }
    [[nodiscard]] std::string GetCategory() const override { return "cab"; }

  private:
    static double ProcessBiquad(double input, double b0, double b1, double b2,
                                double a1, double a2, double &s1, double &s2)
    {
      const double output = b0 * input + s1;
      s1 = b1 * input - a1 * output + s2;
      s2 = b2 * input - a2 * output;
      return output;
    }

    void UpdateCoefficients()
    {
      if (mSampleRate <= 0.0)
        return;

      // High-pass: 60-120 Hz depending on bass
      const double hpFreq = 60.0 + (1.0 - mBass) * 80.0;
      ComputeHighPass(hpFreq, 0.707, mHPB0, mHPB1, mHPB2, mHPA1, mHPA2);

      // Low-pass: 4-8 kHz depending on brightness
      const double lpFreq = 4000.0 + mBrightness * 4000.0;
      ComputeLowPass(lpFreq, 0.707, mLPB0, mLPB1, mLPB2, mLPA1, mLPA2);

      // Presence peak: 2-3.5 kHz
      const double peakFreq = 2000.0 + mPresence * 1500.0;
      const double peakGain = 2.0 + mPresence * 4.0;
      ComputePeakingEQ(peakFreq, 1.5, peakGain, mPeakB0, mPeakB1, mPeakB2, mPeakA1, mPeakA2);
    }

    void ComputeHighPass(double freq, double Q, double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * Q);

      const double a0 = 1.0 + alpha;
      b0 = (1.0 + cosw0) / 2.0 / a0;
      b1 = -(1.0 + cosw0) / a0;
      b2 = (1.0 + cosw0) / 2.0 / a0;
      a1 = (-2.0 * cosw0) / a0;
      a2 = (1.0 - alpha) / a0;
    }

    void ComputeLowPass(double freq, double Q, double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * Q);

      const double a0 = 1.0 + alpha;
      b0 = (1.0 - cosw0) / 2.0 / a0;
      b1 = (1.0 - cosw0) / a0;
      b2 = (1.0 - cosw0) / 2.0 / a0;
      a1 = (-2.0 * cosw0) / a0;
      a2 = (1.0 - alpha) / a0;
    }

    void ComputePeakingEQ(double freq, double Q, double gainDb, double &b0, double &b1, double &b2, double &a1, double &a2)
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

    // Parameters
    double mBass = 0.5;
    double mPresence = 0.5;
    double mBrightness = 0.5;
    double mMix = 1.0;
    bool mEnabled = true;

    // Filter coefficients
    double mHPB0 = 0, mHPB1 = 0, mHPB2 = 0, mHPA1 = 0, mHPA2 = 0;
    double mLPB0 = 0, mLPB1 = 0, mLPB2 = 0, mLPA1 = 0, mLPA2 = 0;
    double mPeakB0 = 0, mPeakB1 = 0, mPeakB2 = 0, mPeakA1 = 0, mPeakA2 = 0;

    // Filter state (per channel)
    std::array<double, 2> mHPState1 = {}, mHPState2 = {};
    std::array<double, 2> mLPState1 = {}, mLPState2 = {};
    std::array<double, 2> mPeakState1 = {}, mPeakState2 = {};
  };

  inline void RegisterSimpleCabEffect()
  {
    EffectTypeInfo info;
    info.type = "cab_simple";
    info.displayName = "Simple Cabinet";
    info.category = "cab";
    info.description = "Filter-based cabinet simulation (no IR required)";
    info.requiresResource = false;
    info.parameters = {
        {"bass", "Bass", 0.5, 0.0, 1.0, ""},
        {"presence", "Presence", 0.5, 0.0, 1.0, ""},
        {"brightness", "Brightness", 0.5, 0.0, 1.0, ""},
        {"mix", "Mix", 1.0, 0.0, 1.0, ""}};

    EffectRegistry::Instance().Register("cab_simple", info, []()
                                        { return std::make_unique<SimpleCabEffect>(); });
  }

} // namespace guitarfx
