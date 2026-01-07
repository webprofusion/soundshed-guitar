#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>
#include <array>

namespace namguitar
{
  /**
   * 4-band parametric EQ effect.
   */
  class ParametricEQEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      UpdateCoefficients();
      Reset();
    }

    void Reset() override
    {
      for (auto& band : mBands)
      {
        band.z1L = band.z2L = 0.0f;
        band.z1R = band.z2R = 0.0f;
      }
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
      for (int i = 0; i < numSamples; ++i)
      {
        float sampleL = inputs[0] ? inputs[0][i] : 0.0f;
        float sampleR = inputs[1] ? inputs[1][i] : 0.0f;

        // Process through each band
        for (auto& band : mBands)
        {
          if (!band.enabled)
            continue;

          // Left channel
          float outL = band.b0 * sampleL + band.b1 * band.z1L + band.b2 * band.z2L
                     - band.a1 * band.x1L - band.a2 * band.x2L;
          band.z2L = band.z1L;
          band.z1L = sampleL;
          band.x2L = band.x1L;
          band.x1L = outL;
          sampleL = outL;

          // Right channel
          float outR = band.b0 * sampleR + band.b1 * band.z1R + band.b2 * band.z2R
                     - band.a1 * band.x1R - band.a2 * band.x2R;
          band.z2R = band.z1R;
          band.z1R = sampleR;
          band.x2R = band.x1R;
          band.x1R = outR;
          sampleR = outR;
        }

        if (outputs[0])
          outputs[0][i] = sampleL;
        if (outputs[1])
          outputs[1][i] = sampleR;
      }
    }

    void SetParam(const std::string& key, double value) override
    {
      if (key == "lowGain")
        mBands[0].gainDb = value;
      else if (key == "lowFreq")
        mBands[0].freq = value;
      else if (key == "lowMidGain")
        mBands[1].gainDb = value;
      else if (key == "lowMidFreq")
        mBands[1].freq = value;
      else if (key == "lowMidQ")
        mBands[1].q = value;
      else if (key == "highMidGain")
        mBands[2].gainDb = value;
      else if (key == "highMidFreq")
        mBands[2].freq = value;
      else if (key == "highMidQ")
        mBands[2].q = value;
      else if (key == "highGain")
        mBands[3].gainDb = value;
      else if (key == "highFreq")
        mBands[3].freq = value;

      UpdateCoefficients();
    }

    void SetConfig(const std::string&, const std::string&) override {}

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
      if (key == "lowGain")
        return mBands[0].gainDb;
      if (key == "lowFreq")
        return mBands[0].freq;
      if (key == "lowMidGain")
        return mBands[1].gainDb;
      if (key == "lowMidFreq")
        return mBands[1].freq;
      if (key == "lowMidQ")
        return mBands[1].q;
      if (key == "highMidGain")
        return mBands[2].gainDb;
      if (key == "highMidFreq")
        return mBands[2].freq;
      if (key == "highMidQ")
        return mBands[2].q;
      if (key == "highGain")
        return mBands[3].gainDb;
      if (key == "highFreq")
        return mBands[3].freq;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "eq_parametric"; }
    [[nodiscard]] std::string GetCategory() const override { return "eq"; }

  private:
    struct Band
    {
      bool enabled = true;
      double gainDb = 0.0;
      double freq = 1000.0;
      double q = 1.0;
      bool isShelf = false;

      // Biquad coefficients
      float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
      float a1 = 0.0f, a2 = 0.0f;

      // State
      float z1L = 0.0f, z2L = 0.0f;
      float z1R = 0.0f, z2R = 0.0f;
      float x1L = 0.0f, x2L = 0.0f;
      float x1R = 0.0f, x2R = 0.0f;
    };

    void UpdateCoefficients()
    {
      // Low shelf
      mBands[0].isShelf = true;
      CalculateLowShelf(mBands[0]);

      // Parametric mid bands
      mBands[1].isShelf = false;
      CalculatePeaking(mBands[1]);

      mBands[2].isShelf = false;
      CalculatePeaking(mBands[2]);

      // High shelf
      mBands[3].isShelf = true;
      CalculateHighShelf(mBands[3]);
    }

    void CalculatePeaking(Band& band)
    {
      if (std::abs(band.gainDb) < 0.001)
      {
        band.b0 = 1.0f;
        band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
        return;
      }

      const double A = std::pow(10.0, band.gainDb / 40.0);
      const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * band.q);

      const double a0 = 1.0 + alpha / A;
      band.b0 = static_cast<float>((1.0 + alpha * A) / a0);
      band.b1 = static_cast<float>((-2.0 * cosw0) / a0);
      band.b2 = static_cast<float>((1.0 - alpha * A) / a0);
      band.a1 = static_cast<float>((-2.0 * cosw0) / a0);
      band.a2 = static_cast<float>((1.0 - alpha / A) / a0);
    }

    void CalculateLowShelf(Band& band)
    {
      if (std::abs(band.gainDb) < 0.001)
      {
        band.b0 = 1.0f;
        band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
        return;
      }

      const double A = std::pow(10.0, band.gainDb / 40.0);
      const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / 2.0 * std::sqrt(2.0);
      const double sqrtA = std::sqrt(A);

      const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
      band.b0 = static_cast<float>(A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0);
      band.b1 = static_cast<float>(2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0);
      band.b2 = static_cast<float>(A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
      band.a1 = static_cast<float>(-2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0);
      band.a2 = static_cast<float>(((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
    }

    void CalculateHighShelf(Band& band)
    {
      if (std::abs(band.gainDb) < 0.001)
      {
        band.b0 = 1.0f;
        band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
        return;
      }

      const double A = std::pow(10.0, band.gainDb / 40.0);
      const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / 2.0 * std::sqrt(2.0);
      const double sqrtA = std::sqrt(A);

      const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
      band.b0 = static_cast<float>(A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0);
      band.b1 = static_cast<float>(-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0);
      band.b2 = static_cast<float>(A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
      band.a1 = static_cast<float>(2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0);
      band.a2 = static_cast<float>(((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
    }

    std::array<Band, 4> mBands = {{
      {true, 0.0, 100.0, 0.707, true},   // Low shelf
      {true, 0.0, 400.0, 1.0, false},    // Low mid
      {true, 0.0, 2000.0, 1.0, false},   // High mid
      {true, 0.0, 8000.0, 0.707, true}   // High shelf
    }};
  };

  inline void RegisterParametricEQEffect()
  {
    EffectTypeInfo info;
    info.type = "eq_parametric";
    info.displayName = "Parametric EQ";
    info.category = "eq";
    info.description = "4-band parametric equalizer";
    info.requiresResource = false;
    info.parameters = {
      {"lowGain", "Low Gain", 0.0, -12.0, 12.0, "dB"},
      {"lowFreq", "Low Freq", 100.0, 20.0, 500.0, "Hz"},
      {"lowMidGain", "Low Mid Gain", 0.0, -12.0, 12.0, "dB"},
      {"lowMidFreq", "Low Mid Freq", 400.0, 100.0, 2000.0, "Hz"},
      {"lowMidQ", "Low Mid Q", 1.0, 0.1, 10.0, ""},
      {"highMidGain", "High Mid Gain", 0.0, -12.0, 12.0, "dB"},
      {"highMidFreq", "High Mid Freq", 2000.0, 500.0, 8000.0, "Hz"},
      {"highMidQ", "High Mid Q", 1.0, 0.1, 10.0, ""},
      {"highGain", "High Gain", 0.0, -12.0, 12.0, "dB"},
      {"highFreq", "High Freq", 8000.0, 2000.0, 16000.0, "Hz"}
    };

    EffectRegistry::Instance().Register("eq_parametric", info, []() {
      return std::make_unique<ParametricEQEffect>();
    });
  }

} // namespace namguitar
