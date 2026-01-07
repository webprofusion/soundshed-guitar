#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>

namespace namguitar
{
  /**
   * Simple gain stage effect.
   */
  class GainEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
    }

    void Reset() override {}

    void Process(float** inputs, float** outputs, int numSamples) override
    {
      const float gain = static_cast<float>(std::pow(10.0, mGainDb / 20.0));

      for (int ch = 0; ch < 2; ++ch)
      {
        if (inputs[ch] && outputs[ch])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            outputs[ch][i] = inputs[ch][i] * gain;
          }
        }
      }
    }

    void SetParam(const std::string& key, double value) override
    {
      if (key == "gain" || key == "gainDb")
      {
        mGainDb = value;
      }
    }

    void SetConfig(const std::string&, const std::string&) override {}

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
      if (key == "gain" || key == "gainDb")
        return mGainDb;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "gain"; }
    [[nodiscard]] std::string GetCategory() const override { return "utility"; }

  private:
    double mGainDb = 0.0;
  };

  // Register the effect
  inline void RegisterGainEffect()
  {
    EffectTypeInfo info;
    info.type = "gain";
    info.displayName = "Gain";
    info.category = "utility";
    info.description = "Simple gain stage";
    info.requiresResource = false;
    info.parameters = {
      {"gainDb", "Gain", 0.0, -24.0, 24.0, "dB"}
    };

    EffectRegistry::Instance().Register("gain", info, []() {
      return std::make_unique<GainEffect>();
    });
  }

} // namespace namguitar
