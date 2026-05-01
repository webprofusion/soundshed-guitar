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
     * Transpose effect using Signalsmith Stretch for integer semitone steps.
     */
  class TransposeEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      mWetL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
      mWetR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
      mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

      // Latency/quality toggle currently hard-coded to best latency.
      mStretch.presetCheaper(2, static_cast<float>(sampleRate), false);
      //      mStretch.presetDefault(2, static_cast<float>(sampleRate), true);
      mConfigured = true;
      ApplyTranspose();
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

      if (mSemitones == 0 && mMix >= 1.0)
      {
        CopyStereoInputToOutput(inputs, outputs, numSamples);
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

      // Render transposed audio into wet buffers, then crossfade against dry
      // input so zero-shift/high-mix cases can remain transparent.
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
        const int clamped = static_cast<int>(std::round(std::clamp(value, -36.0, 12.0)));
        if (clamped != mSemitones)
        {
          mSemitones = clamped;
          ApplyTranspose();
        }
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "semitones")
        return static_cast<double>(mSemitones);
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "transpose"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

    [[nodiscard]] int GetLatencySamples() const override
    {
      return mConfigured ? static_cast<int>(mStretch.inputLatency()) : 0;
    }

  private:
    void ApplyTranspose()
    {
      if (!mConfigured || mSampleRate <= 0.0)
        return;
      const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
      mStretch.setTransposeSemitones(static_cast<float>(mSemitones), tonalityLimit);
    }

    static constexpr double kTonalityLimitHz = 16000.0; //8000

    int mSemitones = 0;
    double mMix = 1.0;
    bool mConfigured = false;

    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
  };

  inline void RegisterTransposeEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kTranspose;
    info.aliases = {"transpose"};
    info.displayName = "Transpose";
    info.category = "pitch";
    info.description = "High-quality transpose effect";
    info.requiresResource = false;
    info.parameters = {
        {"semitones", "Semitones", 0.0, -36.0, 12.0, "st", "", false, 1.0},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}};
    EffectRegistry::Instance().Register(info.type, info, []()
                                        { return std::make_unique<TransposeEffect>(); });
  }

} // namespace guitarfx
