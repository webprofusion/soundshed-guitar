#pragma once

#include <cmath>
#include <numbers>
#include <array>

namespace guitarfx
{
  // 4-band Parametric EQ with:
  // - Band 1: Low Shelf (Bass) - adjustable frequency and gain
  // - Band 2: Low-Mid Peaking - adjustable frequency, gain, and Q
  // - Band 3: High-Mid Peaking - adjustable frequency, gain, and Q
  // - Band 4: High Shelf (Treble) - adjustable frequency and gain
  //
  // Each band uses a biquad filter in Transposed Direct Form II for efficiency.
  class ParametricEQ
  {
  public:
    static constexpr int kNumBands = 4;

    enum BandType
    {
      kBandLowShelf = 0,
      kBandLowMid,
      kBandHighMid,
      kBandHighShelf
    };

    ParametricEQ() = default;

    void Prepare(double sampleRate)
    {
      mSampleRate = sampleRate;
      UpdateAllCoefficients();
      Reset();
    }

    void Reset()
    {
      for (int i = 0; i < kNumBands; ++i)
      {
        mState1[i] = 0.0;
        mState2[i] = 0.0;
      }
    }

    // Process a single sample through all 4 EQ bands
    double Process(double sample)
    {
      double out = sample;
      for (int i = 0; i < kNumBands; ++i)
      {
        if (mBandEnabled[i])
        {
          out = ProcessBiquad(out, mB0[i], mB1[i], mB2[i], mA1[i], mA2[i], mState1[i], mState2[i]);
        }
      }
      return out;
    }

    // Enable/disable the entire EQ
    void SetEnabled(bool enabled) { mEnabled = enabled; }
    [[nodiscard]] bool IsEnabled() const { return mEnabled; }

    // Enable/disable individual bands
    void SetBandEnabled(int band, bool enabled)
    {
      if (band >= 0 && band < kNumBands)
      {
        mBandEnabled[band] = enabled;
      }
    }

    // Set band gain in dB (-12 to +12 dB typical)
    void SetBandGain(int band, double gainDb)
    {
      if (band >= 0 && band < kNumBands)
      {
        mGainDb[band] = std::clamp(gainDb, -12.0, 12.0);
        UpdateBandCoefficients(band);
      }
    }

    // Set band frequency in Hz
    void SetBandFrequency(int band, double freqHz)
    {
      if (band >= 0 && band < kNumBands)
      {
        // Clamp to reasonable audio range
        mFrequency[band] = std::clamp(freqHz, 20.0, 20000.0);
        UpdateBandCoefficients(band);
      }
    }

    // Set band Q (only affects peaking bands, 0.1 to 10.0)
    void SetBandQ(int band, double q)
    {
      if (band >= 0 && band < kNumBands)
      {
        mQ[band] = std::clamp(q, 0.1, 10.0);
        UpdateBandCoefficients(band);
      }
    }

    [[nodiscard]] double GetBandGain(int band) const { return (band >= 0 && band < kNumBands) ? mGainDb[band] : 0.0; }
    [[nodiscard]] double GetBandFrequency(int band) const { return (band >= 0 && band < kNumBands) ? mFrequency[band] : 1000.0; }
    [[nodiscard]] double GetBandQ(int band) const { return (band >= 0 && band < kNumBands) ? mQ[band] : 1.0; }

  private:
    // Transposed Direct Form II biquad - most efficient for real-time audio
    static double ProcessBiquad(double input, double b0, double b1, double b2,
                                double a1, double a2, double &s1, double &s2)
    {
      const double output = b0 * input + s1;
      s1 = b1 * input - a1 * output + s2;
      s2 = b2 * input - a2 * output;
      return output;
    }

    void UpdateAllCoefficients()
    {
      for (int i = 0; i < kNumBands; ++i)
      {
        UpdateBandCoefficients(i);
      }
    }

    void UpdateBandCoefficients(int band)
    {
      if (mSampleRate <= 0.0 || band < 0 || band >= kNumBands)
        return;

      switch (band)
      {
      case kBandLowShelf:
        ComputeLowShelf(mFrequency[band], mGainDb[band],
                        mB0[band], mB1[band], mB2[band], mA1[band], mA2[band]);
        break;
      case kBandLowMid:
      case kBandHighMid:
        ComputePeakingEQ(mFrequency[band], mQ[band], mGainDb[band],
                         mB0[band], mB1[band], mB2[band], mA1[band], mA2[band]);
        break;
      case kBandHighShelf:
        ComputeHighShelf(mFrequency[band], mGainDb[band],
                         mB0[band], mB1[band], mB2[band], mA1[band], mA2[band]);
        break;
      }
    }

    // Low shelf filter coefficients
    void ComputeLowShelf(double freq, double gainDb, double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double omega = 2.0 * std::numbers::pi * freq / mSampleRate;
      const double sinOmega = std::sin(omega);
      const double cosOmega = std::cos(omega);
      const double alpha = sinOmega / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 0.707 - 1.0) + 2.0);
      const double sqrtA = std::sqrt(A);

      const double a0 = (A + 1.0) + (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha;
      b0 = (A * ((A + 1.0) - (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha)) / a0;
      b1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cosOmega)) / a0;
      b2 = (A * ((A + 1.0) - (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha)) / a0;
      a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cosOmega)) / a0;
      a2 = ((A + 1.0) + (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha) / a0;
    }

    // High shelf filter coefficients
    void ComputeHighShelf(double freq, double gainDb, double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double omega = 2.0 * std::numbers::pi * freq / mSampleRate;
      const double sinOmega = std::sin(omega);
      const double cosOmega = std::cos(omega);
      const double alpha = sinOmega / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 0.707 - 1.0) + 2.0);
      const double sqrtA = std::sqrt(A);

      const double a0 = (A + 1.0) - (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha;
      b0 = (A * ((A + 1.0) + (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha)) / a0;
      b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cosOmega)) / a0;
      b2 = (A * ((A + 1.0) + (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha)) / a0;
      a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cosOmega)) / a0;
      a2 = ((A + 1.0) - (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha) / a0;
    }

    // Peaking EQ filter coefficients
    void ComputePeakingEQ(double freq, double q, double gainDb, double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double omega = 2.0 * std::numbers::pi * freq / mSampleRate;
      const double sinOmega = std::sin(omega);
      const double cosOmega = std::cos(omega);
      const double alpha = sinOmega / (2.0 * q);

      const double a0 = 1.0 + alpha / A;
      b0 = (1.0 + alpha * A) / a0;
      b1 = (-2.0 * cosOmega) / a0;
      b2 = (1.0 - alpha * A) / a0;
      a1 = (-2.0 * cosOmega) / a0;
      a2 = (1.0 - alpha / A) / a0;
    }

    double mSampleRate = 48000.0;
    bool mEnabled = true;

    // Per-band parameters
    std::array<bool, kNumBands> mBandEnabled = {true, true, true, true};
    std::array<double, kNumBands> mGainDb = {0.0, 0.0, 0.0, 0.0};
    std::array<double, kNumBands> mFrequency = {100.0, 500.0, 2000.0, 8000.0}; // Default frequencies
    std::array<double, kNumBands> mQ = {0.707, 1.0, 1.0, 0.707};               // Q for peaking bands

    // Biquad coefficients per band
    std::array<double, kNumBands> mB0 = {1.0, 1.0, 1.0, 1.0};
    std::array<double, kNumBands> mB1 = {0.0, 0.0, 0.0, 0.0};
    std::array<double, kNumBands> mB2 = {0.0, 0.0, 0.0, 0.0};
    std::array<double, kNumBands> mA1 = {0.0, 0.0, 0.0, 0.0};
    std::array<double, kNumBands> mA2 = {0.0, 0.0, 0.0, 0.0};

    // Filter states per band (Transposed Direct Form II)
    std::array<double, kNumBands> mState1 = {0.0, 0.0, 0.0, 0.0};
    std::array<double, kNumBands> mState2 = {0.0, 0.0, 0.0, 0.0};
  };

} // namespace guitarfx
