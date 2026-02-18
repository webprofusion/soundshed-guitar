#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace guitarfx
{
  class ReverbEffect : public EffectProcessor
  {
  public:
    enum class Mode
    {
      Room,
      Hall,
      Plate,
      Chamber,
      Spring,
      Shimmer,
      Ambient,
      Advanced
    };

    explicit ReverbEffect(Mode mode)
        : mMode(mode)
    {
      ApplyModeDefaults();
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      const auto profile = GetProfile(mMode);
      constexpr double maxSizeScale = 2.3;

      const size_t maxPreDelaySamples = DelayMsToSamples(kMaxPreDelayMs + kMaxTapMs + 4.0);
      mPreDelayL.assign(maxPreDelaySamples, 0.0f);
      mPreDelayR.assign(maxPreDelaySamples, 0.0f);
      mPreDelayWrite = 0;

      for (size_t i = 0; i < kCombCount; ++i)
      {
        const size_t lenL = DelayMsToSamples(profile.combMsL[i] * maxSizeScale + 3.0);
        const size_t lenR = DelayMsToSamples(profile.combMsR[i] * maxSizeScale + 3.0);
        mCombBufferL[i].assign(lenL, 0.0f);
        mCombBufferR[i].assign(lenR, 0.0f);
        mCombWriteL[i] = 0;
        mCombWriteR[i] = 0;
        mCombFilterStoreL[i] = 0.0f;
        mCombFilterStoreR[i] = 0.0f;
      }

      for (size_t i = 0; i < kAllpassCount; ++i)
      {
        const size_t lenL = DelayMsToSamples(profile.allpassMsL[i] * maxSizeScale + 3.0);
        const size_t lenR = DelayMsToSamples(profile.allpassMsR[i] * maxSizeScale + 3.0);
        mAllpassBufferL[i].assign(lenL, 0.0f);
        mAllpassBufferR[i].assign(lenR, 0.0f);
        mAllpassWriteL[i] = 0;
        mAllpassWriteR[i] = 0;
      }

      UpdateParameters();
      Reset();
    }

    void Reset() override
    {
      std::fill(mPreDelayL.begin(), mPreDelayL.end(), 0.0f);
      std::fill(mPreDelayR.begin(), mPreDelayR.end(), 0.0f);
      mPreDelayWrite = 0;

      for (size_t i = 0; i < kCombCount; ++i)
      {
        std::fill(mCombBufferL[i].begin(), mCombBufferL[i].end(), 0.0f);
        std::fill(mCombBufferR[i].begin(), mCombBufferR[i].end(), 0.0f);
        mCombWriteL[i] = 0;
        mCombWriteR[i] = 0;
        mCombFilterStoreL[i] = 0.0f;
        mCombFilterStoreR[i] = 0.0f;
      }

      for (size_t i = 0; i < kAllpassCount; ++i)
      {
        std::fill(mAllpassBufferL[i].begin(), mAllpassBufferL[i].end(), 0.0f);
        std::fill(mAllpassBufferR[i].begin(), mAllpassBufferR[i].end(), 0.0f);
        mAllpassWriteL[i] = 0;
        mAllpassWriteR[i] = 0;
      }

      mLowpassStateL = 0.0f;
      mLowpassStateR = 0.0f;
      mHpPrevInL = 0.0f;
      mHpPrevInR = 0.0f;
      mHpPrevOutL = 0.0f;
      mHpPrevOutR = 0.0f;
      mDuckEnv = 0.0f;
      mModPhase = 0.0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!outputs || numSamples <= 0)
        return;

      if (mCombBufferL[0].empty() || mPreDelayL.empty())
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
      const float width = static_cast<float>(mWidth);
      const float drive = static_cast<float>(mDrive);
      const float duckAmount = static_cast<float>(mDucking);

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs && inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs && inputs[1] ? inputs[1][i] : inL;

        const float inputLevel = 0.5f * (std::fabs(inL) + std::fabs(inR));
        const float duckCoeff = (inputLevel > mDuckEnv) ? mDuckAttackCoeff : mDuckReleaseCoeff;
        mDuckEnv += (inputLevel - mDuckEnv) * duckCoeff;
        const float duckGain = 1.0f - duckAmount * std::clamp(mDuckEnv * 1.4f, 0.0f, 1.0f);

        const float monoIn = 0.5f * (inL + inR);
        const float sideIn = 0.5f * (inL - inR);
        const float shapedL = monoIn + sideIn * width;
        const float shapedR = monoIn - sideIn * width;

        mPreDelayL[mPreDelayWrite] = shapedL;
        mPreDelayR[mPreDelayWrite] = shapedR;

        const float preL = ReadFromDelay(mPreDelayL, mPreDelayWrite, mPreDelaySamples);
        const float preR = ReadFromDelay(mPreDelayR, mPreDelayWrite, mPreDelaySamples);

        float earlyL = preL * 0.55f;
        float earlyR = preR * 0.55f;
        for (size_t t = 0; t < kEarlyTapCount; ++t)
        {
          earlyL += ReadFromDelay(mPreDelayL, mPreDelayWrite, mPreDelaySamples + mEarlyTapSamples[t]) * mEarlyTapGains[t];
          earlyR += ReadFromDelay(mPreDelayR, mPreDelayWrite, mPreDelaySamples + mEarlyTapSamplesMirror[t]) * mEarlyTapGains[t];
        }

        if (++mPreDelayWrite >= mPreDelayL.size())
          mPreDelayWrite = 0;

        const float mod = static_cast<float>(std::sin(mModPhase));
        mModPhase += mModPhaseInc;
        if (mModPhase >= kTwoPi)
          mModPhase -= kTwoPi;

        float tankInL = ApplyDrive(preL * (1.0f - mEarlyMix) + earlyL * mEarlyMix, drive);
        float tankInR = ApplyDrive(preR * (1.0f - mEarlyMix) + earlyR * mEarlyMix, drive);

        tankInL += mod * mModDepth;
        tankInR -= mod * mModDepth;

        float combOutL = 0.0f;
        float combOutR = 0.0f;

        for (size_t c = 0; c < kCombCount; ++c)
        {
          const float delayedL = ReadFromDelay(mCombBufferL[c], mCombWriteL[c], mCombDelaySamplesL[c]);
          const float delayedR = ReadFromDelay(mCombBufferR[c], mCombWriteR[c], mCombDelaySamplesR[c]);

          mCombFilterStoreL[c] = delayedL * (1.0f - mDamp) + mCombFilterStoreL[c] * mDamp;
          mCombFilterStoreR[c] = delayedR * (1.0f - mDamp) + mCombFilterStoreR[c] * mDamp;

          mCombBufferL[c][mCombWriteL[c]] = tankInL + mCombFilterStoreL[c] * mFeedback;
          mCombBufferR[c][mCombWriteR[c]] = tankInR + mCombFilterStoreR[c] * mFeedback;

          if (++mCombWriteL[c] >= mCombBufferL[c].size())
            mCombWriteL[c] = 0;
          if (++mCombWriteR[c] >= mCombBufferR[c].size())
            mCombWriteR[c] = 0;

          combOutL += delayedL;
          combOutR += delayedR;
        }

        float wetL = combOutL * mCombGain;
        float wetR = combOutR * mCombGain;

        for (size_t a = 0; a < kAllpassCount; ++a)
        {
          const float delayedL = ReadFromDelay(mAllpassBufferL[a], mAllpassWriteL[a], mAllpassDelaySamplesL[a]);
          const float delayedR = ReadFromDelay(mAllpassBufferR[a], mAllpassWriteR[a], mAllpassDelaySamplesR[a]);

          const float inputAPF_L = wetL;
          const float inputAPF_R = wetR;

          wetL = delayedL - inputAPF_L * mDiffusionGain;
          wetR = delayedR - inputAPF_R * mDiffusionGain;

          mAllpassBufferL[a][mAllpassWriteL[a]] = inputAPF_L + delayedL * mDiffusionGain;
          mAllpassBufferR[a][mAllpassWriteR[a]] = inputAPF_R + delayedR * mDiffusionGain;

          if (++mAllpassWriteL[a] >= mAllpassBufferL[a].size())
            mAllpassWriteL[a] = 0;
          if (++mAllpassWriteR[a] >= mAllpassBufferR[a].size())
            mAllpassWriteR[a] = 0;
        }

        if (mMode == Mode::Shimmer)
        {
          const float sparkle = 0.35f + static_cast<float>(mTone) * 0.45f;
          wetL += std::fabs(wetL) * sparkle * mShimmerAmount;
          wetR += std::fabs(wetR) * sparkle * mShimmerAmount;
        }

        wetL = ProcessFiltersL(wetL);
        wetR = ProcessFiltersR(wetR);

        const float stereoMid = 0.5f * (wetL + wetR);
        const float stereoSide = 0.5f * (wetL - wetR) * width;
        wetL = stereoMid + stereoSide;
        wetR = stereoMid - stereoSide;

        wetL *= duckGain;
        wetR *= duckGain;

        const float mixedL = inL * dry + wetL * wet;
        const float mixedR = inR * dry + wetR * wet;

        if (outputs[0])
          outputs[0][i] = mixedL;
        if (outputs[1])
          outputs[1][i] = mixedR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "decay")
      {
        mDecay = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "size")
      {
        mSize = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "damping")
      {
        mDamping = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "diffusion")
      {
        mDiffusion = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "preDelay")
      {
        mPreDelayMs = std::clamp(value, 0.0, kMaxPreDelayMs);
        UpdateParameters();
      }
      else if (key == "lowCut")
      {
        mLowCutHz = std::clamp(value, 20.0, 1200.0);
        UpdateParameters();
      }
      else if (key == "highCut")
      {
        mHighCutHz = std::clamp(value, 1000.0, 20000.0);
        UpdateParameters();
      }
      else if (key == "tone")
      {
        mTone = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "width")
      {
        mWidth = std::clamp(value, 0.0, 1.2);
      }
      else if (key == "modRate")
      {
        mModRateHz = std::clamp(value, 0.02, 8.0);
        UpdateParameters();
      }
      else if (key == "modDepth")
      {
        mModDepth = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "ducking")
      {
        mDucking = std::clamp(value, 0.0, 1.0);
      }
      else if (key == "drive")
      {
        mDrive = std::clamp(value, 0.0, 1.0);
      }
      else if (key == "shimmer")
      {
        mShimmer = std::clamp(value, 0.0, 1.0);
        UpdateParameters();
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "decay")
        return mDecay;
      if (key == "size")
        return mSize;
      if (key == "damping")
        return mDamping;
      if (key == "diffusion")
        return mDiffusion;
      if (key == "preDelay")
        return mPreDelayMs;
      if (key == "lowCut")
        return mLowCutHz;
      if (key == "highCut")
        return mHighCutHz;
      if (key == "tone")
        return mTone;
      if (key == "width")
        return mWidth;
      if (key == "modRate")
        return mModRateHz;
      if (key == "modDepth")
        return mModDepth;
      if (key == "ducking")
        return mDucking;
      if (key == "drive")
        return mDrive;
      if (key == "shimmer")
        return mShimmer;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
      switch (mMode)
      {
      case Mode::Hall:
        return "reverb_hall";
      case Mode::Plate:
        return "reverb_plate";
      case Mode::Chamber:
        return "reverb_chamber";
      case Mode::Spring:
        return "reverb_spring";
      case Mode::Shimmer:
        return "reverb_shimmer";
      case Mode::Ambient:
        return "reverb_ambient";
      case Mode::Advanced:
        return "reverb_advanced";
      case Mode::Room:
      default:
        return "reverb_room";
      }
    }

    [[nodiscard]] std::string GetCategory() const override { return "reverb"; }

    static std::string ModeName(Mode mode)
    {
      switch (mode)
      {
      case Mode::Hall:
        return "Hall";
      case Mode::Plate:
        return "Plate";
      case Mode::Chamber:
        return "Chamber";
      case Mode::Spring:
        return "Spring";
      case Mode::Shimmer:
        return "Shimmer";
      case Mode::Ambient:
        return "Ambient";
      case Mode::Advanced:
        return "Advanced";
      case Mode::Room:
      default:
        return "Room";
      }
    }

    static std::string ModeType(Mode mode)
    {
      switch (mode)
      {
      case Mode::Hall:
        return "reverb_hall";
      case Mode::Plate:
        return "reverb_plate";
      case Mode::Chamber:
        return "reverb_chamber";
      case Mode::Spring:
        return "reverb_spring";
      case Mode::Shimmer:
        return "reverb_shimmer";
      case Mode::Ambient:
        return "reverb_ambient";
      case Mode::Advanced:
        return "reverb_advanced";
      case Mode::Room:
      default:
        return "reverb_room";
      }
    }

  private:
    static constexpr size_t kCombCount = 8;
    static constexpr size_t kAllpassCount = 4;
    static constexpr size_t kEarlyTapCount = 4;
    static constexpr double kMaxPreDelayMs = 220.0;
    static constexpr double kMaxTapMs = 46.0;
    static constexpr double kTwoPi = 6.2831853071795864769;

    struct ModeProfile
    {
      std::array<double, kCombCount> combMsL;
      std::array<double, kCombCount> combMsR;
      std::array<double, kAllpassCount> allpassMsL;
      std::array<double, kAllpassCount> allpassMsR;
      std::array<double, kEarlyTapCount> earlyTapMs;
      std::array<float, kEarlyTapCount> earlyTapGains;
      double decayBias = 1.0;
      double dampBias = 0.5;
      double diffusionBias = 0.5;
      double modulationBias = 0.5;
      double shimmerBias = 0.0;
      double driveBias = 0.0;
      double earlyMixBias = 0.35;
    };

    static ModeProfile GetProfile(Mode mode)
    {
      switch (mode)
      {
      case Mode::Hall:
        return {
            {35.1, 38.7, 41.9, 45.7, 49.2, 53.1, 57.4, 61.8},
            {36.3, 39.4, 42.8, 46.4, 50.1, 54.0, 58.6, 63.2},
            {8.1, 12.7, 18.2, 24.3},
            {9.0, 13.9, 19.5, 25.6},
            {11.0, 18.0, 29.0, 41.0},
            {0.33f, 0.23f, 0.17f, 0.12f},
            1.25, 0.45, 0.75, 0.65, 0.0, 0.0, 0.28};
      case Mode::Plate:
        return {
            {22.5, 24.2, 26.9, 28.8, 31.5, 34.7, 37.4, 40.6},
            {23.0, 25.1, 27.4, 29.7, 32.6, 35.8, 38.6, 41.9},
            {5.2, 7.8, 10.4, 13.9},
            {5.8, 8.3, 11.1, 14.5},
            {6.0, 10.0, 15.0, 22.0},
            {0.42f, 0.26f, 0.19f, 0.12f},
            1.05, 0.32, 0.82, 0.55, 0.08, 0.15, 0.18};
      case Mode::Chamber:
        return {
            {26.7, 29.8, 33.4, 36.9, 39.8, 43.6, 47.5, 50.9},
            {27.4, 30.6, 34.1, 37.6, 40.7, 44.4, 48.3, 51.8},
            {6.1, 9.1, 12.8, 17.2},
            {6.8, 9.8, 13.5, 17.9},
            {8.0, 13.0, 21.0, 31.0},
            {0.38f, 0.24f, 0.17f, 0.12f},
            1.1, 0.52, 0.68, 0.42, 0.0, 0.05, 0.36};
      case Mode::Spring:
        return {
            {18.2, 20.7, 24.3, 27.0, 30.1, 32.8, 35.5, 38.4},
            {18.7, 21.4, 24.8, 27.7, 30.7, 33.2, 36.1, 39.1},
            {3.1, 5.2, 7.3, 9.7},
            {3.6, 5.8, 7.9, 10.1},
            {4.0, 7.0, 11.0, 16.0},
            {0.45f, 0.27f, 0.17f, 0.11f},
            0.9, 0.28, 0.52, 0.32, 0.0, 0.35, 0.22};
      case Mode::Shimmer:
        return {
            {38.1, 42.3, 46.8, 50.6, 55.1, 59.8, 64.9, 69.8},
            {39.4, 43.7, 47.9, 52.1, 56.6, 61.4, 66.0, 71.1},
            {9.1, 14.0, 19.3, 26.1},
            {10.0, 15.1, 20.7, 27.6},
            {13.0, 21.0, 34.0, 46.0},
            {0.30f, 0.23f, 0.18f, 0.13f},
            1.32, 0.36, 0.78, 0.84, 0.45, 0.0, 0.24};
      case Mode::Ambient:
        return {
            {45.8, 50.4, 56.2, 61.9, 67.5, 73.1, 79.6, 85.4},
            {47.0, 52.1, 57.4, 63.2, 69.1, 74.9, 80.8, 86.9},
            {11.7, 17.6, 24.9, 33.2},
            {12.6, 18.8, 26.3, 34.4},
            {14.0, 24.0, 38.0, 46.0},
            {0.31f, 0.24f, 0.18f, 0.14f},
            1.4, 0.42, 0.9, 0.9, 0.18, 0.0, 0.26};
      case Mode::Advanced:
        return {
            {31.4, 34.5, 37.9, 41.3, 45.0, 48.4, 52.2, 56.0},
            {32.0, 35.2, 38.7, 42.1, 45.8, 49.4, 53.1, 57.0},
            {7.2, 10.8, 15.0, 20.3},
            {7.8, 11.4, 15.7, 21.1},
            {9.0, 15.0, 24.0, 35.0},
            {0.36f, 0.24f, 0.18f, 0.13f},
            1.18, 0.44, 0.78, 0.62, 0.12, 0.08, 0.3};
      case Mode::Room:
      default:
        return {
            {19.7, 22.1, 24.9, 27.8, 30.5, 33.2, 35.7, 38.1},
            {20.3, 22.8, 25.4, 28.5, 31.3, 33.9, 36.4, 39.2},
            {4.8, 6.8, 9.1, 12.2},
            {5.2, 7.3, 9.8, 12.9},
            {5.0, 9.0, 14.0, 20.0},
            {0.40f, 0.25f, 0.19f, 0.13f},
            0.95, 0.52, 0.56, 0.3, 0.0, 0.0, 0.4};
      }
    }

    size_t DelayMsToSamples(double ms) const
    {
      const double samples = ms * mSampleRate * 0.001;
      return std::max<size_t>(1, static_cast<size_t>(samples));
    }

    static float ReadFromDelay(const std::vector<float> &buffer, size_t writePos, size_t delaySamples)
    {
      if (buffer.empty())
        return 0.0f;
      const size_t back = std::min(delaySamples, buffer.size() - 1);
      const size_t readPos = (writePos + buffer.size() - back) % buffer.size();
      return buffer[readPos];
    }

    float ProcessFiltersL(float sample)
    {
      mLowpassStateL += mLowpassAlpha * (sample - mLowpassStateL);
      const float hpOut = mHighpassAlpha * (mHpPrevOutL + mLowpassStateL - mHpPrevInL);
      mHpPrevInL = mLowpassStateL;
      mHpPrevOutL = hpOut;
      return hpOut;
    }

    float ProcessFiltersR(float sample)
    {
      mLowpassStateR += mLowpassAlpha * (sample - mLowpassStateR);
      const float hpOut = mHighpassAlpha * (mHpPrevOutR + mLowpassStateR - mHpPrevInR);
      mHpPrevInR = mLowpassStateR;
      mHpPrevOutR = hpOut;
      return hpOut;
    }

    void UpdateParameters()
    {
      const auto profile = GetProfile(mMode);

      const double sizeScale = 0.6 + mSize * 1.4;
      for (size_t i = 0; i < kCombCount; ++i)
      {
        mCombDelaySamplesL[i] = std::max<size_t>(1, DelayMsToSamples(profile.combMsL[i] * sizeScale));
        mCombDelaySamplesR[i] = std::max<size_t>(1, DelayMsToSamples(profile.combMsR[i] * sizeScale));
        mCombDelaySamplesL[i] = std::min(mCombDelaySamplesL[i], mCombBufferL[i].empty() ? size_t(1) : mCombBufferL[i].size() - 1);
        mCombDelaySamplesR[i] = std::min(mCombDelaySamplesR[i], mCombBufferR[i].empty() ? size_t(1) : mCombBufferR[i].size() - 1);
      }

      for (size_t i = 0; i < kAllpassCount; ++i)
      {
        mAllpassDelaySamplesL[i] = std::max<size_t>(1, DelayMsToSamples(profile.allpassMsL[i] * sizeScale));
        mAllpassDelaySamplesR[i] = std::max<size_t>(1, DelayMsToSamples(profile.allpassMsR[i] * sizeScale));
        mAllpassDelaySamplesL[i] = std::min(mAllpassDelaySamplesL[i], mAllpassBufferL[i].empty() ? size_t(1) : mAllpassBufferL[i].size() - 1);
        mAllpassDelaySamplesR[i] = std::min(mAllpassDelaySamplesR[i], mAllpassBufferR[i].empty() ? size_t(1) : mAllpassBufferR[i].size() - 1);
      }

      for (size_t t = 0; t < kEarlyTapCount; ++t)
      {
        mEarlyTapSamples[t] = DelayMsToSamples(profile.earlyTapMs[t] * (0.9 + 0.4 * mSize));
        mEarlyTapSamplesMirror[t] = DelayMsToSamples((profile.earlyTapMs[t] + 0.9 + static_cast<double>(t) * 0.7) * (0.9 + 0.4 * mSize));
        mEarlyTapSamples[t] = std::min(mEarlyTapSamples[t], mPreDelayL.empty() ? size_t(1) : mPreDelayL.size() - 1);
        mEarlyTapSamplesMirror[t] = std::min(mEarlyTapSamplesMirror[t], mPreDelayL.empty() ? size_t(1) : mPreDelayL.size() - 1);
        mEarlyTapGains[t] = profile.earlyTapGains[t];
      }

      mPreDelaySamples = DelayMsToSamples(mPreDelayMs);
      mPreDelaySamples = std::min(mPreDelaySamples, mPreDelayL.empty() ? size_t(1) : mPreDelayL.size() - 1);

      mFeedback = static_cast<float>(std::clamp(0.56 + (mDecay * profile.decayBias) * 0.4, 0.3, 0.985));

      const double dampBase = std::clamp(0.02 + (mDamping * 0.62) + (1.0 - mTone) * 0.26 + profile.dampBias * 0.08, 0.02, 0.96);
      mDamp = static_cast<float>(dampBase);

      mDiffusionGain = static_cast<float>(std::clamp(0.28 + mDiffusion * 0.58 + profile.diffusionBias * 0.08, 0.2, 0.92));
      mCombGain = 1.0f / static_cast<float>(kCombCount);

      const double modBias = std::clamp(profile.modulationBias + mModDepth * 0.8, 0.0, 1.5);
      mModPhaseInc = kTwoPi * std::clamp(mModRateHz, 0.02, 8.0) / std::max(1.0, mSampleRate);
      mModDepth = std::clamp(0.0002 + mModDepth * 0.004 * modBias, 0.0, 0.02);

      const double lowCut = std::clamp(mLowCutHz, 20.0, 1200.0);
      const double highCut = std::clamp(mHighCutHz * (0.65 + mTone * 0.7), 1000.0, 20000.0);
      const double dt = 1.0 / std::max(1.0, mSampleRate);
      const double rcLow = 1.0 / (2.0 * 3.14159265358979323846 * lowCut);
      const double rcHigh = 1.0 / (2.0 * 3.14159265358979323846 * highCut);
      mHighpassAlpha = static_cast<float>(rcLow / (rcLow + dt));
      mLowpassAlpha = static_cast<float>(dt / (rcHigh + dt));

      mShimmerAmount = std::clamp(mShimmer * (profile.shimmerBias + 0.35), 0.0, 1.0);
      mEarlyMix = static_cast<float>(std::clamp(profile.earlyMixBias + mDiffusion * 0.18, 0.08, 0.62));

      const double duckAttack = 0.006;
      const double duckRelease = 0.11;
      mDuckAttackCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * duckAttack)));
      mDuckReleaseCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * duckRelease)));
    }

    void ApplyModeDefaults()
    {
      switch (mMode)
      {
      case Mode::Hall:
        mDecay = 0.76;
        mSize = 0.7;
        mDamping = 0.46;
        mDiffusion = 0.72;
        mPreDelayMs = 24.0;
        mLowCutHz = 120.0;
        mHighCutHz = 12000.0;
        mTone = 0.62;
        mWidth = 1.05;
        mModRateHz = 0.45;
        mModDepth = 0.24;
        mDucking = 0.12;
        mDrive = 0.0;
        mShimmer = 0.0;
        mMix = 0.27;
        break;
      case Mode::Plate:
        mDecay = 0.58;
        mSize = 0.52;
        mDamping = 0.35;
        mDiffusion = 0.84;
        mPreDelayMs = 12.0;
        mLowCutHz = 160.0;
        mHighCutHz = 14500.0;
        mTone = 0.76;
        mWidth = 0.95;
        mModRateHz = 0.62;
        mModDepth = 0.14;
        mDucking = 0.08;
        mDrive = 0.12;
        mShimmer = 0.0;
        mMix = 0.24;
        break;
      case Mode::Chamber:
        mDecay = 0.6;
        mSize = 0.56;
        mDamping = 0.54;
        mDiffusion = 0.66;
        mPreDelayMs = 15.0;
        mLowCutHz = 140.0;
        mHighCutHz = 9800.0;
        mTone = 0.52;
        mWidth = 0.9;
        mModRateHz = 0.33;
        mModDepth = 0.1;
        mDucking = 0.1;
        mDrive = 0.04;
        mShimmer = 0.0;
        mMix = 0.24;
        break;
      case Mode::Spring:
        mDecay = 0.5;
        mSize = 0.38;
        mDamping = 0.22;
        mDiffusion = 0.52;
        mPreDelayMs = 5.0;
        mLowCutHz = 180.0;
        mHighCutHz = 8300.0;
        mTone = 0.68;
        mWidth = 0.76;
        mModRateHz = 1.2;
        mModDepth = 0.09;
        mDucking = 0.06;
        mDrive = 0.22;
        mShimmer = 0.0;
        mMix = 0.23;
        break;
      case Mode::Shimmer:
        mDecay = 0.84;
        mSize = 0.8;
        mDamping = 0.28;
        mDiffusion = 0.82;
        mPreDelayMs = 28.0;
        mLowCutHz = 170.0;
        mHighCutHz = 18000.0;
        mTone = 0.9;
        mWidth = 1.15;
        mModRateHz = 0.32;
        mModDepth = 0.34;
        mDucking = 0.18;
        mDrive = 0.0;
        mShimmer = 0.45;
        mMix = 0.30;
        break;
      case Mode::Ambient:
        mDecay = 0.86;
        mSize = 0.9;
        mDamping = 0.42;
        mDiffusion = 0.9;
        mPreDelayMs = 34.0;
        mLowCutHz = 120.0;
        mHighCutHz = 11000.0;
        mTone = 0.65;
        mWidth = 1.1;
        mModRateHz = 0.18;
        mModDepth = 0.44;
        mDucking = 0.22;
        mDrive = 0.0;
        mShimmer = 0.12;
        mMix = 0.32;
        break;
      case Mode::Advanced:
        mDecay = 0.64;
        mSize = 0.55;
        mDamping = 0.46;
        mDiffusion = 0.7;
        mPreDelayMs = 16.0;
        mLowCutHz = 140.0;
        mHighCutHz = 12000.0;
        mTone = 0.62;
        mWidth = 1.0;
        mModRateHz = 0.45;
        mModDepth = 0.18;
        mDucking = 0.08;
        mDrive = 0.0;
        mShimmer = 0.0;
        mMix = 0.24;
        break;
      case Mode::Room:
      default:
        mDecay = 0.46;
        mSize = 0.42;
        mDamping = 0.56;
        mDiffusion = 0.54;
        mPreDelayMs = 8.0;
        mLowCutHz = 170.0;
        mHighCutHz = 9800.0;
        mTone = 0.5;
        mWidth = 0.88;
        mModRateHz = 0.5;
        mModDepth = 0.07;
        mDucking = 0.08;
        mDrive = 0.0;
        mShimmer = 0.0;
        mMix = 0.22;
        break;
      }
    }

    static float ApplyDrive(float sample, float amount)
    {
      if (amount <= 0.0f)
        return sample;
      const float drive = 1.0f + amount * 5.0f;
      const float norm = static_cast<float>(std::tanh(drive));
      if (norm <= 0.0f)
        return sample;
      return static_cast<float>(std::tanh(sample * drive)) / norm;
    }

    Mode mMode;

    std::vector<float> mPreDelayL;
    std::vector<float> mPreDelayR;
    size_t mPreDelayWrite = 0;

    std::array<std::vector<float>, kCombCount> mCombBufferL;
    std::array<std::vector<float>, kCombCount> mCombBufferR;
    std::array<size_t, kCombCount> mCombWriteL = {};
    std::array<size_t, kCombCount> mCombWriteR = {};
    std::array<size_t, kCombCount> mCombDelaySamplesL = {};
    std::array<size_t, kCombCount> mCombDelaySamplesR = {};
    std::array<float, kCombCount> mCombFilterStoreL = {};
    std::array<float, kCombCount> mCombFilterStoreR = {};

    std::array<std::vector<float>, kAllpassCount> mAllpassBufferL;
    std::array<std::vector<float>, kAllpassCount> mAllpassBufferR;
    std::array<size_t, kAllpassCount> mAllpassWriteL = {};
    std::array<size_t, kAllpassCount> mAllpassWriteR = {};
    std::array<size_t, kAllpassCount> mAllpassDelaySamplesL = {};
    std::array<size_t, kAllpassCount> mAllpassDelaySamplesR = {};

    std::array<size_t, kEarlyTapCount> mEarlyTapSamples = {};
    std::array<size_t, kEarlyTapCount> mEarlyTapSamplesMirror = {};
    std::array<float, kEarlyTapCount> mEarlyTapGains = {};
    size_t mPreDelaySamples = 1;

    float mLowpassStateL = 0.0f;
    float mLowpassStateR = 0.0f;
    float mHpPrevInL = 0.0f;
    float mHpPrevInR = 0.0f;
    float mHpPrevOutL = 0.0f;
    float mHpPrevOutR = 0.0f;
    float mLowpassAlpha = 0.1f;
    float mHighpassAlpha = 0.98f;

    float mDuckEnv = 0.0f;
    float mDuckAttackCoeff = 0.03f;
    float mDuckReleaseCoeff = 0.002f;

    double mModPhase = 0.0;
    double mModPhaseInc = 0.0;

    double mDecay = 0.5;
    double mSize = 0.5;
    double mDamping = 0.5;
    double mDiffusion = 0.6;
    double mPreDelayMs = 12.0;
    double mLowCutHz = 140.0;
    double mHighCutHz = 10000.0;
    double mTone = 0.5;
    double mWidth = 1.0;
    double mModRateHz = 0.5;
    double mModDepth = 0.2;
    double mDucking = 0.1;
    double mDrive = 0.0;
    double mShimmer = 0.0;
    double mMix = 0.24;

    float mFeedback = 0.78f;
    float mDamp = 0.3f;
    float mDiffusionGain = 0.6f;
    float mCombGain = 0.125f;
    float mShimmerAmount = 0.0f;
    float mEarlyMix = 0.3f;
  };

  inline void RegisterReverbEffect()
  {
    const std::array<ReverbEffect::Mode, 8> modes = {
        ReverbEffect::Mode::Room,
        ReverbEffect::Mode::Hall,
        ReverbEffect::Mode::Plate,
        ReverbEffect::Mode::Chamber,
        ReverbEffect::Mode::Spring,
        ReverbEffect::Mode::Shimmer,
      ReverbEffect::Mode::Ambient,
      ReverbEffect::Mode::Advanced};

    for (const auto mode : modes)
    {
      EffectTypeInfo info;
      info.type = ReverbEffect::ModeType(mode);
      info.displayName = ReverbEffect::ModeName(mode) + " Reverb";
      info.category = "reverb";
      info.description = ReverbEffect::ModeName(mode) + " algorithmic reverb";
      info.requiresResource = false;
      auto param = [](const std::string &id,
                      const std::string &name,
                      double def,
                      double min,
                      double max,
                      const std::string &unit,
                      const std::string &group = "",
                      bool advanced = false)
      {
        return ParameterDef{id, name, def, min, max, unit, group, advanced};
      };

      switch (mode)
      {
      case ReverbEffect::Mode::Room:
        info.parameters = {
            param("decay", "Decay", 0.46, 0.0, 1.0, ""),
            param("size", "Size", 0.42, 0.0, 1.0, ""),
            param("damping", "Damping", 0.56, 0.0, 1.0, ""),
            param("preDelay", "Pre Delay", 8.0, 0.0, 220.0, "ms"),
            param("mix", "Mix", 0.22, 0.0, 1.0, "")};
        break;
      case ReverbEffect::Mode::Hall:
        info.parameters = {
            param("decay", "Decay", 0.76, 0.0, 1.0, ""),
            param("size", "Size", 0.7, 0.0, 1.0, ""),
            param("preDelay", "Pre Delay", 24.0, 0.0, 220.0, "ms"),
            param("damping", "Damping", 0.46, 0.0, 1.0, ""),
            param("width", "Width", 1.05, 0.0, 1.2, ""),
            param("mix", "Mix", 0.27, 0.0, 1.0, "")};
        break;
      case ReverbEffect::Mode::Plate:
        info.parameters = {
            param("decay", "Decay", 0.58, 0.0, 1.0, ""),
            param("preDelay", "Pre Delay", 12.0, 0.0, 220.0, "ms"),
            param("tone", "Tone", 0.76, 0.0, 1.0, ""),
            param("damping", "Damping", 0.35, 0.0, 1.0, ""),
            param("mix", "Mix", 0.24, 0.0, 1.0, "")};
        break;
      case ReverbEffect::Mode::Chamber:
        info.parameters = {
            param("decay", "Decay", 0.6, 0.0, 1.0, ""),
            param("size", "Size", 0.56, 0.0, 1.0, ""),
            param("tone", "Tone", 0.52, 0.0, 1.0, ""),
            param("preDelay", "Pre Delay", 15.0, 0.0, 220.0, "ms"),
            param("mix", "Mix", 0.24, 0.0, 1.0, "")};
        break;
      case ReverbEffect::Mode::Spring:
        info.parameters = {
            param("decay", "Decay", 0.5, 0.0, 1.0, ""),
            param("tone", "Tone", 0.68, 0.0, 1.0, ""),
            param("drive", "Drive", 0.22, 0.0, 1.0, ""),
            param("mix", "Mix", 0.23, 0.0, 1.0, "")};
        break;
      case ReverbEffect::Mode::Shimmer:
        info.parameters = {
            param("decay", "Decay", 0.84, 0.0, 1.0, ""),
            param("size", "Size", 0.8, 0.0, 1.0, ""),
            param("shimmer", "Shimmer", 0.45, 0.0, 1.0, ""),
            param("preDelay", "Pre Delay", 28.0, 0.0, 220.0, "ms"),
            param("mix", "Mix", 0.30, 0.0, 1.0, "")};
        break;
      case ReverbEffect::Mode::Ambient:
        info.parameters = {
            param("decay", "Decay", 0.86, 0.0, 1.0, ""),
            param("size", "Size", 0.9, 0.0, 1.0, ""),
            param("diffusion", "Diffusion", 0.9, 0.0, 1.0, ""),
            param("width", "Width", 1.1, 0.0, 1.2, ""),
            param("mix", "Mix", 0.32, 0.0, 1.0, "")};
        break;
      case ReverbEffect::Mode::Advanced:
        info.displayName = "Advanced Reverb";
        info.type = "reverb_advanced";
        info.description = "Algorithmic reverb with full common and advanced controls";
        info.parameters = {
            param("decay", "Decay", 0.64, 0.0, 1.0, "", "Common"),
            param("size", "Size", 0.55, 0.0, 1.0, "", "Common"),
            param("mix", "Mix", 0.24, 0.0, 1.0, "", "Common"),
            param("damping", "Damping", 0.46, 0.0, 1.0, "", "Common"),
            param("preDelay", "Pre Delay", 16.0, 0.0, 220.0, "ms", "Common"),
            param("tone", "Tone", 0.62, 0.0, 1.0, "", "Common"),
            param("width", "Width", 1.0, 0.0, 1.2, "", "Common"),
            param("diffusion", "Diffusion", 0.7, 0.0, 1.0, "", "Advanced", true),
            param("lowCut", "Low Cut", 140.0, 20.0, 1200.0, "Hz", "Advanced", true),
            param("highCut", "High Cut", 12000.0, 1000.0, 20000.0, "Hz", "Advanced", true),
            param("modRate", "Mod Rate", 0.45, 0.02, 8.0, "Hz", "Advanced", true),
            param("modDepth", "Mod Depth", 0.18, 0.0, 1.0, "", "Advanced", true),
            param("ducking", "Ducking", 0.08, 0.0, 1.0, "", "Advanced", true),
            param("drive", "Drive", 0.0, 0.0, 1.0, "", "Advanced", true),
            param("shimmer", "Shimmer", 0.0, 0.0, 1.0, "", "Advanced", true)};
        break;
      }

      EffectRegistry::Instance().Register(info.type, info, [mode]()
                                          { return std::make_unique<ReverbEffect>(mode); });
    }
  }

} // namespace guitarfx
