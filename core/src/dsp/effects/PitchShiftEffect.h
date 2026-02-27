#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * Pitchshift effect using Signalsmith Stretch.
   * Supports stepped semitones or free-form pitch shift (-12 to +12).
   */
  class PitchShiftEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      mWetL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
      mWetR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
      mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

      mStretch.presetCheaper(2, static_cast<float>(sampleRate), false);
      ApplyTranspose();
      mConfigured = true;
      Reset();
    }

    void Reset() override
    {
      if (mConfigured)
      {
        mStretch.reset();
      }
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!inputs || !outputs)
        return;

      // Clamp to allocated buffer size to prevent out-of-bounds writes
      numSamples = std::min(numSamples, mMaxBlockSize);

      if (GetMappedSemitones() == 0.0 && mMix >= 1.0)
      {
        for (int ch = 0; ch < 2; ++ch)
        {
          if (inputs[ch] && outputs[ch])
          {
            std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
          }
        }
        return;
      }

      if (!mConfigured)
        return;

      if (static_cast<size_t>(numSamples) > mWetL.size())
      {
        mWetL.resize(static_cast<size_t>(numSamples), 0.0f);
        mWetR.resize(static_cast<size_t>(numSamples), 0.0f);
        mZero.resize(static_cast<size_t>(numSamples), 0.0f);
      }

      float *inputPtrs[2] = {
        inputs[0] ? inputs[0] : mZero.data(),
        inputs[1] ? inputs[1] : mZero.data()
      };
      float *wetPtrs[2] = { mWetL.data(), mWetR.data() };

      mStretch.process(inputPtrs, numSamples, wetPtrs, numSamples);

      const float dryMix = static_cast<float>(1.0 - mMix);
      const float wetMix = static_cast<float>(mMix);

      for (int i = 0; i < numSamples; ++i)
      {
        if (outputs[0])
        {
          const float dry = inputs[0] ? inputs[0][i] : 0.0f;
          outputs[0][i] = dry * dryMix + mWetL[static_cast<size_t>(i)] * wetMix;
        }
        if (outputs[1])
        {
          const float dry = inputs[1] ? inputs[1][i] : 0.0f;
          outputs[1][i] = dry * dryMix + mWetR[static_cast<size_t>(i)] * wetMix;
        }
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "semitones")
      {
        mSemitoneControl = std::clamp(value, -1.0, 1.0);
        ApplyTranspose();
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
      else if (key == "stepMode")
      {
        mStepMode = value > 0.5;
        ApplyTranspose();
      }
      else if (key == "minSemitones")
      {
        mMinSemitones = std::clamp(value, kHardMinSemitones, mMaxSemitones);
        ApplyTranspose();
      }
      else if (key == "maxSemitones")
      {
        mMaxSemitones = std::clamp(value, mMinSemitones, kHardMaxSemitones);
        ApplyTranspose();
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "semitones")
        return mSemitoneControl;
      if (key == "mix")
        return mMix;
      if (key == "stepMode")
        return mStepMode ? 1.0 : 0.0;
      if (key == "minSemitones")
        return mMinSemitones;
      if (key == "maxSemitones")
        return mMaxSemitones;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "pitch_shift"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

    [[nodiscard]] int GetLatencySamples() const override
    {
      // Signalsmith Stretch introduces latency (typically ~1024 samples); report it
      return mConfigured ? static_cast<int>(mStretch.inputLatency()) : 0;
    }

  private:
    double GetMappedSemitones() const
    {
      const double rawRange = mMaxSemitones - mMinSemitones;
      const double range = rawRange < 0.0 ? 0.0 : rawRange;
      double semitones = mMinSemitones + (mSemitoneControl + 1.0) * 0.5 * range;
      semitones = std::clamp(semitones, mMinSemitones, mMaxSemitones);
      if (mStepMode)
      {
        semitones = std::round(semitones);
      }
      return semitones;
    }

    void ApplyTranspose()
    {
      if (!mConfigured)
        return;
      const double semitones = GetMappedSemitones();
      const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
      mStretch.setTransposeSemitones(static_cast<float>(semitones), tonalityLimit);
    }

    static constexpr double kTonalityLimitHz = 8000.0;

    static constexpr double kHardMinSemitones = -12.0;
    static constexpr double kHardMaxSemitones = 12.0;

    double mSemitoneControl = 0.0;
    double mMix = 1.0;
    bool mStepMode = true;
    double mMinSemitones = kHardMinSemitones;
    double mMaxSemitones = kHardMaxSemitones;
    bool mConfigured = false;

    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
  };

  inline void RegisterPitchShiftEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kPitchShift;
    info.aliases = {"pitch_shift"};
    info.displayName = "Pitch Shift";
    info.category = "pitch";
    info.description = "Pitch shift with stepped or free-form control";
    info.requiresResource = false;
    info.parameters = {
      {"semitones", "Semitones", 0.0, -1.0, 1.0, "amount"},
      {"minSemitones", "Min Semitones", -12.0, -12.0, 12.0, "st"},
      {"maxSemitones", "Max Semitones", 12.0, -12.0, 12.0, "st"},
      {"mix", "Mix", 1.0, 0.0, 1.0, "%"},
      {"stepMode", "Step Mode", 1.0, 0.0, 1.0, "mode"}};
    EffectRegistry::Instance().Register(info.type, info, []()
                                        { return std::make_unique<PitchShiftEffect>(); });
  }

} // namespace guitarfx
