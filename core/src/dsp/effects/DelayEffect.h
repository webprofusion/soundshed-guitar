#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * Digital delay with high/low cut filtering, stereo spread, ping-pong,
   * LFO modulation, drive saturation, and ducking.
   */
  class DelayEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate   = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // 2.1 s headroom covers 2 s delay + worst-case LFO modulation
      const size_t maxSamples = static_cast<size_t>(sampleRate * 2.1);
      mBufferL.assign(maxSamples, 0.0f);
      mBufferR.assign(maxSamples, 0.0f);

      UpdateDelaySamples();
      UpdateFilters();
      Reset();
    }

    void Reset() override
    {
      std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
      std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
      mWritePos   = 0;
      mLfoPhase   = 0.0f;
      mEnvelope   = 0.0f;
      mLpStateL   = mLpStateR   = 0.0f;
      mHpStateL   = mHpStateR   = 0.0f;
      mHpPrevInL  = mHpPrevInR  = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (mBufferL.empty())
        return;

      const float feedback = static_cast<float>(mFeedback);
      const float wet      = static_cast<float>(mMix);
      const float dry      = 1.0f - wet;
      const float drive    = static_cast<float>(mDrive);
      const float ducking  = static_cast<float>(mDucking);
      const float lfoStep  = static_cast<float>(mModRate / mSampleRate);
      const float modAmp   = static_cast<float>(mModDepth * 0.001 * mSampleRate);
      const bool  pingPong = (mStereoMode == 1);
      const size_t bufSize = mBufferL.size();
      const double maxDelay = static_cast<double>(bufSize - 2);

      for (int i = 0; i < numSamples; ++i)
      {
        // LFO
        const float lfoVal = std::sin(mLfoPhase * 6.28318530f);
        mLfoPhase += lfoStep;
        if (mLfoPhase >= 1.0f)
          mLfoPhase -= 1.0f;

        const double delayL = std::clamp(mDelaySamples  + lfoVal * modAmp, 1.0, maxDelay);
        const double delayR = std::clamp(mDelaySamples + mSpreadSamples + lfoVal * modAmp, 1.0, maxDelay);

        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : inL;

        // Envelope follower for ducking
        const float inputPeak = std::max(std::abs(inL), std::abs(inR));
        if (inputPeak > mEnvelope)
          mEnvelope += 0.001f  * (inputPeak - mEnvelope);
        else
          mEnvelope += 0.0001f * (inputPeak - mEnvelope);

        const float duckGain = 1.0f - ducking * std::min(mEnvelope * 4.0f, 1.0f);

        // Read with linear interpolation
        float delayedL = ReadInterp(mBufferL, bufSize, delayL);
        float delayedR = ReadInterp(mBufferR, bufSize, delayR);

        // Tone shaping on delayed signal (shapes feedback colour on each repeat)
        delayedL = ApplyLP(mLpStateL, mLpCoeff, delayedL);
        delayedR = ApplyLP(mLpStateR, mLpCoeff, delayedR);
        delayedL = ApplyHP(mHpStateL, mHpPrevInL, mHpCoeff, delayedL);
        delayedR = ApplyHP(mHpStateR, mHpPrevInR, mHpCoeff, delayedR);

        // Ping-pong: swap which channel feeds back into which buffer
        float fbL = (pingPong ? delayedR : delayedL) * feedback;
        float fbR = (pingPong ? delayedL : delayedR) * feedback;

        // Soft saturation in feedback path
        if (drive > 0.0f)
        {
          const float g = 1.0f + drive * 4.0f;
          fbL = std::tanh(fbL * g) / g;
          fbR = std::tanh(fbR * g) / g;
        }

        mBufferL[mWritePos] = inL + fbL;
        mBufferR[mWritePos] = inR + fbR;

        if (outputs[0])
          outputs[0][i] = inL * dry + delayedL * wet * duckGain;
        if (outputs[1])
          outputs[1][i] = inR * dry + delayedR * wet * duckGain;

        mWritePos = (mWritePos + 1) % bufSize;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "time" || key == "timeMs")
      {
        mDelayMs = std::clamp(value, 1.0, 2000.0);
        UpdateDelaySamples();
      }
      else if (key == "feedback")
        mFeedback = std::clamp(value, 0.0, 0.95);
      else if (key == "mix")
        mMix = std::clamp(value, 0.0, 1.0);
      else if (key == "highCut")
      {
        mHighCutHz = std::clamp(value, 200.0, 20000.0);
        UpdateFilters();
      }
      else if (key == "lowCut")
      {
        mLowCutHz = std::clamp(value, 20.0, 5000.0);
        UpdateFilters();
      }
      else if (key == "stereoMode")
        mStereoMode = static_cast<int>(std::round(std::clamp(value, 0.0, 1.0)));
      else if (key == "spread")
      {
        mSpreadMs = std::clamp(value, 0.0, 50.0);
        UpdateDelaySamples();
      }
      else if (key == "modRate")
        mModRate = std::clamp(value, 0.0, 10.0);
      else if (key == "modDepth")
        mModDepth = std::clamp(value, 0.0, 20.0);
      else if (key == "drive")
        mDrive = std::clamp(value, 0.0, 1.0);
      else if (key == "ducking")
        mDucking = std::clamp(value, 0.0, 1.0);
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "time" || key == "timeMs") return mDelayMs;
      if (key == "feedback")   return mFeedback;
      if (key == "mix")        return mMix;
      if (key == "highCut")    return mHighCutHz;
      if (key == "lowCut")     return mLowCutHz;
      if (key == "stereoMode") return mStereoMode;
      if (key == "spread")     return mSpreadMs;
      if (key == "modRate")    return mModRate;
      if (key == "modDepth")   return mModDepth;
      if (key == "drive")      return mDrive;
      if (key == "ducking")    return mDucking;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "delay_digital"; }
    [[nodiscard]] std::string GetCategory() const override { return "delay"; }

  private:
    // Read from circular buffer with linear interpolation.
    [[nodiscard]] float ReadInterp(const std::vector<float> &buf, size_t bufSize, double delay) const
    {
      const size_t intD = static_cast<size_t>(delay);
      const float  frac = static_cast<float>(delay - static_cast<double>(intD));
      const size_t posA = (mWritePos + bufSize - intD)     % bufSize;
      const size_t posB = (mWritePos + bufSize - intD - 1) % bufSize;
      return buf[posA] * (1.0f - frac) + buf[posB] * frac;
    }

    // One-pole low-pass
    static float ApplyLP(float &state, float coeff, float in)
    {
      state = coeff * in + (1.0f - coeff) * state;
      return state;
    }

    // One-pole high-pass
    static float ApplyHP(float &state, float &prevIn, float coeff, float in)
    {
      state  = coeff * (state + in - prevIn);
      prevIn = in;
      return state;
    }

    void UpdateDelaySamples()
    {
      mDelaySamples  = mSampleRate * mDelayMs  * 0.001;
      mSpreadSamples = mSampleRate * mSpreadMs * 0.001;
      if (!mBufferL.empty())
      {
        const double maxD = static_cast<double>(mBufferL.size() - 2);
        mDelaySamples  = std::min(mDelaySamples,  maxD);
        mSpreadSamples = std::min(mSpreadSamples, maxD - mDelaySamples);
      }
    }

    void UpdateFilters()
    {
      if (mSampleRate <= 0.0)
        return;
      constexpr double pi2 = 6.28318530717958647;
      mLpCoeff = static_cast<float>(1.0 - std::exp(-pi2 * mHighCutHz / mSampleRate));
      mHpCoeff = static_cast<float>(std::exp(-pi2 * mLowCutHz / mSampleRate));
    }

    // Buffers
    std::vector<float> mBufferL, mBufferR;
    size_t mWritePos = 0;

    // Derived state
    double mDelaySamples  = 0.0;
    double mSpreadSamples = 0.0;

    // Parameters
    double mDelayMs    = 300.0;
    double mFeedback   = 0.3;
    double mMix        = 0.3;
    double mHighCutHz  = 8000.0;
    double mLowCutHz   = 20.0;
    int    mStereoMode = 0;
    double mSpreadMs   = 0.0;
    double mModRate    = 0.0;
    double mModDepth   = 0.0;
    double mDrive      = 0.0;
    double mDucking    = 0.0;

    // Filter coefficients & state
    float mLpCoeff   = 1.0f;
    float mHpCoeff   = 1.0f;
    float mLpStateL  = 0.0f, mLpStateR  = 0.0f;
    float mHpStateL  = 0.0f, mHpStateR  = 0.0f;
    float mHpPrevInL = 0.0f, mHpPrevInR = 0.0f;

    // LFO & envelope
    float mLfoPhase = 0.0f;
    float mEnvelope = 0.0f;
  };

  inline void RegisterDelayEffect()
  {
    EffectTypeInfo info;
    info.type         = "delay_digital";
    info.displayName  = "Digital Delay";
    info.category     = "delay";
    info.description  = "Stereo digital delay with tone shaping, modulation, drive, and ducking";
    info.requiresResource = false;
    info.parameters = {
        {"time",       "Time",        300.0,  1.0,    2000.0,  "ms"},
        {"feedback",   "Feedback",    0.3,    0.0,    0.95,    ""},
        {"mix",        "Mix",         0.3,    0.0,    1.0,     ""},
        {"highCut",    "High Cut",    8000.0, 200.0,  20000.0, "Hz"},
        {"lowCut",     "Low Cut",     20.0,   20.0,   5000.0,  "Hz"},
        {"drive",      "Drive",       0.0,    0.0,    1.0,     ""},
        {"stereoMode", "Stereo Mode", 0.0,    0.0,    1.0,     ""},
        {"spread",     "Spread",      0.0,    0.0,    50.0,    "ms",  "", true},
        {"modRate",    "Mod Rate",    0.0,    0.0,    10.0,    "Hz",  "", true},
        {"modDepth",   "Mod Depth",   0.0,    0.0,    20.0,    "ms",  "", true},
        {"ducking",    "Ducking",     0.0,    0.0,    1.0,     "",    "", true}};

    EffectRegistry::Instance().Register("delay_digital", info, []()
                                        { return std::make_unique<DelayEffect>(); });
  }

} // namespace guitarfx
