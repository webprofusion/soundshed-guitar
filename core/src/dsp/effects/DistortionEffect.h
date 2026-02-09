#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>

/**
 * @class DistortionEffect
 * @brief Hard-clipping distortion with a one-pole tone filter and wet/dry mix.
 *
 * @details
 * Algorithm overview:
 * - **Drive stage:** Input samples are amplified by a drive gain (1.0 + 29.0 * drive),
 *   then hard-clipped to [-1, 1], producing a classic distortion waveform.
 * - **Tone stage:** The clipped signal is passed through a one-pole low-pass filter
 *   whose cutoff is interpolated between 800 Hz and 8000 Hz based on the tone parameter.
 *   This attenuates harshness at lower tone settings and preserves brightness at higher values.
 * - **Level stage:** The processed signal is scaled by a linear gain derived from a
 *   dB parameter (level).
 * - **Mix stage:** A wet/dry blend combines the original input and processed signal.
 *
 * Parameters:
 * - **drive** [0..1]: Amount of pre-clipping gain; higher values increase saturation.
 * - **tone**  [0..1]: Low-pass cutoff control (800–8000 Hz) for post-distortion shaping.
 * - **level** [-12..12] dB: Output gain applied after distortion and tone filtering.
 * - **mix**   [0..1]: Wet/dry balance (0 = clean, 1 = fully distorted).
 *
 * State:
 * - Maintains separate tone filter states for left/right channels, reset via Reset().
 *
 * Usage:
 * - Call Prepare() to set sample rate and reset state.
 * - Process() performs in-place-style processing with optional null channels.
 */
namespace guitarfx
{
  /**
   * Distortion effect with hard clipping and tone control.
   */
  class DistortionEffect : public EffectProcessor
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
      mToneStateL = 0.0f;
      mToneStateR = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      const float driveGain = 1.0f + 29.0f * mDrive;
      const float levelGain = static_cast<float>(std::pow(10.0, mLevelDb * 0.05));

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        float wetL = std::clamp(inL * driveGain, -1.0f, 1.0f);
        float wetR = std::clamp(inR * driveGain, -1.0f, 1.0f);

        wetL = ApplyTone(wetL, mToneStateL);
        wetR = ApplyTone(wetR, mToneStateR);

        wetL *= levelGain;
        wetR *= levelGain;

        const float outL = inL * (1.0f - mMix) + wetL * mMix;
        const float outR = inR * (1.0f - mMix) + wetR * mMix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "drive")
      {
        mDrive = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "tone")
      {
        mTone = static_cast<float>(std::clamp(value, 0.0, 1.0));
        UpdateToneCoefficient();
      }
      else if (key == "level")
      {
        mLevelDb = static_cast<float>(std::clamp(value, -12.0, 12.0));
      }
      else if (key == "mix")
      {
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "drive")
        return mDrive;
      if (key == "tone")
        return mTone;
      if (key == "level")
        return mLevelDb;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "distortion"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    void UpdateToneCoefficient()
    {
      const float minHz = 800.0f;
      const float maxHz = 8000.0f;
      const float cutoff = minHz + (maxHz - minHz) * mTone;
      const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
      mToneCoef = 1.0f - std::exp(-x);
    }

    float ApplyTone(float input, float &state)
    {
      state += mToneCoef * (input - state);
      return state;
    }

    float mDrive = 0.6f;
    float mTone = 0.5f;
    float mLevelDb = 0.0f;
    float mMix = 1.0f;

    float mToneCoef = 0.0f;
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;
  };

  inline void RegisterDistortionEffect()
  {
    EffectTypeInfo info;
    info.type = "distortion";
    info.displayName = "Distortion";
    info.category = "dynamics";
    info.description = "Hard clipping distortion";
    info.requiresResource = false;
    info.parameters = {
      {"drive", "Drive", 0.6, 0.0, 1.0, "amount"},
      {"tone", "Tone", 0.5, 0.0, 1.0, "amount"},
      {"level", "Level", 0.0, -12.0, 12.0, "dB"},
      {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}
    };

    EffectRegistry::Instance().Register("distortion", info, []()
      { return std::make_unique<DistortionEffect>(); });
  }

} // namespace guitarfx
