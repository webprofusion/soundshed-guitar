#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
  /*
   * Design notes (gain stages v1):
   * - Stage count is clamped to 1..6, defaulting to 2 to preserve legacy presets.
   * - Tone stack is positioned between stage 2 and stage 3 ("split around tone").
   * - Stage gains are in dB (per-stage trims) with 0 dB default; gain/voice retain voicing.
   * - Missing params fall back to defaults; no allocations or locks in Process().
   */
  /**
   * Built-in amp effect with smooth Clean/Drive voicing and tone controls.
   */
  class BuiltinAmpEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      UpdateSmoothing();
      UpdatePreFilters();
      UpdateToneStack();
      Reset();
    }

    void Reset() override
    {
      for (int ch = 0; ch < 2; ++ch)
      {
        mLowS1[ch] = mLowS2[ch] = 0.0;
        mMidS1[ch] = mMidS2[ch] = 0.0;
        mContourS1[ch] = mContourS2[ch] = 0.0;
        mTrebleS1[ch] = mTrebleS2[ch] = 0.0;
        mPresenceS1[ch] = mPresenceS2[ch] = 0.0;
        mPreHPS1[ch] = mPreHPS2[ch] = 0.0;
      }
      mVoiceSmoothed = mVoice;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!outputs)
        return;

      const float cleanGain = static_cast<float>(1.0 + 5.0 * mGain);
      const float driveGain = static_cast<float>(1.0 + 45.0 * mGain);
      const float outputGain = static_cast<float>(std::pow(10.0, mOutputDb * 0.05));
      const float masterGain = static_cast<float>(1.0 + 6.0 * mGain);
      const float smoothStep = static_cast<float>(1.0 - mVoiceSmoothCoef);
      const int stageCount = std::clamp(mStageCount, 1, kMaxStages);
      const int preStages = std::min(stageCount, kPreToneStages);

      for (int i = 0; i < numSamples; ++i)
      {
        mVoiceSmoothed += smoothStep * (mVoice - mVoiceSmoothed);
        const float voice = std::clamp(mVoiceSmoothed, 0.0f, 1.0f);

        for (int ch = 0; ch < 2; ++ch)
        {
          float *in = inputs ? (ch == 0 ? inputs[0] : inputs[1]) : nullptr;
          float *out = (ch == 0) ? outputs[0] : outputs[1];
          if (!out)
            continue;

          const float sample = in ? in[i] : 0.0f;

          double tightened = ProcessBiquad(sample, mPreHPB0, mPreHPB1, mPreHPB2, mPreHPA1, mPreHPA2,
                                           mPreHPS1[ch], mPreHPS2[ch]);

          const float pre = static_cast<float>(tightened) * mStageGainLinear[0] * masterGain;
          const float clean = SoftClip(pre * cleanGain) * 0.9f;
          const float drive = SoftClip(pre * driveGain);
          const float blended = clean + (drive - clean) * voice;

          float stageOut = blended;
          if (preStages >= 2)
          {
            stageOut = SoftClip(stageOut * masterGain * mStageGainLinear[1]);
          }

          double processed = stageOut;
          processed = ProcessBiquad(processed, mLowB0, mLowB1, mLowB2, mLowA1, mLowA2, mLowS1[ch], mLowS2[ch]);
          processed = ProcessBiquad(processed, mMidB0, mMidB1, mMidB2, mMidA1, mMidA2, mMidS1[ch], mMidS2[ch]);
          processed = ProcessBiquad(processed, mContourB0, mContourB1, mContourB2, mContourA1, mContourA2,
                                    mContourS1[ch], mContourS2[ch]);
          processed = ProcessBiquad(processed, mTrebleB0, mTrebleB1, mTrebleB2, mTrebleA1, mTrebleA2,
                                    mTrebleS1[ch], mTrebleS2[ch]);
          processed = ProcessBiquad(processed, mPresenceB0, mPresenceB1, mPresenceB2, mPresenceA1, mPresenceA2,
                                    mPresenceS1[ch], mPresenceS2[ch]);

          for (int stageIndex = kPreToneStages; stageIndex < stageCount; ++stageIndex)
          {
            processed = SoftClip(static_cast<float>(processed) * masterGain * mStageGainLinear[stageIndex]);
          }

          out[i] = static_cast<float>(processed) * outputGain;
        }
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "voice")
      {
        mVoice = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "gain")
      {
        mGain = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "bass")
      {
        mBass = std::clamp(value, 0.0, 1.0);
        UpdateToneStack();
      }
      else if (key == "middle")
      {
        mMiddle = std::clamp(value, 0.0, 1.0);
        UpdateToneStack();
      }
      else if (key == "treble")
      {
        mTreble = std::clamp(value, 0.0, 1.0);
        UpdateToneStack();
      }
      else if (key == "contour")
      {
        mContour = std::clamp(value, 0.0, 1.0);
        UpdateToneStack();
      }
      else if (key == "presence")
      {
        mPresence = std::clamp(value, 0.0, 1.0);
        UpdateToneStack();
      }
      else if (key == "output")
      {
        mOutputDb = std::clamp(value, -24.0, 24.0);
      }
      else if (key == "stageCount")
      {
        const int count = static_cast<int>(std::round(value));
        mStageCount = std::clamp(count, 1, kMaxStages);
      }
      else if (key == "stage1Gain")
      {
        SetStageGain(0, value);
      }
      else if (key == "stage2Gain")
      {
        SetStageGain(1, value);
      }
      else if (key == "stage3Gain")
      {
        SetStageGain(2, value);
      }
      else if (key == "stage4Gain")
      {
        SetStageGain(3, value);
      }
      else if (key == "stage5Gain")
      {
        SetStageGain(4, value);
      }
      else if (key == "stage6Gain")
      {
        SetStageGain(5, value);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "voice")
        return mVoice;
      if (key == "gain")
        return mGain;
      if (key == "bass")
        return mBass;
      if (key == "middle")
        return mMiddle;
      if (key == "treble")
        return mTreble;
      if (key == "contour")
        return mContour;
      if (key == "presence")
        return mPresence;
      if (key == "output")
        return mOutputDb;
      if (key == "stageCount")
        return mStageCount;
      if (key == "stage1Gain")
        return mStageGainDb[0];
      if (key == "stage2Gain")
        return mStageGainDb[1];
      if (key == "stage3Gain")
        return mStageGainDb[2];
      if (key == "stage4Gain")
        return mStageGainDb[3];
      if (key == "stage5Gain")
        return mStageGainDb[4];
      if (key == "stage6Gain")
        return mStageGainDb[5];
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "amp_builtin"; }
    [[nodiscard]] std::string GetCategory() const override { return "amp"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr int kMaxStages = 6;
    static constexpr int kPreToneStages = 2;

    static float SoftClip(float x)
    {
      return std::tanh(x);
    }

    static double ProcessBiquad(double input, double b0, double b1, double b2,
                                double a1, double a2, double &s1, double &s2)
    {
      const double output = b0 * input + s1;
      s1 = b1 * input - a1 * output + s2;
      s2 = b2 * input - a2 * output;
      return output;
    }

    static float DbToLinear(double db)
    {
      return static_cast<float>(std::pow(10.0, db * 0.05));
    }

    void SetStageGain(int index, double value)
    {
      if (index < 0 || index >= kMaxStages)
        return;
      const double clamped = std::clamp(value, -24.0, 24.0);
      mStageGainDb[index] = clamped;
      mStageGainLinear[index] = DbToLinear(clamped);
    }

    void UpdateSmoothing()
    {
      if (mSampleRate <= 0.0)
      {
        mVoiceSmoothCoef = 0.0f;
        return;
      }
      const double tau = 0.03; // 30 ms smoothing
      mVoiceSmoothCoef = static_cast<float>(std::exp(-1.0 / (tau * mSampleRate)));
    }

    void UpdateToneStack()
    {
      if (mSampleRate <= 0.0)
        return;

      const double bassGain = (mBass - 0.5) * 18.0;
      const double midGain = (mMiddle - 0.5) * 18.0;
      const double trebleGain = (mTreble - 0.5) * 18.0;
      const double contourGain = -12.0 * mContour;
      const double presenceGain = (mPresence - 0.5) * 12.0;

      ComputeLowShelf(120.0, 0.8, bassGain, mLowB0, mLowB1, mLowB2, mLowA1, mLowA2);
      ComputePeakingEQ(750.0, 0.9, midGain, mMidB0, mMidB1, mMidB2, mMidA1, mMidA2);
      ComputePeakingEQ(600.0, 0.7, contourGain, mContourB0, mContourB1, mContourB2, mContourA1, mContourA2);
      ComputeHighShelf(3500.0, 0.9, trebleGain, mTrebleB0, mTrebleB1, mTrebleB2, mTrebleA1, mTrebleA2);
      ComputePeakingEQ(4000.0, 1.2, presenceGain, mPresenceB0, mPresenceB1, mPresenceB2, mPresenceA1, mPresenceA2);
    }

    void UpdatePreFilters()
    {
      if (mSampleRate <= 0.0)
        return;

      ComputeHighPass(90.0, 0.707, mPreHPB0, mPreHPB1, mPreHPB2, mPreHPA1, mPreHPA2);
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

    void ComputeLowShelf(double freq, double slope, double gainDb,
                         double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double w0 = 2.0 * kPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double sqrtA = std::sqrt(A);
      const double alpha = sinw0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);

      const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
      b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
      b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
      b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
      a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
      a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
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

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 0;

    float mVoice = 0.0f;
    float mVoiceSmoothed = 0.0f;
    float mVoiceSmoothCoef = 0.0f;
    float mGain = 0.45f;
    double mBass = 0.5;
    double mMiddle = 0.5;
    double mTreble = 0.5;
    double mContour = 0.2;
    double mPresence = 0.5;
    double mOutputDb = 0.0;
    int mStageCount = 2;
    std::array<double, kMaxStages> mStageGainDb = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<float, kMaxStages> mStageGainLinear = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    double mPreHPB0 = 1.0, mPreHPB1 = 0.0, mPreHPB2 = 0.0, mPreHPA1 = 0.0, mPreHPA2 = 0.0;

    double mLowB0 = 1.0, mLowB1 = 0.0, mLowB2 = 0.0, mLowA1 = 0.0, mLowA2 = 0.0;
    double mMidB0 = 1.0, mMidB1 = 0.0, mMidB2 = 0.0, mMidA1 = 0.0, mMidA2 = 0.0;
    double mContourB0 = 1.0, mContourB1 = 0.0, mContourB2 = 0.0, mContourA1 = 0.0, mContourA2 = 0.0;
    double mTrebleB0 = 1.0, mTrebleB1 = 0.0, mTrebleB2 = 0.0, mTrebleA1 = 0.0, mTrebleA2 = 0.0;
    double mPresenceB0 = 1.0, mPresenceB1 = 0.0, mPresenceB2 = 0.0, mPresenceA1 = 0.0, mPresenceA2 = 0.0;

    std::array<double, 2> mLowS1 = {}, mLowS2 = {};
    std::array<double, 2> mMidS1 = {}, mMidS2 = {};
    std::array<double, 2> mContourS1 = {}, mContourS2 = {};
    std::array<double, 2> mTrebleS1 = {}, mTrebleS2 = {};
    std::array<double, 2> mPresenceS1 = {}, mPresenceS2 = {};
    std::array<double, 2> mPreHPS1 = {}, mPreHPS2 = {};
  };

  inline void RegisterBuiltinAmpEffect()
  {
    EffectTypeInfo info;
    info.type = "amp_builtin";
    info.displayName = "Heavy American";
    info.category = "amp";
    info.description = "Built-in amp with smooth Clean/Drive voice and modern tone controls";
    info.requiresResource = false;
    info.parameters = {
      {"voice", "Voice", 0.0, 0.0, 1.0, "toggle"},
      {"gain", "Gain", 0.45, 0.0, 1.0, "amount"},
      {"bass", "Bass", 0.5, 0.0, 1.0, "amount"},
      {"middle", "Middle", 0.5, 0.0, 1.0, "amount"},
      {"treble", "Treble", 0.5, 0.0, 1.0, "amount"},
      {"contour", "Contour", 0.2, 0.0, 1.0, "amount"},
      {"presence", "Presence", 0.5, 0.0, 1.0, "amount"},
      {"output", "Output", 0.0, -24.0, 24.0, "dB"},
      {"stageCount", "Gain Stages", 2.0, 1.0, 6.0, "amount"},
      {"stage1Gain", "Stage 1 Gain", 0.0, -24.0, 24.0, "dB"},
      {"stage2Gain", "Stage 2 Gain", 0.0, -24.0, 24.0, "dB"},
      {"stage3Gain", "Stage 3 Gain", 0.0, -24.0, 24.0, "dB"},
      {"stage4Gain", "Stage 4 Gain", 0.0, -24.0, 24.0, "dB"},
      {"stage5Gain", "Stage 5 Gain", 0.0, -24.0, 24.0, "dB"},
      {"stage6Gain", "Stage 6 Gain", 0.0, -24.0, 24.0, "dB"}
    };

    EffectRegistry::Instance().Register("amp_builtin", info, []()
      { return std::make_unique<BuiltinAmpEffect>(); });
  }

} // namespace guitarfx
