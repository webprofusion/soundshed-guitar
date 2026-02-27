#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
  /*
   * Design notes (gain stages v1):
  * - Stage count is clamped to 1..4, defaulting to 2.
   * - Tone stack is positioned between stage 2 and stage 3 ("split around tone").
  * - Stage gain is a single dB trim applied to all stages; gain/voice retain voicing.
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
      UpdateSagCoefficients();
      UpdatePreFilters();
      UpdatePreEmphasis();
      UpdateToneStack();
      UpdateSpeakerFilters();
      UpdatePostFilters();
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
        mPreEmphS1[ch] = mPreEmphS2[ch] = 0.0;
        mDepthS1[ch] = mDepthS2[ch] = 0.0;
        mResonanceS1[ch] = mResonanceS2[ch] = 0.0;
        mDampingS1[ch] = mDampingS2[ch] = 0.0;
        mPostHPS1[ch] = mPostHPS2[ch] = 0.0;
        mSagEnv[ch] = 0.0f;
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
          tightened = ProcessBiquad(tightened, mPreEmphB0, mPreEmphB1, mPreEmphB2, mPreEmphA1, mPreEmphA2,
                                    mPreEmphS1[ch], mPreEmphS2[ch]);

          const float pre = static_cast<float>(tightened) * mStageGainLinear * masterGain;
          const float clean = SoftClip(pre * cleanGain) * 0.9f;
          const float drive = SoftClip(pre * driveGain);
          const float blended = clean + (drive - clean) * voice;

          float stageOut = blended;
          if (preStages >= 2)
          {
            stageOut = SoftClip(stageOut * masterGain * mStageGainLinear);
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
            processed = SoftClip(static_cast<float>(processed) * masterGain * mStageGainLinear);
          }

          float powerInput = static_cast<float>(processed);
          if (std::abs(mBias) > 1.0e-6f)
          {
            powerInput += mBias * 0.1f;
          }

          const float detector = std::abs(powerInput);
          if (detector > mSagEnv[ch])
            mSagEnv[ch] = mSagAttackCoef * mSagEnv[ch] + (1.0f - mSagAttackCoef) * detector;
          else
            mSagEnv[ch] = mSagReleaseCoef * mSagEnv[ch] + (1.0f - mSagReleaseCoef) * detector;

          const float sagGain = 1.0f / (1.0f + mSag * mSagEnv[ch]);
          powerInput *= sagGain;

          const float powerGain = 1.0f + 6.0f * mPowerDrive;
          float powered = SoftClip(powerInput * powerGain);

          double speaker = powered;
          speaker = ProcessBiquad(speaker, mDepthB0, mDepthB1, mDepthB2, mDepthA1, mDepthA2,
                                  mDepthS1[ch], mDepthS2[ch]);
          speaker = ProcessBiquad(speaker, mResonanceB0, mResonanceB1, mResonanceB2, mResonanceA1, mResonanceA2,
                                  mResonanceS1[ch], mResonanceS2[ch]);
          speaker = ProcessBiquad(speaker, mDampingB0, mDampingB1, mDampingB2, mDampingA1, mDampingA2,
                                  mDampingS1[ch], mDampingS2[ch]);

          speaker = ProcessBiquad(speaker, mPostHPB0, mPostHPB1, mPostHPB2, mPostHPA1, mPostHPA2,
                                  mPostHPS1[ch], mPostHPS2[ch]);

          out[i] = static_cast<float>(speaker) * outputGain;
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
      else if (key == "bright")
      {
        mBright = (value >= 0.5) ? 1.0f : 0.0f;
        UpdatePreEmphasis();
      }
      else if (key == "preEmphasis")
      {
        mPreEmphasis = static_cast<float>(std::clamp(value, 0.0, 1.0));
        UpdatePreEmphasis();
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
      else if (key == "stageGain" || key == "stage1Gain" || key == "stage2Gain" || key == "stage3Gain"
               || key == "stage4Gain" || key == "stage5Gain" || key == "stage6Gain")
      {
        SetStageGain(value);
      }
      else if (key == "powerDrive")
      {
        mPowerDrive = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "sag")
      {
        mSag = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "bias")
      {
        mBias = static_cast<float>(std::clamp(value, -1.0, 1.0));
      }
      else if (key == "depth")
      {
        mDepth = static_cast<float>(std::clamp(value, 0.0, 1.0));
        UpdateSpeakerFilters();
      }
      else if (key == "resonance")
      {
        mResonance = static_cast<float>(std::clamp(value, 0.0, 1.0));
        UpdateSpeakerFilters();
      }
      else if (key == "damping")
      {
        mDamping = static_cast<float>(std::clamp(value, 0.0, 1.0));
        UpdateSpeakerFilters();
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "voice")
        return mVoice;
      if (key == "gain")
        return mGain;
      if (key == "bright")
        return mBright;
      if (key == "preEmphasis")
        return mPreEmphasis;
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
      if (key == "stageGain" || key == "stage1Gain" || key == "stage2Gain" || key == "stage3Gain"
          || key == "stage4Gain" || key == "stage5Gain" || key == "stage6Gain")
        return mStageGainDb;
      if (key == "powerDrive")
        return mPowerDrive;
      if (key == "sag")
        return mSag;
      if (key == "bias")
        return mBias;
      if (key == "depth")
        return mDepth;
      if (key == "resonance")
        return mResonance;
      if (key == "damping")
        return mDamping;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "amp_builtin"; }
    [[nodiscard]] std::string GetCategory() const override { return "amp"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr int kMaxStages = 4;
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

    void SetStageGain(double value)
    {
      const double clamped = std::clamp(value, -24.0, 24.0);
      mStageGainDb = clamped;
      mStageGainLinear = DbToLinear(clamped);
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

    void UpdateSagCoefficients()
    {
      if (mSampleRate <= 0.0)
      {
        mSagAttackCoef = 0.0f;
        mSagReleaseCoef = 0.0f;
        return;
      }
      const double attackTau = 0.01;  // 10 ms
      const double releaseTau = 0.18; // 180 ms
      mSagAttackCoef = static_cast<float>(std::exp(-1.0 / (attackTau * mSampleRate)));
      mSagReleaseCoef = static_cast<float>(std::exp(-1.0 / (releaseTau * mSampleRate)));
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

    void UpdatePreEmphasis()
    {
      if (mSampleRate <= 0.0)
        return;

      const double brightBoost = (mBright > 0.5f) ? 3.0 : 0.0;
      const double emphasisBoost = static_cast<double>(mPreEmphasis) * 6.0;
      const double gainDb = brightBoost + emphasisBoost;

      ComputeHighShelf(2500.0, 0.8, gainDb, mPreEmphB0, mPreEmphB1, mPreEmphB2, mPreEmphA1, mPreEmphA2);
    }

    void UpdateSpeakerFilters()
    {
      if (mSampleRate <= 0.0)
        return;

      const double depthGain = static_cast<double>(mDepth) * 6.0;
      const double resonanceGain = static_cast<double>(mResonance) * 6.0;
      const double dampingGain = static_cast<double>(mDamping) * -6.0;

      ComputeLowShelf(120.0, 0.9, depthGain, mDepthB0, mDepthB1, mDepthB2, mDepthA1, mDepthA2);
      ComputePeakingEQ(120.0, 1.0, resonanceGain, mResonanceB0, mResonanceB1, mResonanceB2,
                       mResonanceA1, mResonanceA2);
      ComputeHighShelf(3500.0, 0.9, dampingGain, mDampingB0, mDampingB1, mDampingB2, mDampingA1, mDampingA2);
    }

    void UpdatePostFilters()
    {
      if (mSampleRate <= 0.0)
        return;

      ComputeHighPass(25.0, 0.707, mPostHPB0, mPostHPB1, mPostHPB2, mPostHPA1, mPostHPA2);
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
    double mStageGainDb = 0.0;
    float mStageGainLinear = 1.0f;
    float mBright = 0.0f;
    float mPreEmphasis = 0.0f;
    float mPowerDrive = 0.0f;
    float mSag = 0.0f;
    float mBias = 0.0f;
    float mDepth = 0.4f;
    float mResonance = 0.4f;
    float mDamping = 0.5f;
    float mSagAttackCoef = 0.0f;
    float mSagReleaseCoef = 0.0f;

    double mPreHPB0 = 1.0, mPreHPB1 = 0.0, mPreHPB2 = 0.0, mPreHPA1 = 0.0, mPreHPA2 = 0.0;
    double mPreEmphB0 = 1.0, mPreEmphB1 = 0.0, mPreEmphB2 = 0.0, mPreEmphA1 = 0.0, mPreEmphA2 = 0.0;
    double mDepthB0 = 1.0, mDepthB1 = 0.0, mDepthB2 = 0.0, mDepthA1 = 0.0, mDepthA2 = 0.0;
    double mResonanceB0 = 1.0, mResonanceB1 = 0.0, mResonanceB2 = 0.0, mResonanceA1 = 0.0, mResonanceA2 = 0.0;
    double mDampingB0 = 1.0, mDampingB1 = 0.0, mDampingB2 = 0.0, mDampingA1 = 0.0, mDampingA2 = 0.0;
    double mPostHPB0 = 1.0, mPostHPB1 = 0.0, mPostHPB2 = 0.0, mPostHPA1 = 0.0, mPostHPA2 = 0.0;

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
    std::array<double, 2> mPreEmphS1 = {}, mPreEmphS2 = {};
    std::array<double, 2> mDepthS1 = {}, mDepthS2 = {};
    std::array<double, 2> mResonanceS1 = {}, mResonanceS2 = {};
    std::array<double, 2> mDampingS1 = {}, mDampingS2 = {};
    std::array<double, 2> mPostHPS1 = {}, mPostHPS2 = {};
    std::array<float, 2> mSagEnv = {0.0f, 0.0f};
  };

  inline void RegisterBuiltinAmpEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kAmpBuiltin;
    info.aliases = {"amp_builtin"};
    info.displayName = "Heavy American";
    info.category = "amp";
    info.description = "Built-in amp with smooth Clean/Drive voice and modern tone controls";
    info.requiresResource = false;
    info.parameters = {
      {"voice",       "Voice",        0.0,   0.0,   1.0,  "toggle", "Input"},
      {"gain",        "Gain",         0.45,  0.0,   1.0,  "amount", "Input"},
      {"bright",      "Bright",       0.0,   0.0,   1.0,  "toggle", "Input"},
      {"preEmphasis", "Pre Emphasis", 0.0,   0.0,   1.0,  "amount", "Input",   true},
      {"stageCount",  "Gain Stages",  2.0,   1.0,   4.0,  "amount", "Input",   false, 1.0},
      {"stageGain",   "Gain Per Stage",   0.0,  -24.0,  24.0, "dB",     "Input"},
      {"bass",        "Bass",         0.5,   0.0,   1.0,  "amount", "Tone"},
      {"middle",      "Middle",       0.5,   0.0,   1.0,  "amount", "Tone"},
      {"treble",      "Treble",       0.5,   0.0,   1.0,  "amount", "Tone"},
      {"contour",     "Contour",      0.2,   0.0,   1.0,  "amount", "Tone"},
      {"presence",    "Presence",     0.5,   0.0,   1.0,  "amount", "Tone"},
      {"output",      "Output",       0.0,  -24.0,  24.0, "dB",     "Output"},
      {"powerDrive",  "Power Drive",  0.0,   0.0,   1.0,  "amount", "Power",   true},
      {"sag",         "Sag",          0.0,   0.0,   1.0,  "amount", "Power",   true},
      {"bias",        "Bias",         0.0,  -1.0,   1.0,  "amount", "Power",   true},
      {"depth",       "Depth",        0.4,   0.0,   1.0,  "amount", "Speaker", true},
      {"resonance",   "Resonance",    0.4,   0.0,   1.0,  "amount", "Speaker", true},
      {"damping",     "Damping",      0.5,   0.0,   1.0,  "amount", "Speaker", true}
    };

    EffectRegistry::Instance().Register(info.type, info, []()
      { return std::make_unique<BuiltinAmpEffect>(); });
  }

} // namespace guitarfx
