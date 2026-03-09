#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
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
      Chamber,
      Spring,
      Advanced
    };

    explicit ReverbEffect(Mode mode)
        : mMode(mode)
    {
      ApplyModeDefaults(mMode);
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // When Advanced mode, always allocate buffers using the largest remaining topology
      // so supported characters can be switched at runtime without reallocation.
      const auto profile = (mMode == Mode::Advanced) ? GetProfile(Mode::Chamber) : GetProfile(mMode);
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

      // Smooth coefficient: ~15ms ramp to eliminate zipper noise on param changes.
      mSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.015)));
      // Size scale smoothed at ~200ms — long enough to prevent zipper on delay-length changes.
      mSizeSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.2)));
      // Snap smoothed params to targets — no ramp on initialization.
      mFeedback = mFeedbackTarget;
      mDamp = mDampTarget;
      mDiffusionGain = mDiffusionGainTarget;
      mLowpassAlpha = mLowpassAlphaTarget;
      mHighpassAlpha = mHighpassAlphaTarget;
      mSizeScaleCurrent = mSizeScaleTarget; // snap size scale so no ramp on load
      mMixSmoothed = static_cast<float>(mMix);
      mWidthSmoothed = static_cast<float>(mWidth);
      // Clear mode-specific state so no stale values from a previous Prepare.
      mSpringBpS1 = {};
      mSpringBpS2 = {};
      mSpringBp2S1 = {};
      mSpringBp2S2 = {};
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
      mSpringBpS1 = {};
      mSpringBpS2 = {};
      mSpringBp2S1 = {};
      mSpringBp2S2 = {};
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!outputs || numSamples <= 0)
        return;

      numSamples = std::min(numSamples, mMaxBlockSize);

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

      const float drive = static_cast<float>(mDrive);
      const float duckAmount = static_cast<float>(mDucking);

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs && inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs && inputs[1] ? inputs[1][i] : inL;

        // Smooth all parameters that can cause zipper noise when changed.
        mFeedback += (mFeedbackTarget - mFeedback) * mSmoothCoeff;
        mDamp += (mDampTarget - mDamp) * mSmoothCoeff;
        mDiffusionGain += (mDiffusionGainTarget - mDiffusionGain) * mSmoothCoeff;
        mLowpassAlpha += (mLowpassAlphaTarget - mLowpassAlpha) * mSmoothCoeff;
        mHighpassAlpha += (mHighpassAlphaTarget - mHighpassAlpha) * mSmoothCoeff;
        // Size scale smoothed at ~200ms to eliminate zipper noise from delay-length changes.
        mSizeScaleCurrent += (mSizeScaleTarget - mSizeScaleCurrent) * mSizeSmoothCoeff;
        // Mix and width smoothed to prevent clicks on parameter changes.
        mMixSmoothed += (static_cast<float>(mMix) - mMixSmoothed) * mSmoothCoeff;
        mWidthSmoothed += (static_cast<float>(mWidth) - mWidthSmoothed) * mSmoothCoeff;
        const float wet = mMixSmoothed;
        const float dry = 1.0f - wet;
        const float width = mWidthSmoothed;

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

        float earlyL = preL * mEarlyBaseGain;
        float earlyR = preR * mEarlyBaseGain;
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

        const float combModDepth = mModDepthInternal;
        const float allpassModDepth = mModDepthInternal * 0.6f;

        float tankInL = ApplyDrive(preL * (1.0f - mEarlyMix) + earlyL * mEarlyMix, drive);
        float tankInR = ApplyDrive(preR * (1.0f - mEarlyMix) + earlyR * mEarlyMix, drive);

        float combOutL = 0.0f;
        float combOutR = 0.0f;
        std::array<float, kCombCount> combFeedbackL = {};
        std::array<float, kCombCount> combFeedbackR = {};
        float combFeedbackSumL = 0.0f;
        float combFeedbackSumR = 0.0f;

        for (size_t c = 0; c < kCombCount; ++c)
        {
          const float combPhase = mod + static_cast<float>(std::sin(mModPhase + kCombModPhaseOffsets[c]));
          const float combScaleL = std::max(0.85f, 1.0f + combPhase * combModDepth);
          const float combScaleR = std::max(0.85f, 1.0f - combPhase * combModDepth);
          const float dL = std::clamp(mBaseCombSamplesL[c] * mSizeScaleCurrent * combScaleL,
                                      1.0f, static_cast<float>(mCombBufferL[c].size() - 2));
          const float dR = std::clamp(mBaseCombSamplesR[c] * mSizeScaleCurrent * combScaleR,
                                      1.0f, static_cast<float>(mCombBufferR[c].size() - 2));
          const float delayedL = ReadFromDelayFractional(mCombBufferL[c], mCombWriteL[c], dL);
          const float delayedR = ReadFromDelayFractional(mCombBufferR[c], mCombWriteR[c], dR);

          mCombFilterStoreL[c] = delayedL * (1.0f - mDamp) + mCombFilterStoreL[c] * mDamp;
          mCombFilterStoreR[c] = delayedR * (1.0f - mDamp) + mCombFilterStoreR[c] * mDamp;

          combFeedbackL[c] = mCombFilterStoreL[c];
          combFeedbackR[c] = mCombFilterStoreR[c];
          combFeedbackSumL += combFeedbackL[c];
          combFeedbackSumR += combFeedbackR[c];

          combOutL += delayedL;
          combOutR += delayedR;
        }

        constexpr float kHouseholderScale = 2.0f / static_cast<float>(kCombCount);
        constexpr float kStereoFeedbackBlend = 0.12f;
        for (size_t c = 0; c < kCombCount; ++c)
        {
          const float mixedL = combFeedbackL[c] - combFeedbackSumL * kHouseholderScale;
          const float mixedR = combFeedbackR[c] - combFeedbackSumR * kHouseholderScale;
          const float feedbackL = mixedL + (mixedR - mixedL) * kStereoFeedbackBlend;
          const float feedbackR = mixedR + (mixedL - mixedR) * kStereoFeedbackBlend;

          mCombBufferL[c][mCombWriteL[c]] = tankInL + feedbackL * mFeedback;
          mCombBufferR[c][mCombWriteR[c]] = tankInR + feedbackR * mFeedback;

          if (++mCombWriteL[c] >= mCombBufferL[c].size())
            mCombWriteL[c] = 0;
          if (++mCombWriteR[c] >= mCombBufferR[c].size())
            mCombWriteR[c] = 0;
        }

        float wetL = combOutL * mCombGain;
        float wetR = combOutR * mCombGain;

        for (size_t a = 0; a < kAllpassCount; ++a)
        {
          const float allpassPhase = mod + static_cast<float>(std::sin(mModPhase + kAllpassModPhaseOffsets[a]));
          const float allpassScaleL = std::max(0.9f, 1.0f + allpassPhase * allpassModDepth);
          const float allpassScaleR = std::max(0.9f, 1.0f - allpassPhase * allpassModDepth);
          const float dL = std::clamp(mBaseAllpassSamplesL[a] * mSizeScaleCurrent * allpassScaleL,
                                      1.0f, static_cast<float>(mAllpassBufferL[a].size() - 2));
          const float dR = std::clamp(mBaseAllpassSamplesR[a] * mSizeScaleCurrent * allpassScaleR,
                                      1.0f, static_cast<float>(mAllpassBufferR[a].size() - 2));
          const float delayedL = ReadFromDelayFractional(mAllpassBufferL[a], mAllpassWriteL[a], dL);
          const float delayedR = ReadFromDelayFractional(mAllpassBufferR[a], mAllpassWriteR[a], dR);

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

        wetL = ProcessFiltersL(wetL);
        wetR = ProcessFiltersR(wetR);

        // Spring mode: blend two bandpass-filtered signals for dispersive "boing" character.
        // Primary peak (fundamental) + harmonic peak at ~1.75× for authentic spring texture.
        if (mMode == Mode::Spring || (mMode == Mode::Advanced && mCharacter == Mode::Spring))
        {
          const float bpL  = static_cast<float>(ProcessSpringBp (static_cast<double>(wetL), 0));
          const float bpR  = static_cast<float>(ProcessSpringBp (static_cast<double>(wetR), 1));
          const float bp2L = static_cast<float>(ProcessSpringBp2(static_cast<double>(wetL), 0));
          const float bp2R = static_cast<float>(ProcessSpringBp2(static_cast<double>(wetR), 1));
          wetL = wetL * 0.45f + bpL * 0.40f + bp2L * 0.15f;
          wetR = wetR * 0.45f + bpR * 0.40f + bp2R * 0.15f;
        }

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
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
      else if (key == "character" && mMode == Mode::Advanced)
      {
        const int idx = static_cast<int>(std::round(std::clamp(value, 0.0, 2.0)));
        mCharacter = CharacterFromIndex(idx);
        // Only swap the delay topology and clear buffers — all other parameters are
        // intentionally preserved so Advanced mode users retain their manual settings.
        Reset();
        UpdateParameters();
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
      if (key == "mix")
        return mMix;
      if (key == "character")
        return static_cast<double>(CharacterToIndex(mCharacter));
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
      switch (mMode)
      {
      case Mode::Chamber:
        return "reverb_chamber";
      case Mode::Spring:
        return "reverb_spring";
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
      case Mode::Chamber:
        return "Chamber";
      case Mode::Spring:
        return "Spring";
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
      case Mode::Chamber:
        return "reverb_chamber";
      case Mode::Spring:
        return "reverb_spring";
      case Mode::Advanced:
        return "reverb_advanced";
      case Mode::Room:
      default:
        return "reverb_room";
      }
    }

    static const char* ModeGuid(Mode mode)
    {
      switch (mode)
      {
      case Mode::Chamber:
        return EffectGuids::kReverbChamber;
      case Mode::Spring:
        return EffectGuids::kReverbSpring;
      case Mode::Advanced:
        return EffectGuids::kReverbAdvanced;
      case Mode::Room:
      default:
        return EffectGuids::kReverbRoom;
      }
    }

  private:
    static constexpr size_t kCombCount = 8;
    static constexpr size_t kAllpassCount = 4;
    static constexpr size_t kEarlyTapCount = 4;
    static constexpr double kMaxPreDelayMs = 220.0;
    static constexpr double kMaxTapMs = 46.0;
    static constexpr double kTwoPi = 6.2831853071795864769;
    static constexpr std::array<double, kCombCount> kCombModPhaseOffsets = {
      0.0, 0.73, 1.41, 2.19, 2.94, 3.67, 4.28, 5.11};
    static constexpr std::array<double, kAllpassCount> kAllpassModPhaseOffsets = {
      0.37, 1.83, 3.12, 4.71};

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
      double driveBias = 0.0;
      double earlyMixBias = 0.35;
      double sizeBase = 0.58;
      double sizeRange = 1.0;
      double feedbackBase = 0.48;
      double feedbackRange = 0.28;
      double earlyBaseGain = 0.4;
      double earlyTapScale = 1.0;
      double earlyMixMin = 0.06;
      double earlyMixMax = 0.46;
    };

    static ModeProfile GetProfile(Mode mode)
    {
      switch (mode)
      {
      case Mode::Chamber:
        return {
            {26.7, 29.8, 33.4, 36.9, 39.8, 43.6, 47.5, 50.9},
            {27.4, 30.6, 34.1, 37.6, 40.7, 44.4, 48.3, 51.8},
            {6.1, 9.1, 12.8, 17.2},
            {6.8, 9.8, 13.5, 17.9},
            {8.0, 13.0, 21.0, 31.0},
            {0.38f, 0.24f, 0.17f, 0.12f},
            1.1, 0.52, 0.68, 0.42, 0.05, 0.26, 0.50, 0.86, 0.46, 0.24, 0.26, 0.74, 0.08, 0.34};
      case Mode::Spring:
        return {
            {18.2, 20.7, 24.3, 27.0, 30.1, 32.8, 35.5, 38.4},
            {18.7, 21.4, 24.8, 27.7, 30.7, 33.2, 36.1, 39.1},
            {3.1, 5.2, 7.3, 9.7},
            {3.6, 5.8, 7.9, 10.1},
            {4.0, 7.0, 11.0, 16.0},
            {0.45f, 0.27f, 0.17f, 0.11f},
            0.9, 0.28, 0.52, 0.32, 0.35, 0.18, 0.46, 0.70, 0.44, 0.20, 0.38, 0.90, 0.08, 0.32};
      case Mode::Room:
      default:
        return {
            {19.7, 22.1, 24.9, 27.8, 30.5, 33.2, 35.7, 38.1},
            {20.3, 22.8, 25.4, 28.5, 31.3, 33.9, 36.4, 39.2},
            {4.8, 6.8, 9.1, 12.2},
            {5.2, 7.3, 9.8, 12.9},
            {5.0, 9.0, 14.0, 20.0},
            {0.40f, 0.25f, 0.19f, 0.13f},
            0.95, 0.52, 0.56, 0.3, 0.0, 0.40, 0.58, 0.96, 0.50, 0.28, 0.42, 1.00, 0.12, 0.44};
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

    // Linear-interpolated delay read — eliminates integer-step zipper noise when delay
    // length changes smoothly. Caller must ensure delaySamples <= buffer.size() - 2.
    float ReadFromDelayFractional(const std::vector<float>& buffer, size_t writePos, float delaySamples) const
    {
      if (buffer.empty())
        return 0.0f;
      const size_t d0 = static_cast<size_t>(delaySamples);
      const float frac = delaySamples - static_cast<float>(d0);
      const float s0 = ReadFromDelay(buffer, writePos, d0);
      const float s1 = ReadFromDelay(buffer, writePos, d0 + 1);
      return s0 + frac * (s1 - s0);
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

    // Biquad bandpass used for Spring mode tonal coloring.
    void ComputeBandpass(double freq, double q,
                         double &b0, double &b1, double &b2,
                         double &a1, double &a2) const
    {
      if (mSampleRate <= 0.0)
      {
        b0 = 1.0; b1 = 0.0; b2 = 0.0; a1 = 0.0; a2 = 0.0;
        return;
      }
      const double w0 = kTwoPi * freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * q);
      const double a0inv = 1.0 / (1.0 + alpha);
      b0 = alpha * a0inv;
      b1 = 0.0;
      b2 = -alpha * a0inv;
      a1 = -2.0 * cosw0 * a0inv;
      a2 = (1.0 - alpha) * a0inv;
    }

    double ProcessSpringBp(double input, int ch)
    {
      const double out = mSpringBpB0 * input + mSpringBpS1[ch];
      mSpringBpS1[ch] = mSpringBpB1 * input - mSpringBpA1 * out + mSpringBpS2[ch];
      mSpringBpS2[ch] = mSpringBpB2 * input - mSpringBpA2 * out;
      return out;
    }

    double ProcessSpringBp2(double input, int ch)
    {
      const double out = mSpringBp2B0 * input + mSpringBp2S1[ch];
      mSpringBp2S1[ch] = mSpringBp2B1 * input - mSpringBp2A1 * out + mSpringBp2S2[ch];
      mSpringBp2S2[ch] = mSpringBp2B2 * input - mSpringBp2A2 * out;
      return out;
    }

    void UpdateParameters()
    {
      const auto profile = (mMode == Mode::Advanced) ? GetProfile(mCharacter) : GetProfile(mMode);

      // Store base delay samples (unscaled, sizeScale=1) for per-sample fractional interpolation.
      // The current size scale is smoothed in Process() to eliminate integer-step zipper noise.
      for (size_t i = 0; i < kCombCount; ++i)
      {
        mBaseCombSamplesL[i] = static_cast<float>(DelayMsToSamples(profile.combMsL[i]));
        mBaseCombSamplesR[i] = static_cast<float>(DelayMsToSamples(profile.combMsR[i]));
      }

      for (size_t i = 0; i < kAllpassCount; ++i)
      {
        mBaseAllpassSamplesL[i] = static_cast<float>(DelayMsToSamples(profile.allpassMsL[i]));
        mBaseAllpassSamplesR[i] = static_cast<float>(DelayMsToSamples(profile.allpassMsR[i]));
      }

      mSizeScaleTarget = static_cast<float>(profile.sizeBase + mSize * profile.sizeRange);

      for (size_t t = 0; t < kEarlyTapCount; ++t)
      {
        mEarlyTapSamples[t] = DelayMsToSamples(profile.earlyTapMs[t] * (0.9 + 0.4 * mSize));
        mEarlyTapSamplesMirror[t] = DelayMsToSamples((profile.earlyTapMs[t] + 0.9 + static_cast<double>(t) * 0.7) * (0.9 + 0.4 * mSize));
        mEarlyTapSamples[t] = std::min(mEarlyTapSamples[t], mPreDelayL.empty() ? size_t(1) : mPreDelayL.size() - 1);
        mEarlyTapSamplesMirror[t] = std::min(mEarlyTapSamplesMirror[t], mPreDelayL.empty() ? size_t(1) : mPreDelayL.size() - 1);
        mEarlyTapGains[t] = profile.earlyTapGains[t] * static_cast<float>(profile.earlyTapScale);
      }

      mPreDelaySamples = DelayMsToSamples(mPreDelayMs);
      mPreDelaySamples = std::min(mPreDelaySamples, mPreDelayL.empty() ? size_t(1) : mPreDelayL.size() - 1);

      mFeedbackTarget = static_cast<float>(std::clamp(profile.feedbackBase + (mDecay * profile.decayBias) * profile.feedbackRange, 0.3, 0.985));

      const double dampBase = std::clamp(0.02 + (mDamping * 0.62) + (1.0 - mTone) * 0.26 + profile.dampBias * 0.08, 0.02, 0.96);
      mDampTarget = static_cast<float>(dampBase);

      mDiffusionGainTarget = static_cast<float>(std::clamp(0.28 + mDiffusion * 0.58 + profile.diffusionBias * 0.08, 0.2, 0.92));
      mCombGain = 1.0f / static_cast<float>(kCombCount);

      const double modBias = std::clamp(profile.modulationBias + mModDepth * 0.8, 0.0, 1.5);
      mModPhaseInc = kTwoPi * std::clamp(mModRateHz, 0.02, 8.0) / std::max(1.0, mSampleRate);
      // mModDepth keeps the user-facing 0–1 value; mModDepthInternal holds the audio-rate scale.
      mModDepthInternal = static_cast<float>(std::clamp(0.0002 + mModDepth * 0.004 * modBias, 0.0, 0.02));

      const double lowCut = std::clamp(mLowCutHz, 20.0, 1200.0);
      const double highCut = std::clamp(mHighCutHz * (0.65 + mTone * 0.7), 1000.0, 20000.0);
      const double dt = 1.0 / std::max(1.0, mSampleRate);
      const double rcLow = 1.0 / (2.0 * 3.14159265358979323846 * lowCut);
      const double rcHigh = 1.0 / (2.0 * 3.14159265358979323846 * highCut);
      mHighpassAlphaTarget = static_cast<float>(rcLow / (rcLow + dt));
      mLowpassAlphaTarget = static_cast<float>(dt / (rcHigh + dt));

      mEarlyBaseGain = static_cast<float>(profile.earlyBaseGain);
      mEarlyMix = static_cast<float>(std::clamp(profile.earlyMixBias + mDiffusion * 0.12, profile.earlyMixMin, profile.earlyMixMax));

      const double duckAttack = 0.006;
      const double duckRelease = 0.11;
      mDuckAttackCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * duckAttack)));
      mDuckReleaseCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * duckRelease)));

      // Spring mode: two biquad bandpass filters for dispersive "boing" character.
      // Real spring tanks have a fundamental resonance and a harmonic above it.
      // Both centre frequencies track Tone: darker → lower, brighter → higher.
      if (mMode == Mode::Spring || (mMode == Mode::Advanced && mCharacter == Mode::Spring))
      {
        const double centerHz = 700.0 + mTone * 1800.0;     // primary:  700–2500 Hz
        const double q = 0.75 + mTone * 0.60;
        ComputeBandpass(centerHz, q,
                        mSpringBpB0, mSpringBpB1, mSpringBpB2,
                        mSpringBpA1, mSpringBpA2);
        // Second harmonic peak at ~1.75× primary — adds the characteristic metallic sheen
        // of dispersive spring propagation without over-brightening the tail.
        const double centerHz2 = std::clamp(centerHz * 1.75, 1200.0, 6500.0);
        const double q2 = 0.50 + mTone * 0.30;             // softer resonance than primary
        ComputeBandpass(centerHz2, q2,
                        mSpringBp2B0, mSpringBp2B1, mSpringBp2B2,
                        mSpringBp2A1, mSpringBp2A2);
      }

    }

    static Mode CharacterFromIndex(int index)
    {
      switch (index)
      {
      case 1: return Mode::Chamber;
      case 2: return Mode::Spring;
      default: return Mode::Room;
      }
    }

    static int CharacterToIndex(Mode mode)
    {
      switch (mode)
      {
      case Mode::Chamber: return 1;
      case Mode::Spring:  return 2;
      default:            return 0;
      }
    }

    // ApplyModeDefaults() sets DSP parameter defaults for the given mode.
    // Called in the constructor (with mMode) and on character changes (with mCharacter).
    void ApplyModeDefaults(Mode mode)
    {
      switch (mode)
      {
      case Mode::Chamber:
        mDecay = 0.42;
        mSize = 0.38;
        mDamping = 0.66;
        mDiffusion = 0.7;
        mPreDelayMs = 12.0;
        mLowCutHz = 140.0;
        mHighCutHz = 7600.0;
        mTone = 0.42;
        mWidth = 0.84;
        mModRateHz = 0.26;
        mModDepth = 0.18;
        mDucking = 0.1;
        mDrive = 0.0;
        mMix = 0.16;
        break;
      case Mode::Spring:
        mDecay = 0.34;
        mSize = 0.28;
        mDamping = 0.34;
        mDiffusion = 0.48;
        mPreDelayMs = 3.0;
        mLowCutHz = 180.0;
        mHighCutHz = 6400.0;
        mTone = 0.52;
        mWidth = 0.68;
        mModRateHz = 0.9;
        mModDepth = 0.08;
        mDucking = 0.06;
        mDrive = 0.1;
        mMix = 0.14;
        break;
      case Mode::Advanced:
        mDecay = 0.4;
        mSize = 0.4;
        mDamping = 0.66;
        mDiffusion = 0.74;
        mPreDelayMs = 12.0;
        mLowCutHz = 140.0;
        mHighCutHz = 7600.0;
        mTone = 0.42;
        mWidth = 0.88;
        mModRateHz = 0.28;
        mModDepth = 0.26;
        mDucking = 0.08;
        mDrive = 0.0;
        mMix = 0.16;
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
    // Active character topology when mMode == Advanced; ignored for all other modes.
    Mode mCharacter = Mode::Room;

    std::vector<float> mPreDelayL;
    std::vector<float> mPreDelayR;
    size_t mPreDelayWrite = 0;

    std::array<std::vector<float>, kCombCount> mCombBufferL;
    std::array<std::vector<float>, kCombCount> mCombBufferR;
    std::array<size_t, kCombCount> mCombWriteL = {};
    std::array<size_t, kCombCount> mCombWriteR = {};
    // Base (unscaled) delay samples for each comb — multiplied by mSizeScaleCurrent per sample.
    std::array<float, kCombCount> mBaseCombSamplesL = {};
    std::array<float, kCombCount> mBaseCombSamplesR = {};
    std::array<float, kCombCount> mCombFilterStoreL = {};
    std::array<float, kCombCount> mCombFilterStoreR = {};

    std::array<std::vector<float>, kAllpassCount> mAllpassBufferL;
    std::array<std::vector<float>, kAllpassCount> mAllpassBufferR;
    std::array<size_t, kAllpassCount> mAllpassWriteL = {};
    std::array<size_t, kAllpassCount> mAllpassWriteR = {};
    // Base (unscaled) delay samples for each allpass — multiplied by mSizeScaleCurrent per sample.
    std::array<float, kAllpassCount> mBaseAllpassSamplesL = {};
    std::array<float, kAllpassCount> mBaseAllpassSamplesR = {};

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
    double mMix = 0.24;

    float mFeedback = 0.78f;
    float mDamp = 0.3f;
    float mDiffusionGain = 0.6f;
    float mCombGain = 0.125f;
    float mEarlyBaseGain = 0.42f;
    float mEarlyMix = 0.3f;

    // Smoothed-parameter targets — set in UpdateParameters(), lerped per-sample in Process().
    float mFeedbackTarget = 0.78f;
    float mDampTarget = 0.3f;
    float mDiffusionGainTarget = 0.6f;
    float mLowpassAlphaTarget = 0.1f;
    float mHighpassAlphaTarget = 0.98f;
    float mSmoothCoeff = 0.002f;

    // Size scale smoothing — eliminates zipper noise when the size parameter is adjusted.
    // Lerped at ~200ms so integer delay-length steps are inaudible.
    float mSizeScaleTarget = 1.0f;
    float mSizeScaleCurrent = 1.0f;
    float mSizeSmoothCoeff = 0.0001f;

    // Mix and width smoothed values — lerped per-sample to prevent click on parameter changes.
    float mMixSmoothed = 0.24f;
    float mWidthSmoothed = 1.0f;

    // Audio-rate modulation depth (scaled from mModDepth user value in UpdateParameters).
    // Kept separate so GetParam("modDepth") always returns the user-facing 0–1 value.
    float mModDepthInternal = 0.0008f;

    // Spring mode: primary biquad bandpass state + coefficients for fundamental resonance.
    std::array<double, 2> mSpringBpS1 = {};
    std::array<double, 2> mSpringBpS2 = {};
    double mSpringBpB0 = 1.0, mSpringBpB1 = 0.0, mSpringBpB2 = -1.0;
    double mSpringBpA1 = 0.0, mSpringBpA2 = 0.0;

    // Spring mode: second biquad bandpass at ~1.75× primary — harmonic dispersive peak.
    std::array<double, 2> mSpringBp2S1 = {};
    std::array<double, 2> mSpringBp2S2 = {};
    double mSpringBp2B0 = 1.0, mSpringBp2B1 = 0.0, mSpringBp2B2 = -1.0;
    double mSpringBp2A1 = 0.0, mSpringBp2A2 = 0.0;

  };

  inline void RegisterReverbEffect()
  {
    const std::array<ReverbEffect::Mode, 3> modes = {
        ReverbEffect::Mode::Room,
        ReverbEffect::Mode::Chamber,
        ReverbEffect::Mode::Advanced};

    for (const auto mode : modes)
    {
      EffectTypeInfo info;
      info.type = ReverbEffect::ModeGuid(mode);
      info.aliases = {ReverbEffect::ModeType(mode)};
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
            param("decay", "Decay", 0.46, 0.0, 1.0, "amount"),
            param("size", "Size", 0.42, 0.0, 1.0, "amount"),
            param("damping", "Damping", 0.56, 0.0, 1.0, "amount"),
            param("preDelay", "Pre Delay", 8.0, 0.0, 220.0, "ms"),
            param("mix", "Mix", 0.22, 0.0, 1.0, "amount")};
        break;
      case ReverbEffect::Mode::Chamber:
        info.parameters = {
            param("decay", "Decay", 0.6, 0.0, 1.0, "amount"),
            param("size", "Size", 0.56, 0.0, 1.0, "amount"),
            param("tone", "Tone", 0.52, 0.0, 1.0, "amount"),
            param("preDelay", "Pre Delay", 15.0, 0.0, 220.0, "ms"),
            param("mix", "Mix", 0.24, 0.0, 1.0, "amount")};
        break;
      case ReverbEffect::Mode::Advanced:
        info.displayName = "Advanced Reverb";
        info.description = "Algorithmic reverb with full common and advanced controls";
        info.parameters = {
            ParameterDef{"character", "Character", 0.0, 0.0, 2.0, "enum", "Common", false, 1.0,
                         {"Room", "Chamber", "Spring"}},
            param("decay", "Decay", 0.64, 0.0, 1.0, "amount", "Common"),
            param("size", "Size", 0.55, 0.0, 1.0, "amount", "Common"),
            param("mix", "Mix", 0.24, 0.0, 1.0, "amount", "Common"),
            param("damping", "Damping", 0.46, 0.0, 1.0, "amount", "Common"),
            param("preDelay", "Pre Delay", 16.0, 0.0, 220.0, "ms", "Common"),
            param("tone", "Tone", 0.62, 0.0, 1.0, "amount", "Common"),
            param("width", "Width", 1.0, 0.0, 1.2, "amount", "Common"),
            param("diffusion", "Diffusion", 0.7, 0.0, 1.0, "amount", "Advanced", true),
            param("lowCut", "Low Cut", 140.0, 20.0, 1200.0, "Hz", "Advanced", true),
            param("highCut", "High Cut", 12000.0, 1000.0, 20000.0, "Hz", "Advanced", true),
            param("modRate", "Mod Rate", 0.45, 0.02, 8.0, "Hz", "Advanced", true),
            param("modDepth", "Mod Depth", 0.18, 0.0, 1.0, "amount", "Advanced", true),
            param("ducking", "Ducking", 0.08, 0.0, 1.0, "amount", "Advanced", true),
            param("drive", "Drive", 0.0, 0.0, 1.0, "amount", "Advanced", true)};
        break;
      }

      EffectRegistry::Instance().Register(info.type, info, [mode]()
                                          { return std::make_unique<ReverbEffect>(mode); });
    }
  }

} // namespace guitarfx
