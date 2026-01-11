#pragma once

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>

namespace guitarfx
{
  /**
   * Simple algorithmic reverb using a network of allpass and comb filters.
   * Based on the Schroeder/Moorer reverb design.
   */
  class ReverbEffect
  {
  public:
    ReverbEffect() = default;

    void Prepare(double sampleRate, int maxBlockSize)
    {
      mSampleRate = sampleRate;

      // Comb filter delay times (in samples at 48kHz, scaled for actual sample rate)
      const double scale = sampleRate / 48000.0;

      // Initialize comb filters with prime-ish delay times for rich sound
      mCombDelays = {
          static_cast<size_t>(1557 * scale),
          static_cast<size_t>(1617 * scale),
          static_cast<size_t>(1491 * scale),
          static_cast<size_t>(1422 * scale),
          static_cast<size_t>(1277 * scale),
          static_cast<size_t>(1356 * scale),
          static_cast<size_t>(1188 * scale),
          static_cast<size_t>(1116 * scale)};

      // Allpass filter delay times
      mAllpassDelays = {
          static_cast<size_t>(225 * scale),
          static_cast<size_t>(556 * scale),
          static_cast<size_t>(441 * scale),
          static_cast<size_t>(341 * scale)};

      // Allocate buffers
      for (size_t i = 0; i < kNumCombs; ++i)
      {
        mCombBuffersL[i].resize(mCombDelays[i], 0.0);
        mCombBuffersR[i].resize(mCombDelays[i], 0.0);
        mCombIndices[i] = 0;
      }

      for (size_t i = 0; i < kNumAllpasses; ++i)
      {
        mAllpassBuffersL[i].resize(mAllpassDelays[i], 0.0);
        mAllpassBuffersR[i].resize(mAllpassDelays[i], 0.0);
        mAllpassIndices[i] = 0;
      }

      UpdateDecayFactor();
    }

    void Reset()
    {
      for (size_t i = 0; i < kNumCombs; ++i)
      {
        std::fill(mCombBuffersL[i].begin(), mCombBuffersL[i].end(), 0.0);
        std::fill(mCombBuffersR[i].begin(), mCombBuffersR[i].end(), 0.0);
        mCombIndices[i] = 0;
      }

      for (size_t i = 0; i < kNumAllpasses; ++i)
      {
        std::fill(mAllpassBuffersL[i].begin(), mAllpassBuffersL[i].end(), 0.0);
        std::fill(mAllpassBuffersR[i].begin(), mAllpassBuffersR[i].end(), 0.0);
        mAllpassIndices[i] = 0;
      }
    }

    void SetEnabled(bool enabled) { mEnabled = enabled; }
    [[nodiscard]] bool IsEnabled() const noexcept { return mEnabled; }

    void SetDecay(double decay)
    {
      mDecay = std::clamp(decay, 0.0, 1.0);
      UpdateDecayFactor();
    }

    void SetDamping(double damping)
    {
      mDamping = std::clamp(damping, 0.0, 1.0);
    }

    void SetMix(double mix)
    {
      mMix = std::clamp(mix, 0.0, 1.0);
    }

    void Process(double &left, double &right)
    {
      if (!mEnabled)
      {
        return;
      }

      const double inputL = left;
      const double inputR = right;

      // Process through parallel comb filters
      double combOutL = 0.0;
      double combOutR = 0.0;

      for (size_t i = 0; i < kNumCombs; ++i)
      {
        combOutL += ProcessComb(inputL, mCombBuffersL[i], mCombIndices[i], mCombDelays[i], mLowpassL[i]);
        combOutR += ProcessComb(inputR, mCombBuffersR[i], mCombIndices[i], mCombDelays[i], mLowpassR[i]);
      }

      combOutL /= static_cast<double>(kNumCombs);
      combOutR /= static_cast<double>(kNumCombs);

      // Process through series allpass filters
      double allpassOutL = combOutL;
      double allpassOutR = combOutR;

      for (size_t i = 0; i < kNumAllpasses; ++i)
      {
        allpassOutL = ProcessAllpass(allpassOutL, mAllpassBuffersL[i], mAllpassIndices[i], mAllpassDelays[i]);
        allpassOutR = ProcessAllpass(allpassOutR, mAllpassBuffersR[i], mAllpassIndices[i], mAllpassDelays[i]);
      }

      // Mix dry and wet signals
      left = inputL * (1.0 - mMix) + allpassOutL * mMix;
      right = inputR * (1.0 - mMix) + allpassOutR * mMix;
    }

  private:
    static constexpr size_t kNumCombs = 8;
    static constexpr size_t kNumAllpasses = 4;
    static constexpr double kAllpassCoeff = 0.5;

    void UpdateDecayFactor()
    {
      // Convert decay (0-1) to feedback coefficient
      // Higher decay = longer reverb tail
      mDecayFactor = 0.7 + mDecay * 0.28; // Range: 0.7 to 0.98
    }

    double ProcessComb(double input, std::vector<double> &buffer, size_t &index, size_t delay, double &lowpass)
    {
      if (buffer.empty())
        return input;

      const double delayed = buffer[index];

      // Apply damping (lowpass filter on feedback)
      lowpass = delayed * (1.0 - mDamping) + lowpass * mDamping;

      // Write new sample with feedback
      buffer[index] = input + lowpass * mDecayFactor;

      // Advance index
      index = (index + 1) % delay;

      return delayed;
    }

    double ProcessAllpass(double input, std::vector<double> &buffer, size_t &index, size_t delay)
    {
      if (buffer.empty())
        return input;

      const double delayed = buffer[index];
      const double output = -input + delayed;

      buffer[index] = input + delayed * kAllpassCoeff;
      index = (index + 1) % delay;

      return output;
    }

    bool mEnabled = false;
    double mSampleRate = 48000.0;
    double mDecay = 0.5;   // Reverb decay time (0-1)
    double mDamping = 0.5; // High frequency damping (0-1)
    double mMix = 0.3;     // Wet/dry mix
    double mDecayFactor = 0.84;

    // Comb filter state
    std::array<std::vector<double>, kNumCombs> mCombBuffersL;
    std::array<std::vector<double>, kNumCombs> mCombBuffersR;
    std::array<size_t, kNumCombs> mCombDelays{};
    std::array<size_t, kNumCombs> mCombIndices{};
    std::array<double, kNumCombs> mLowpassL{};
    std::array<double, kNumCombs> mLowpassR{};

    // Allpass filter state
    std::array<std::vector<double>, kNumAllpasses> mAllpassBuffersL;
    std::array<std::vector<double>, kNumAllpasses> mAllpassBuffersR;
    std::array<size_t, kNumAllpasses> mAllpassDelays{};
    std::array<size_t, kNumAllpasses> mAllpassIndices{};
  };

} // namespace guitarfx
