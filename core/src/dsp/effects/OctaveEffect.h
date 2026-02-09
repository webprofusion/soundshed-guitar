#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
  /**
   * Octave effect with octave up/down blend.
   */
  class OctaveEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      UpdateToneCoefficient();
      Reset();
    }

    void Reset() override
    {
      mPrevMono = 0.0f;
      mOctavePolarity = 1.0f;
      mToneStateUp = 0.0f;
      mToneStateDown = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        const float mono = 0.5f * (inL + inR);
        if ((mono >= 0.0f && mPrevMono < 0.0f) || (mono < 0.0f && mPrevMono >= 0.0f))
        {
          mOctavePolarity = -mOctavePolarity;
        }
        mPrevMono = mono;

        float octaveDown = mono * mOctavePolarity;
        float octaveUp = std::abs(mono);

        octaveUp = ApplyTone(octaveUp, mToneStateUp);
        octaveDown = ApplyTone(octaveDown, mToneStateDown);

        const float wet = octaveUp * mOctaveUp + octaveDown * mOctaveDown;
        const float outL = inL * (1.0f - mMix) + wet * mMix;
        const float outR = inR * (1.0f - mMix) + wet * mMix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "octaveUp")
      {
        mOctaveUp = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "octaveDown")
      {
        mOctaveDown = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "tone")
      {
        mTone = static_cast<float>(std::clamp(value, 0.0, 1.0));
        UpdateToneCoefficient();
      }
      else if (key == "mix")
      {
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "octaveUp")
        return mOctaveUp;
      if (key == "octaveDown")
        return mOctaveDown;
      if (key == "tone")
        return mTone;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "octave"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    void UpdateToneCoefficient()
    {
      const float minHz = 400.0f;
      const float maxHz = 4000.0f;
      const float cutoff = minHz + (maxHz - minHz) * mTone;
      const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
      mToneCoef = 1.0f - std::exp(-x);
    }

    float ApplyTone(float input, float &state)
    {
      state += mToneCoef * (input - state);
      return state;
    }

    float mOctaveUp = 0.6f;
    float mOctaveDown = 0.6f;
    float mTone = 0.5f;
    float mMix = 1.0f;

    float mToneCoef = 0.0f;
    float mToneStateUp = 0.0f;
    float mToneStateDown = 0.0f;

    float mPrevMono = 0.0f;
    float mOctavePolarity = 1.0f;
  };

  inline void RegisterOctaveEffect()
  {
    EffectTypeInfo info;
    info.type = "octave";
    info.displayName = "Octave";
    info.category = "modulation";
    info.description = "Octave up/down blend";
    info.requiresResource = false;
    info.parameters = {
      {"octaveUp", "Oct Up", 0.6, 0.0, 1.0, "amount"},
      {"octaveDown", "Oct Down", 0.6, 0.0, 1.0, "amount"},
      {"tone", "Tone", 0.5, 0.0, 1.0, "amount"},
      {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}
    };

    EffectRegistry::Instance().Register("octave", info, []()
      { return std::make_unique<OctaveEffect>(); });
  }

} // namespace guitarfx
