#pragma once

#include <cmath>
#include <numbers>

namespace guitarfx
{
  // Simple, high-performance cabinet simulation using cascaded biquad filters.
  // Emulates the frequency response of a typical guitar speaker cabinet:
  // - High-pass: Removes sub-bass below speaker capability (~80 Hz)
  // - Low-pass: Rolls off harsh high frequencies (~5 kHz)
  // - Presence peak: Adds speaker cone resonance (~2.5 kHz)
  //
  // This provides a lightweight alternative to IR convolution when CPU is limited
  // or when a generic cabinet sound is acceptable.
  class SimpleCabSim
  {
  public:
    SimpleCabSim() = default;

    void Prepare(double sampleRate)
    {
      mSampleRate = sampleRate;
      UpdateCoefficients();
      Reset();
    }

    void Reset()
    {
      // Reset all filter states
      mHPState1 = mHPState2 = 0.0;
      mLPState1 = mLPState2 = 0.0;
      mPeakState1 = mPeakState2 = 0.0;
    }

    // Process a single sample through the cabinet simulation
    double Process(double sample)
    {
      // High-pass filter (removes sub-bass)
      double hp = ProcessBiquad(sample, mHPB0, mHPB1, mHPB2, mHPA1, mHPA2, mHPState1, mHPState2);

      // Low-pass filter (rolls off highs)
      double lp = ProcessBiquad(hp, mLPB0, mLPB1, mLPB2, mLPA1, mLPA2, mLPState1, mLPState2);

      // Presence peak (speaker resonance)
      double out = ProcessBiquad(lp, mPeakB0, mPeakB1, mPeakB2, mPeakA1, mPeakA2, mPeakState1, mPeakState2);

      return out;
    }

    // Adjustable parameters (0.0 to 1.0 normalized)
    void SetBass(double bass)
    {
      mBassAmount = std::clamp(bass, 0.0, 1.0);
      UpdateCoefficients();
    }

    void SetPresence(double presence)
    {
      mPresenceAmount = std::clamp(presence, 0.0, 1.0);
      UpdateCoefficients();
    }

    void SetBrightness(double brightness)
    {
      mBrightness = std::clamp(brightness, 0.0, 1.0);
      UpdateCoefficients();
    }

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

    void UpdateCoefficients()
    {
      if (mSampleRate <= 0.0)
        return;

      // High-pass filter: 60-120 Hz depending on bass setting
      // Lower bass = higher cutoff (thinner sound)
      const double hpFreq = 60.0 + (1.0 - mBassAmount) * 80.0;
      ComputeHighPass(hpFreq, 0.707, mHPB0, mHPB1, mHPB2, mHPA1, mHPA2);

      // Low-pass filter: 4-8 kHz depending on brightness
      const double lpFreq = 4000.0 + mBrightness * 4000.0;
      ComputeLowPass(lpFreq, 0.707, mLPB0, mLPB1, mLPB2, mLPA1, mLPA2);

      // Presence peak: 2-3.5 kHz with adjustable gain
      const double peakFreq = 2000.0 + mPresenceAmount * 1500.0;
      const double peakGain = 2.0 + mPresenceAmount * 4.0; // 2-6 dB boost
      ComputePeakingEQ(peakFreq, 1.5, peakGain, mPeakB0, mPeakB1, mPeakB2, mPeakA1, mPeakA2);
    }

    // Compute 2nd order Butterworth high-pass filter coefficients
    void ComputeHighPass(double freq, double q, double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double omega = 2.0 * std::numbers::pi * freq / mSampleRate;
      const double sinOmega = std::sin(omega);
      const double cosOmega = std::cos(omega);
      const double alpha = sinOmega / (2.0 * q);

      const double a0 = 1.0 + alpha;
      b0 = ((1.0 + cosOmega) / 2.0) / a0;
      b1 = (-(1.0 + cosOmega)) / a0;
      b2 = ((1.0 + cosOmega) / 2.0) / a0;
      a1 = (-2.0 * cosOmega) / a0;
      a2 = (1.0 - alpha) / a0;
    }

    // Compute 2nd order Butterworth low-pass filter coefficients
    void ComputeLowPass(double freq, double q, double &b0, double &b1, double &b2, double &a1, double &a2)
    {
      const double omega = 2.0 * std::numbers::pi * freq / mSampleRate;
      const double sinOmega = std::sin(omega);
      const double cosOmega = std::cos(omega);
      const double alpha = sinOmega / (2.0 * q);

      const double a0 = 1.0 + alpha;
      b0 = ((1.0 - cosOmega) / 2.0) / a0;
      b1 = (1.0 - cosOmega) / a0;
      b2 = ((1.0 - cosOmega) / 2.0) / a0;
      a1 = (-2.0 * cosOmega) / a0;
      a2 = (1.0 - alpha) / a0;
    }

    // Compute peaking EQ filter coefficients (for presence boost)
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

    // User parameters (0-1 normalized)
    double mBassAmount = 0.5;     // Controls HP cutoff (more bass = lower cutoff)
    double mPresenceAmount = 0.5; // Controls presence peak frequency and gain
    double mBrightness = 0.5;     // Controls LP cutoff (brighter = higher cutoff)

    // High-pass filter coefficients and state
    double mHPB0 = 1.0, mHPB1 = 0.0, mHPB2 = 0.0;
    double mHPA1 = 0.0, mHPA2 = 0.0;
    double mHPState1 = 0.0, mHPState2 = 0.0;

    // Low-pass filter coefficients and state
    double mLPB0 = 1.0, mLPB1 = 0.0, mLPB2 = 0.0;
    double mLPA1 = 0.0, mLPA2 = 0.0;
    double mLPState1 = 0.0, mLPState2 = 0.0;

    // Presence peak filter coefficients and state
    double mPeakB0 = 1.0, mPeakB1 = 0.0, mPeakB2 = 0.0;
    double mPeakA1 = 0.0, mPeakA2 = 0.0;
    double mPeakState1 = 0.0, mPeakState2 = 0.0;
  };

} // namespace guitarfx
