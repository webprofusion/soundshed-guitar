#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

namespace guitarfx
{
  /**
   * Simple stereo delay effect with feedback and mix control.
   * Supports tempo-synced or free-running delay times.
   */
  class DelayEffect
  {
  public:
    DelayEffect() = default;

    void Prepare(double sampleRate, int maxBlockSize)
    {
      mSampleRate = sampleRate;
      // Max delay of 2 seconds
      const size_t maxDelaySamples = static_cast<size_t>(sampleRate * 2.0);
      mDelayBufferL.resize(maxDelaySamples, 0.0);
      mDelayBufferR.resize(maxDelaySamples, 0.0);
      mWriteIndex = 0;
      mMaxDelaySamples = maxDelaySamples;
      UpdateDelaySamples();
    }

    void Reset()
    {
      std::fill(mDelayBufferL.begin(), mDelayBufferL.end(), 0.0);
      std::fill(mDelayBufferR.begin(), mDelayBufferR.end(), 0.0);
      mWriteIndex = 0;
    }

    void SetEnabled(bool enabled) { mEnabled = enabled; }
    [[nodiscard]] bool IsEnabled() const noexcept { return mEnabled; }

    void SetDelayTime(double milliseconds)
    {
      mDelayTimeMs = std::clamp(milliseconds, 1.0, 2000.0);
      UpdateDelaySamples();
    }

    void SetFeedback(double feedback)
    {
      mFeedback = std::clamp(feedback, 0.0, 0.95); // Limit to prevent runaway
    }

    void SetMix(double mix)
    {
      mMix = std::clamp(mix, 0.0, 1.0);
    }

    void Process(double &left, double &right)
    {
      if (!mEnabled || mDelayBufferL.empty())
      {
        return;
      }

      // Calculate read position
      const size_t readIndex = (mWriteIndex + mMaxDelaySamples - mDelaySamples) % mMaxDelaySamples;

      // Read delayed samples
      const double delayedL = mDelayBufferL[readIndex];
      const double delayedR = mDelayBufferR[readIndex];

      // Write to delay buffer with feedback
      mDelayBufferL[mWriteIndex] = left + delayedL * mFeedback;
      mDelayBufferR[mWriteIndex] = right + delayedR * mFeedback;

      // Advance write position
      mWriteIndex = (mWriteIndex + 1) % mMaxDelaySamples;

      // Mix dry and wet signals
      left = left * (1.0 - mMix) + delayedL * mMix;
      right = right * (1.0 - mMix) + delayedR * mMix;
    }

  private:
    void UpdateDelaySamples()
    {
      if (mSampleRate > 0)
      {
        mDelaySamples = static_cast<size_t>(mDelayTimeMs * mSampleRate / 1000.0);
        mDelaySamples = std::min(mDelaySamples, mMaxDelaySamples - 1);
      }
    }

    bool mEnabled = false;
    double mSampleRate = 48000.0;
    double mDelayTimeMs = 300.0; // Default 300ms
    double mFeedback = 0.4;      // Default 40% feedback
    double mMix = 0.3;           // Default 30% wet

    std::vector<double> mDelayBufferL;
    std::vector<double> mDelayBufferR;
    size_t mWriteIndex = 0;
    size_t mDelaySamples = 0;
    size_t mMaxDelaySamples = 0;
  };

} // namespace guitarfx
