#pragma once

/**
 * GrainPitchShifter.h — the simplest pitch shifter there is, for use inside a feedback loop.
 *
 * Two taps sweep a short delay line at the speed the shift asks for, each faded in and out
 * with a sin^2 window half a sweep apart, so the two windows always sum to one. It has no
 * latency to speak of and costs two interpolated reads a sample, and on a sustained wash it is
 * clean; on a dry guitar its grains flutter audibly. That is the trade the shimmer reverb
 * wants: the shifter sits in the tank's feedback, where everything it hears is already smeared,
 * and where a shifter with a long analysis window would add its window to every pass.
 */

#include "dsp/effects/DelayLineSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace guitarfx
{
class GrainPitchShifter
{
  public:
    /// Allocates. Call from Prepare, never from Process.
    void Prepare(double sampleRate, double windowMs)
    {
        mWindow = std::max(16.0, windowMs * 0.001 * sampleRate);
        mLine.Resize(static_cast<std::size_t>(std::ceil(mWindow)) + 8);
        SetSemitones(mSemitones);
        Reset();
    }

    void Reset() noexcept
    {
        mLine.Clear();
        mPhase = 0.0;
    }

    /// Any shift, up or down; zero passes the signal through, a window late.
    void SetSemitones(double semitones) noexcept
    {
        mSemitones = semitones;
        const double ratio = std::pow(2.0, semitones / 12.0);
        mRising = ratio > 1.0;
        mIncrement = std::fabs(1.0 - ratio) / std::max(1.0, mWindow);
    }

    [[nodiscard]] float Process(float input) noexcept
    {
        const double other = (mPhase >= 0.5) ? mPhase - 0.5 : mPhase + 0.5;
        const float window = delay_line::FastSin01(static_cast<float>(0.5 * mPhase));
        const float firstGain = window * window;
        const float output =
            mLine.ReadHermite(TapDelay(mPhase)) * firstGain + mLine.ReadHermite(TapDelay(other)) * (1.0f - firstGain);
        mLine.Write(input);

        mPhase += mIncrement;

        if (mPhase >= 1.0)
        {
            mPhase -= 1.0;
        }

        return output;
    }

  private:
    /// Shifting up, a tap closes on the write head, so it plays faster than real time;
    /// shifting down, it falls back from it.
    [[nodiscard]] double TapDelay(double phase) const noexcept
    {
        return delay_line::kMinHermiteDelay + mWindow * (mRising ? 1.0 - phase : phase);
    }

    delay_line::FractionalDelayLine mLine;
    double mWindow = 1.0;
    double mSemitones = 12.0;
    double mIncrement = 0.0;
    double mPhase = 0.0;
    bool mRising = true;
};

/**
 * A shimmer reverb's return path, both channels: the late reverb shifted, band-limited so its
 * octave-ups cannot pile up as fizz and its octave-downs as rumble, and soft-limited so the
 * loop settles at a level instead of running away.
 */
class ShimmerReturn
{
  public:
    static constexpr double kWindowMs = 70.0;
    static constexpr double kHighCutHz = 7000.0;
    static constexpr double kLowCutHz = 120.0;
    static constexpr float kCeiling = 0.5f;

    /// Allocates. Call from Prepare, never from Process.
    void Prepare(double sampleRate)
    {
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            mShifters[channel].Prepare(sampleRate, kWindowMs);
            mLowPass[channel].SetCutoff(kHighCutHz, sampleRate);
            mHighPass[channel].SetCutoff(kLowCutHz, sampleRate);
        }

        Reset();
    }

    void Reset() noexcept
    {
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            mShifters[channel].Reset();
            mLowPass[channel].Reset();
            mHighPass[channel].Reset();
            mLast[channel] = 0.0f;
        }
    }

    void SetSemitones(double semitones) noexcept
    {
        for (auto& shifter : mShifters)
        {
            shifter.SetSemitones(semitones);
        }
    }

    /// Takes one sample of the late reverb; what it returns goes back in on the next sample.
    void Process(float lateL, float lateR) noexcept
    {
        mLast[0] = Shape(0, lateL);
        mLast[1] = Shape(1, lateR);
    }

    [[nodiscard]] float Left() const noexcept
    {
        return mLast[0];
    }

    [[nodiscard]] float Right() const noexcept
    {
        return mLast[1];
    }

    [[nodiscard]] bool Silent() const noexcept
    {
        return mLast[0] == 0.0f && mLast[1] == 0.0f;
    }

  private:
    [[nodiscard]] float Shape(std::size_t channel, float late) noexcept
    {
        const float shifted = mShifters[channel].Process(late);
        const float banded = mHighPass[channel].Process(mLowPass[channel].Process(shifted));
        return delay_line::RailClip(banded, 0.5f * kCeiling, kCeiling);
    }

    std::array<GrainPitchShifter, 2> mShifters;
    std::array<delay_line::OnePoleLp, 2> mLowPass{};
    std::array<delay_line::OnePoleHp, 2> mHighPass{};
    std::array<float, 2> mLast{};
};
} // namespace guitarfx
