#pragma once

#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <algorithm>
#include <array>

namespace guitarfx
{
  /**
   * 4-band parametric EQ effect.
   */
  class ParametricEQEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      UpdateCoefficients();
      Reset();
    }

    void Reset() override
    {
      for (auto &band : mBands)
      {
        ResetBandState(band);
      }
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      for (int i = 0; i < numSamples; ++i)
      {
        float sampleL = inputs[0] ? inputs[0][i] : 0.0f;
        float sampleR = inputs[1] ? inputs[1][i] : 0.0f;

        // Process through each band
        for (auto &band : mBands)
        {
          if (!band.enabled)
            continue;

          sampleL = SanitizeSample(sampleL);
          sampleR = SanitizeSample(sampleR);
          SanitizeBandState(band);

          // Left channel
          float outL = band.b0 * sampleL + band.b1 * band.z1L + band.b2 * band.z2L - band.a1 * band.x1L - band.a2 * band.x2L;
          if (!std::isfinite(outL))
          {
            ResetBandState(band);
            outL = sampleL;
          }
          band.z2L = band.z1L;
          band.z1L = sampleL;
          band.x2L = band.x1L;
          band.x1L = outL;
          sampleL = outL;

          // Right channel
          float outR = band.b0 * sampleR + band.b1 * band.z1R + band.b2 * band.z2R - band.a1 * band.x1R - band.a2 * band.x2R;
          if (!std::isfinite(outR))
          {
            ResetBandState(band);
            outR = sampleR;
          }
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

    void SetParam(const std::string &key, double value) override
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

      ClampBandParams();
      UpdateCoefficients();
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
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
      ClampBandParams();

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

      // Reset filter state to prevent instability transients after coefficient changes
      for (auto &band : mBands)
      {
        SanitizeBandCoefficients(band);
        ResetBandState(band);
      }
    }

    void ClampBandParams()
    {
      const double maxFreq = std::max(20.0, mSampleRate * 0.49);
      mBands[0].gainDb = ClampFinite(mBands[0].gainDb, -12.0, 12.0, 0.0);
      mBands[0].freq = ClampFinite(mBands[0].freq, 20.0, std::min(500.0, maxFreq), 100.0);

      mBands[1].gainDb = ClampFinite(mBands[1].gainDb, -12.0, 12.0, 0.0);
      mBands[1].freq = ClampFinite(mBands[1].freq, 100.0, std::min(2000.0, maxFreq), 400.0);
      mBands[1].q = ClampFinite(mBands[1].q, 0.1, 10.0, 1.0);

      mBands[2].gainDb = ClampFinite(mBands[2].gainDb, -12.0, 12.0, 0.0);
      mBands[2].freq = ClampFinite(mBands[2].freq, 500.0, std::min(8000.0, maxFreq), 2000.0);
      mBands[2].q = ClampFinite(mBands[2].q, 0.1, 10.0, 1.0);

      mBands[3].gainDb = ClampFinite(mBands[3].gainDb, -12.0, 12.0, 0.0);
      mBands[3].freq = ClampFinite(mBands[3].freq, 2000.0, std::min(16000.0, maxFreq), 8000.0);
    }

    static double ClampFinite(double value, double minimum, double maximum, double fallback)
    {
      if (!std::isfinite(value))
        return fallback;
      return std::clamp(value, minimum, maximum);
    }

    static float SanitizeSample(float sample)
    {
      return std::isfinite(sample) ? sample : 0.0f;
    }

    static void ResetBandState(Band &band)
    {
      band.z1L = band.z2L = 0.0f;
      band.z1R = band.z2R = 0.0f;
      band.x1L = band.x2L = 0.0f;
      band.x1R = band.x2R = 0.0f;
    }

    static void SanitizeBandState(Band &band)
    {
      band.z1L = SanitizeSample(band.z1L);
      band.z2L = SanitizeSample(band.z2L);
      band.z1R = SanitizeSample(band.z1R);
      band.z2R = SanitizeSample(band.z2R);
      band.x1L = SanitizeSample(band.x1L);
      band.x2L = SanitizeSample(band.x2L);
      band.x1R = SanitizeSample(band.x1R);
      band.x2R = SanitizeSample(band.x2R);
    }

    static void SetIdentity(Band &band)
    {
      band.b0 = 1.0f;
      band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
    }

    static void SanitizeBandCoefficients(Band &band)
    {
      if (!std::isfinite(band.b0) || !std::isfinite(band.b1) || !std::isfinite(band.b2) ||
          !std::isfinite(band.a1) || !std::isfinite(band.a2))
      {
        SetIdentity(band);
      }
    }

    void CalculatePeaking(Band &band)
    {
      if (std::abs(band.gainDb) < 0.001)
      {
        SetIdentity(band);
        return;
      }

      const double A = std::pow(10.0, band.gainDb / 40.0);
      const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / (2.0 * band.q);

      const double a0 = 1.0 + alpha / A;
      if (!std::isfinite(a0) || std::abs(a0) < 1.0e-9)
      {
        SetIdentity(band);
        return;
      }
      band.b0 = static_cast<float>((1.0 + alpha * A) / a0);
      band.b1 = static_cast<float>((-2.0 * cosw0) / a0);
      band.b2 = static_cast<float>((1.0 - alpha * A) / a0);
      band.a1 = static_cast<float>((-2.0 * cosw0) / a0);
      band.a2 = static_cast<float>((1.0 - alpha / A) / a0);
    }

    void CalculateLowShelf(Band &band)
    {
      if (std::abs(band.gainDb) < 0.001)
      {
        SetIdentity(band);
        return;
      }

      const double A = std::pow(10.0, band.gainDb / 40.0);
      const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / 2.0 * std::sqrt(2.0);
      const double sqrtA = std::sqrt(A);

      const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
      if (!std::isfinite(a0) || std::abs(a0) < 1.0e-9)
      {
        SetIdentity(band);
        return;
      }
      band.b0 = static_cast<float>(A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0);
      band.b1 = static_cast<float>(2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0);
      band.b2 = static_cast<float>(A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
      band.a1 = static_cast<float>(-2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0);
      band.a2 = static_cast<float>(((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
    }

    void CalculateHighShelf(Band &band)
    {
      if (std::abs(band.gainDb) < 0.001)
      {
        SetIdentity(band);
        return;
      }

      const double A = std::pow(10.0, band.gainDb / 40.0);
      const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
      const double cosw0 = std::cos(w0);
      const double sinw0 = std::sin(w0);
      const double alpha = sinw0 / 2.0 * std::sqrt(2.0);
      const double sqrtA = std::sqrt(A);

      const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
      if (!std::isfinite(a0) || std::abs(a0) < 1.0e-9)
      {
        SetIdentity(band);
        return;
      }
      band.b0 = static_cast<float>(A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0);
      band.b1 = static_cast<float>(-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0);
      band.b2 = static_cast<float>(A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
      band.a1 = static_cast<float>(2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0);
      band.a2 = static_cast<float>(((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
    }

    std::array<Band, 4> mBands = {{
        {true, 0.0, 100.0, 0.707, true}, // Low shelf
        {true, 0.0, 400.0, 1.0, false},  // Low mid
        {true, 0.0, 2000.0, 1.0, false}, // High mid
        {true, 0.0, 8000.0, 0.707, true} // High shelf
    }};
  };

  inline void RegisterParametricEQEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kEqParametric;
    info.aliases = {"eq_parametric"};
    info.displayName = "Parametric EQ";
    info.category = "eq";
    info.description = "4-band parametric equalizer";
    info.requiresResource = false;
    info.parameters = {
      {"lowGain",    "Low Gain",      0.0, -12.0,  12.0,   "dB",     "Low",      false, 0.0, {}},
      {"lowFreq",    "Low Freq",    100.0,  20.0,  500.0,   "Hz",     "Low",      false, 0.0, {}},
      {"lowMidGain", "Low-Mid Gain",  0.0, -12.0,  12.0,   "dB",     "Low Mid",  false, 0.0, {}},
      {"lowMidFreq", "Low-Mid Freq",400.0, 100.0, 2000.0,  "Hz",     "Low Mid",  false, 0.0, {}},
      {"lowMidQ",    "Low-Mid Q",     1.0,   0.1,   10.0,   "amount", "Low Mid",  false, 0.0, {}},
      {"highMidGain","High-Mid Gain", 0.0, -12.0,  12.0,   "dB",     "High Mid", false, 0.0, {}},
      {"highMidFreq","High-Mid Freq",2000.0,500.0,8000.0,  "Hz",     "High Mid", false, 0.0, {}},
      {"highMidQ",   "High-Mid Q",    1.0,   0.1,   10.0,   "amount", "High Mid", false, 0.0, {}},
      {"highGain",   "High Gain",     0.0, -12.0,  12.0,   "dB",     "High",     false, 0.0, {}},
      {"highFreq",   "High Freq",  8000.0,2000.0,16000.0,  "Hz",     "High",     false, 0.0, {}}};

    EffectRegistry::Instance().Register(info.type, info, []()
                                        { return std::make_unique<ParametricEQEffect>(); });
  }

} // namespace guitarfx
