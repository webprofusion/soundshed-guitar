#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace guitarfx
{
  class SpringReverbEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;

      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      const size_t maxInputSamples = DelayMsToSamples(kMaxInputDelayMs + 6.0);
      mInputDelayL.assign(maxInputSamples, 0.0f);
      mInputDelayR.assign(maxInputSamples, 0.0f);
      mInputDelayWrite = 0;

      constexpr double extraTailMs = 12.0;
      for (size_t index = 0; index < kTankCount; ++index)
      {
        const size_t lenL = DelayMsToSamples(kTankDelayMsL[index] * kMaxTensionScale + extraTailMs);
        const size_t lenR = DelayMsToSamples(kTankDelayMsR[index] * kMaxTensionScale + extraTailMs);
        mTankDelayL[index].assign(lenL, 0.0f);
        mTankDelayR[index].assign(lenR, 0.0f);
        mTankWriteL[index] = 0;
        mTankWriteR[index] = 0;
        mTankLowpassStateL[index] = 0.0f;
        mTankLowpassStateR[index] = 0.0f;
      }

      constexpr double extraDispersionMs = 4.0;
      for (size_t index = 0; index < kDispersionCount; ++index)
      {
        const size_t lenL = DelayMsToSamples(kDispersionDelayMsL[index] * kMaxTensionScale + extraDispersionMs);
        const size_t lenR = DelayMsToSamples(kDispersionDelayMsR[index] * kMaxTensionScale + extraDispersionMs);
        mDispersionDelayL[index].assign(lenL, 0.0f);
        mDispersionDelayR[index].assign(lenR, 0.0f);
        mDispersionWriteL[index] = 0;
        mDispersionWriteR[index] = 0;
      }

      mSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.015)));
      mTensionSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.12)));

      UpdateParameters();
      mFeedback = mFeedbackTarget;
      mDamp = mDampTarget;
      mBrightness = mBrightnessTarget;
      mTensionScale = mTensionScaleTarget;
      mMixSmoothed = static_cast<float>(mMix);
      mDriveSmoothed = static_cast<float>(mDrive);

      Reset();
    }

    void Reset() override
    {
      std::fill(mInputDelayL.begin(), mInputDelayL.end(), 0.0f);
      std::fill(mInputDelayR.begin(), mInputDelayR.end(), 0.0f);
      mInputDelayWrite = 0;

      for (size_t index = 0; index < kTankCount; ++index)
      {
        std::fill(mTankDelayL[index].begin(), mTankDelayL[index].end(), 0.0f);
        std::fill(mTankDelayR[index].begin(), mTankDelayR[index].end(), 0.0f);
        mTankWriteL[index] = 0;
        mTankWriteR[index] = 0;
        mTankLowpassStateL[index] = 0.0f;
        mTankLowpassStateR[index] = 0.0f;
      }

      for (size_t index = 0; index < kDispersionCount; ++index)
      {
        std::fill(mDispersionDelayL[index].begin(), mDispersionDelayL[index].end(), 0.0f);
        std::fill(mDispersionDelayR[index].begin(), mDispersionDelayR[index].end(), 0.0f);
        mDispersionWriteL[index] = 0;
        mDispersionWriteR[index] = 0;
      }

      mWetToneStateL = 0.0f;
      mWetToneStateR = 0.0f;
      mDripEnv = 0.0f;
      mInputHpPrevL = 0.0f;
      mInputHpPrevR = 0.0f;
      mInputHpStateL = 0.0f;
      mInputHpStateR = 0.0f;
      mBandpassState1L = {};
      mBandpassState1R = {};
      mBandpassState2L = {};
      mBandpassState2R = {};
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!outputs || numSamples <= 0)
        return;

      numSamples = std::min(numSamples, mMaxBlockSize);

      if (mInputDelayL.empty() || mTankDelayL[0].empty())
      {
        CopyInputToOutput(inputs, outputs, numSamples);
        return;
      }

      if (!mEnabled)
      {
        CopyInputToOutput(inputs, outputs, numSamples);
        return;
      }

      for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
      {
        const float inL = inputs && inputs[0] ? inputs[0][sampleIndex] : 0.0f;
        const float inR = inputs && inputs[1] ? inputs[1][sampleIndex] : inL;

        mFeedback += (mFeedbackTarget - mFeedback) * mSmoothCoeff;
        mDamp += (mDampTarget - mDamp) * mSmoothCoeff;
        mBrightness += (mBrightnessTarget - mBrightness) * mSmoothCoeff;
        mTensionScale += (mTensionScaleTarget - mTensionScale) * mTensionSmoothCoeff;
        mMixSmoothed += (static_cast<float>(mMix) - mMixSmoothed) * mSmoothCoeff;
        mDriveSmoothed += (static_cast<float>(mDrive) - mDriveSmoothed) * mSmoothCoeff;

        const float dryMix = 1.0f - mMixSmoothed;
        const float driveAmount = mDriveSmoothed;

        const float hpInL = ProcessInputHighpass(inL, mInputHpPrevL, mInputHpStateL);
        const float hpInR = ProcessInputHighpass(inR, mInputHpPrevR, mInputHpStateR);

        const float mono = 0.5f * (hpInL + hpInR);
        const float side = 0.5f * (hpInL - hpInR);
        const float attackLevel = std::max(std::fabs(hpInL), std::fabs(hpInR));
        mDripEnv += (attackLevel - mDripEnv) * (attackLevel > mDripEnv ? mDripAttackCoeff : mDripReleaseCoeff);

        const float drivenL = ApplyDrive(mono + side * 0.35f, driveAmount);
        const float drivenR = ApplyDrive(mono - side * 0.35f, driveAmount);

        mInputDelayL[mInputDelayWrite] = drivenL;
        mInputDelayR[mInputDelayWrite] = drivenR;
        const float delayedInL = ReadFromDelay(mInputDelayL, mInputDelayWrite, mInputDelaySamples);
        const float delayedInR = ReadFromDelay(mInputDelayR, mInputDelayWrite, mInputDelaySamples);
        if (++mInputDelayWrite >= mInputDelayL.size())
          mInputDelayWrite = 0;

        float exciteL = delayedInL;
        float exciteR = delayedInR;
        for (size_t index = 0; index < kDispersionCount; ++index)
        {
          const float delayL = std::clamp(DelayMsToSamplesFloat(kDispersionDelayMsL[index] * mTensionScale),
                                          1.0f,
                                          static_cast<float>(mDispersionDelayL[index].size() - 2));
          const float delayR = std::clamp(DelayMsToSamplesFloat(kDispersionDelayMsR[index] * mTensionScale),
                                          1.0f,
                                          static_cast<float>(mDispersionDelayR[index].size() - 2));
          exciteL = ProcessAllpass(mDispersionDelayL[index], mDispersionWriteL[index], delayL, exciteL, 0.58f);
          exciteR = ProcessAllpass(mDispersionDelayR[index], mDispersionWriteR[index], delayR, exciteR, 0.58f);
        }

        float tankOutL = 0.0f;
        float tankOutR = 0.0f;
        for (size_t tankIndex = 0; tankIndex < kTankCount; ++tankIndex)
        {
          const float delayL = std::clamp(DelayMsToSamplesFloat(kTankDelayMsL[tankIndex] * mTensionScale),
                                          1.0f,
                                          static_cast<float>(mTankDelayL[tankIndex].size() - 2));
          const float delayR = std::clamp(DelayMsToSamplesFloat(kTankDelayMsR[tankIndex] * mTensionScale),
                                          1.0f,
                                          static_cast<float>(mTankDelayR[tankIndex].size() - 2));

          const float delayedL = ReadFromDelayFractional(mTankDelayL[tankIndex], mTankWriteL[tankIndex], delayL);
          const float delayedR = ReadFromDelayFractional(mTankDelayR[tankIndex], mTankWriteR[tankIndex], delayR);

          mTankLowpassStateL[tankIndex] = FlushNearZero(mTankLowpassStateL[tankIndex] + (delayedL - mTankLowpassStateL[tankIndex]) * mDamp);
          mTankLowpassStateR[tankIndex] = FlushNearZero(mTankLowpassStateR[tankIndex] + (delayedR - mTankLowpassStateR[tankIndex]) * mDamp);

          const float filteredL = mTankLowpassStateL[tankIndex];
          const float filteredR = mTankLowpassStateR[tankIndex];

          const float feedbackL = filteredL * (mFeedback * (0.54f - 0.04f * static_cast<float>(tankIndex)))
                                + filteredR * (0.030f + 0.008f * static_cast<float>(tankIndex));
          const float feedbackR = filteredR * (mFeedback * (0.54f - 0.04f * static_cast<float>(tankIndex)))
                                + filteredL * (0.030f + 0.008f * static_cast<float>(tankIndex));

          const float injectL = exciteL * (0.48f - 0.08f * static_cast<float>(tankIndex));
          const float injectR = exciteR * (0.48f - 0.08f * static_cast<float>(tankIndex));

          const float tankInputL = injectL + feedbackL;
          const float tankInputR = injectR + feedbackR;
          mTankDelayL[tankIndex][mTankWriteL[tankIndex]] = FlushNearZero(ApplyDrive(tankInputL, driveAmount * 0.28f));
          mTankDelayR[tankIndex][mTankWriteR[tankIndex]] = FlushNearZero(ApplyDrive(tankInputR, driveAmount * 0.28f));

          if (++mTankWriteL[tankIndex] >= mTankDelayL[tankIndex].size())
            mTankWriteL[tankIndex] = 0;
          if (++mTankWriteR[tankIndex] >= mTankDelayR[tankIndex].size())
            mTankWriteR[tankIndex] = 0;

          tankOutL += filteredL;
          tankOutR += filteredR;
        }

        tankOutL *= (1.0f / static_cast<float>(kTankCount));
        tankOutR *= (1.0f / static_cast<float>(kTankCount));

        const float drip1L = ProcessBandpass(tankOutL, mBandpassState1L, mDripBand1B0, mDripBand1B2, mDripBand1A1, mDripBand1A2);
        const float drip1R = ProcessBandpass(tankOutR, mBandpassState1R, mDripBand1B0, mDripBand1B2, mDripBand1A1, mDripBand1A2);
        const float drip2L = ProcessBandpass(tankOutL, mBandpassState2L, mDripBand2B0, mDripBand2B2, mDripBand2A1, mDripBand2A2);
        const float drip2R = ProcessBandpass(tankOutR, mBandpassState2R, mDripBand2B0, mDripBand2B2, mDripBand2A1, mDripBand2A2);

        float wetL = tankOutL * (0.68f + mBrightness * 0.14f) + (drip1L * 0.42f + drip2L * 0.20f) * (0.22f + driveAmount * 0.38f) * mDripEnv;
        float wetR = tankOutR * (0.68f + mBrightness * 0.14f) + (drip1R * 0.42f + drip2R * 0.20f) * (0.22f + driveAmount * 0.38f) * mDripEnv;

        mWetToneStateL = FlushNearZero(mWetToneStateL + (wetL - mWetToneStateL) * mBrightness);
        mWetToneStateR = FlushNearZero(mWetToneStateR + (wetR - mWetToneStateR) * mBrightness);
        wetL = mWetToneStateL;
        wetR = mWetToneStateR;

        const float wetMid = 0.5f * (wetL + wetR);
        const float wetSide = 0.5f * (wetL - wetR);
        wetL = wetMid + wetSide * 0.74f;
        wetR = wetMid - wetSide * 0.74f;

        if (outputs[0])
          outputs[0][sampleIndex] = FlushNearZero(inL * dryMix + wetL * mMixSmoothed);
        if (outputs[1])
          outputs[1][sampleIndex] = FlushNearZero(inR * dryMix + wetR * mMixSmoothed);
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "decay")
        mDecay = std::clamp(value, 0.0, 1.0);
      else if (key == "tone")
        mTone = std::clamp(value, 0.0, 1.0);
      else if (key == "drive")
        mDrive = std::clamp(value, 0.0, 1.0);
      else if (key == "mix")
        mMix = std::clamp(value, 0.0, 1.0);
      else
        return;

      UpdateParameters();
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
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "reverb_spring"; }
    [[nodiscard]] std::string GetCategory() const override { return "reverb"; }

  private:
    static constexpr size_t kTankCount = 3;
    static constexpr size_t kDispersionCount = 3;
    static constexpr double kMaxInputDelayMs = 12.0;
    static constexpr double kMaxTensionScale = 1.35;
    static constexpr double kTwoPi = 6.2831853071795864769;
    static constexpr std::array<double, kTankCount> kTankDelayMsL = {26.8, 38.7, 52.1};
    static constexpr std::array<double, kTankCount> kTankDelayMsR = {28.3, 40.4, 54.2};
    static constexpr std::array<double, kDispersionCount> kDispersionDelayMsL = {2.1, 3.4, 5.2};
    static constexpr std::array<double, kDispersionCount> kDispersionDelayMsR = {2.4, 3.7, 5.6};

    size_t DelayMsToSamples(double ms) const
    {
      return std::max<size_t>(1, static_cast<size_t>(ms * mSampleRate * 0.001));
    }

    float DelayMsToSamplesFloat(double ms) const
    {
      return static_cast<float>(std::max(1.0, ms * mSampleRate * 0.001));
    }

    static float ReadFromDelay(const std::vector<float> &buffer, size_t writePos, size_t delaySamples)
    {
      if (buffer.empty())
        return 0.0f;
      const size_t back = std::min(delaySamples, buffer.size() - 1);
      const size_t readPos = (writePos + buffer.size() - back) % buffer.size();
      return buffer[readPos];
    }

    float ReadFromDelayFractional(const std::vector<float> &buffer, size_t writePos, float delaySamples) const
    {
      if (buffer.empty())
        return 0.0f;

      const size_t delayFloor = static_cast<size_t>(delaySamples);
      const float frac = delaySamples - static_cast<float>(delayFloor);
      const float sample0 = ReadFromDelay(buffer, writePos, delayFloor);
      const float sample1 = ReadFromDelay(buffer, writePos, delayFloor + 1);
      return sample0 + frac * (sample1 - sample0);
    }

    float ProcessAllpass(std::vector<float> &buffer, size_t &writePos, float delaySamples, float input, float gain)
    {
      const float delayed = ReadFromDelayFractional(buffer, writePos, delaySamples);
      const float output = delayed - input * gain;
      buffer[writePos] = input + delayed * gain;
      if (++writePos >= buffer.size())
        writePos = 0;
      return output;
    }

    static float ApplyDrive(float sample, float amount)
    {
      if (amount <= 0.0f)
        return sample;
      const float drive = 1.0f + amount * 6.0f;
      const float norm = static_cast<float>(std::tanh(drive));
      if (norm <= 0.0f)
        return sample;
      return static_cast<float>(std::tanh(sample * drive)) / norm;
    }

    static void CopyInputToOutput(float **inputs, float **outputs, int numSamples)
    {
      for (int channel = 0; channel < 2; ++channel)
      {
        if (!outputs[channel])
          continue;
        if (inputs && inputs[channel])
          std::copy_n(inputs[channel], numSamples, outputs[channel]);
        else
          std::fill_n(outputs[channel], numSamples, 0.0f);
      }
    }

    static void ComputeBandpass(double sampleRate, double freq, double q, float &b0, float &b2, float &a1, float &a2)
    {
      if (sampleRate <= 0.0)
      {
        b0 = 1.0f;
        b2 = -1.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        return;
      }

      const double w0 = kTwoPi * freq / sampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * q);
      const double invA0 = 1.0 / (1.0 + alpha);

      b0 = static_cast<float>(alpha * invA0);
      b2 = static_cast<float>(-alpha * invA0);
      a1 = static_cast<float>(-2.0 * cosw0 * invA0);
      a2 = static_cast<float>((1.0 - alpha) * invA0);
    }

    static float ProcessBandpass(float input,
                                 std::array<float, 2> &state,
                                 float b0,
                                 float b2,
                                 float a1,
                                 float a2)
    {
      const float output = b0 * input + state[0];
      state[0] = -a1 * output + state[1];
      state[1] = b2 * input - a2 * output;
      return output;
    }

    float ProcessInputHighpass(float input, float &prevIn, float &prevOut)
    {
      const float output = mInputHpAlpha * (prevOut + input - prevIn);
      prevIn = input;
      prevOut = output;
      return output;
    }

    void UpdateParameters()
    {
      mInputDelaySamples = DelayMsToSamples(1.5 + mDrive * 3.5);
      if (!mInputDelayL.empty())
        mInputDelaySamples = std::min(mInputDelaySamples, mInputDelayL.size() - 1);

      mFeedbackTarget = static_cast<float>(std::clamp(0.50 + mDecay * 0.24, 0.42, 0.74));
      mDampTarget = static_cast<float>(std::clamp(0.10 + mTone * 0.24, 0.08, 0.38));
      mBrightnessTarget = static_cast<float>(std::clamp(0.06 + mTone * 0.22, 0.06, 0.28));
      mTensionScaleTarget = static_cast<float>(std::clamp(0.88 + mTone * 0.30 - mDrive * 0.05, 0.82, kMaxTensionScale));

      const double hpHz = std::clamp(130.0 + mTone * 220.0, 120.0, 380.0);
      const double dt = 1.0 / std::max(1.0, mSampleRate);
      const double rc = 1.0 / (2.0 * 3.14159265358979323846 * hpHz);
      mInputHpAlpha = static_cast<float>(rc / (rc + dt));

      const double dripCenter1 = 1100.0 + mTone * 1700.0;
      const double dripCenter2 = std::clamp(dripCenter1 * 1.85, 2000.0, 6200.0);
      ComputeBandpass(mSampleRate, dripCenter1, 0.85 + mDrive * 0.5, mDripBand1B0, mDripBand1B2, mDripBand1A1, mDripBand1A2);
      ComputeBandpass(mSampleRate, dripCenter2, 0.65 + mDrive * 0.35, mDripBand2B0, mDripBand2B2, mDripBand2A1, mDripBand2A2);

      mDripAttackCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.0025)));
      mDripReleaseCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.045)));
    }

    static float FlushNearZero(float sample)
    {
      return (std::fabs(sample) < 1.0e-9f) ? 0.0f : sample;
    }

    std::vector<float> mInputDelayL;
    std::vector<float> mInputDelayR;
    size_t mInputDelayWrite = 0;
    size_t mInputDelaySamples = 1;

    std::array<std::vector<float>, kTankCount> mTankDelayL;
    std::array<std::vector<float>, kTankCount> mTankDelayR;
    std::array<size_t, kTankCount> mTankWriteL{};
    std::array<size_t, kTankCount> mTankWriteR{};
    std::array<float, kTankCount> mTankLowpassStateL{};
    std::array<float, kTankCount> mTankLowpassStateR{};

    std::array<std::vector<float>, kDispersionCount> mDispersionDelayL;
    std::array<std::vector<float>, kDispersionCount> mDispersionDelayR;
    std::array<size_t, kDispersionCount> mDispersionWriteL{};
    std::array<size_t, kDispersionCount> mDispersionWriteR{};

    double mDecay = 0.42;
    double mTone = 0.52;
    double mDrive = 0.18;
    double mMix = 0.18;

    float mFeedback = 0.70f;
    float mFeedbackTarget = 0.70f;
    float mDamp = 0.20f;
    float mDampTarget = 0.20f;
    float mBrightness = 0.18f;
    float mBrightnessTarget = 0.18f;
    float mTensionScale = 1.0f;
    float mTensionScaleTarget = 1.0f;
    float mMixSmoothed = 0.18f;
    float mDriveSmoothed = 0.18f;
    float mSmoothCoeff = 0.0f;
    float mTensionSmoothCoeff = 0.0f;
    float mWetToneStateL = 0.0f;
    float mWetToneStateR = 0.0f;

    float mInputHpAlpha = 0.95f;
    float mInputHpPrevL = 0.0f;
    float mInputHpPrevR = 0.0f;
    float mInputHpStateL = 0.0f;
    float mInputHpStateR = 0.0f;

    float mDripEnv = 0.0f;
    float mDripAttackCoeff = 0.0f;
    float mDripReleaseCoeff = 0.0f;

    std::array<float, 2> mBandpassState1L{};
    std::array<float, 2> mBandpassState1R{};
    std::array<float, 2> mBandpassState2L{};
    std::array<float, 2> mBandpassState2R{};
    float mDripBand1B0 = 1.0f;
    float mDripBand1B2 = -1.0f;
    float mDripBand1A1 = 0.0f;
    float mDripBand1A2 = 0.0f;
    float mDripBand2B0 = 1.0f;
    float mDripBand2B2 = -1.0f;
    float mDripBand2A1 = 0.0f;
    float mDripBand2A2 = 0.0f;
  };

  inline void RegisterSpringReverbEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbSpring;
    info.aliases = {"reverb_spring"};
    info.displayName = "Spring Reverb";
    info.category = "reverb";
    info.description = "Dedicated spring tank reverb with splashy drip and nonlinear drive";
    info.requiresResource = false;
    info.parameters = {
      {"decay", "Decay", 0.42, 0.0, 1.0, "amount", "spring"},
      {"tone", "Tone", 0.52, 0.0, 1.0, "amount", "spring"},
      {"drive", "Drive", 0.18, 0.0, 1.0, "amount", "spring"},
      {"mix", "Mix", 0.18, 0.0, 1.0, "amount", "spring"}
    };

    EffectRegistry::Instance().Register(info.type,
                                        info,
                                        []()
                                        {
                                          return std::make_unique<SpringReverbEffect>();
                                        });
  }
} // namespace guitarfx