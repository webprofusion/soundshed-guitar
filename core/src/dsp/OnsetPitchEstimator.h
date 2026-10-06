#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// The difference loop stays a function of its own, as in PitchTracker.h: inlined into the lag
// loop, MSVC stops vectorising it.
#if defined(_MSC_VER)
    #define GUITARFX_ONSET_PITCH_NOINLINE __declspec(noinline)
#else
    #define GUITARFX_ONSET_PITCH_NOINLINE __attribute__((noinline))
#endif

namespace guitarfx
{
/**
 * The pitch of a newly picked note, from the audio since its pick alone.
 *
 * PitchTracker (dsp/PitchTracker.h) judges a fixed window as long as twice the longest period it
 * looks for, about 28 ms for a drop-D guitar. After a pick that window still holds the note before
 * it, so on real playing it finds the new note 30-60 ms late, or not at all for a palm-muted note
 * whose window straddles the mute. This starts again at each pick (Begin()) and runs YIN over what
 * has arrived since, skipping the pick's first kSkipSeconds of noise. Half of it is the window and
 * the lag search covers the rest, so a note's period is found as soon as two of them have
 * arrived: about 12 ms for an E3, 25 ms for a low E.
 *
 * An estimate is Complete() once the search has reached twice its period, or the lowest
 * frequency's period, whichever is shorter: a lower fundamental would have shown there. Before
 * that a strong harmonic can pass for the note. Right after a low note's pick the fifth harmonic
 * often does, reading two octaves and more too high with full confidence, so a caller should only
 * believe an early estimate that is near the note it expects.
 *
 * The input is low-passed at 2 kHz and decimated to about 12 kHz, as PitchTracker's is. Prepare()
 * allocates; Begin(), Push() and Stop() do not. An estimate runs every kHopSeconds while active.
 */
class OnsetPitchEstimator
{
  public:
    static constexpr double kMaxHz = 2000.0;
    static constexpr double kSkipSeconds = 0.003;
    static constexpr double kHopSeconds = 0.0025;
    /// How long after a pick estimates are made; Begin() again at the next.
    static constexpr double kActiveSeconds = 0.06;

    void Prepare(double sampleRate, double lowestHz)
    {
        mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        mDecimation = std::max(1, static_cast<int>(std::lround(mSampleRate / kDecimatedRate)));
        mDecimatedRate = mSampleRate / mDecimation;
        mLowPass[0] = biquad::LowPass(kLowPassHz, biquad::ButterworthSectionQ(4, 0), mSampleRate);
        mLowPass[1] = biquad::LowPass(kLowPassHz, biquad::ButterworthSectionQ(4, 1), mSampleRate);
        mMinLag = std::max(2, static_cast<int>(std::floor(mDecimatedRate / kMaxHz)));
        mSkip = static_cast<int>(std::lround(kSkipSeconds * mDecimatedRate));
        mHop = std::max(1, static_cast<int>(std::lround(kHopSeconds * mDecimatedRate)));
        const int active = static_cast<int>(std::ceil(kActiveSeconds * mDecimatedRate));
        mBuffer.assign(static_cast<std::size_t>(active + 4), 0.0f);
        mNormalised.assign(static_cast<std::size_t>(active + 4), 1.0f);
        SetLowestFrequency(lowestHz);
        Stop();
        mFilter[0].Reset();
        mFilter[1].Reset();
    }

    /// The lowest pitch looked for; its period sets when an estimate is Complete().
    void SetLowestFrequency(double hz) noexcept
    {
        const double lowest = IsFinite(hz) && hz > 1.0 ? hz : 45.0;
        mMaxLag = std::max(mMinLag + 2, static_cast<int>(std::ceil(mDecimatedRate / lowest)) + 1);
    }

    /// Starts over on a pick: what came before it is forgotten.
    void Begin() noexcept
    {
        mActive = true;
        mCount = 0;
        mSinceHop = 0;
        mDecimationCount = 0;
        mHz = 0.0;
        mComplete = false;
    }

    /// Stops estimating until the next Begin(); the filter keeps running.
    void Stop() noexcept
    {
        mActive = false;
        mHz = 0.0;
        mComplete = false;
    }

    /// One input sample. True when an estimate was made on it (Hz() is 0 when it found none).
    bool Push(float sample) noexcept
    {
        const float x = IsFinite(sample) ? sample : 0.0f;
        const auto y = static_cast<float>(mFilter[1].Process(mLowPass[1], mFilter[0].Process(mLowPass[0], x)));

        if (!IsFinite(mFilter[0].s1) || !IsFinite(mFilter[1].s1))
        {
            mFilter[0].Reset();
            mFilter[1].Reset();
        }

        if (!mActive || ++mDecimationCount < mDecimation)
        {
            return false;
        }

        mDecimationCount = 0;

        if (mCount >= static_cast<int>(mBuffer.size()))
        {
            Stop();
            return false;
        }

        mBuffer[static_cast<std::size_t>(mCount++)] = y;

        if (++mSinceHop < mHop)
        {
            return false;
        }

        mSinceHop = 0;
        Estimate();
        return true;
    }

    [[nodiscard]] bool IsActive() const noexcept
    {
        return mActive;
    }

    /// The latest estimate, in Hz; 0 when it found no pitch.
    [[nodiscard]] double Hz() const noexcept
    {
        return mHz;
    }

    /// Whether the latest estimate searched far enough that a lower fundamental would have shown.
    [[nodiscard]] bool Complete() const noexcept
    {
        return mComplete;
    }

    /// 1 minus the normalised difference at the chosen lag: near 1 for a clean tone.
    [[nodiscard]] double Confidence() const noexcept
    {
        return mConfidence;
    }

  private:
    static constexpr double kDecimatedRate = 12000.0;
    static constexpr double kLowPassHz = 2000.0;
    static constexpr float kDipThreshold = 0.2f;

    [[nodiscard]] GUITARFX_ONSET_PITCH_NOINLINE static float SquaredDifference(const float* a, const float* b,
                                                                               int count) noexcept
    {
        float sum = 0.0f;

        for (int j = 0; j < count; ++j)
        {
            const float delta = a[j] - b[j];
            sum += delta * delta;
        }

        return sum;
    }

    void Estimate() noexcept
    {
        mHz = 0.0;
        mComplete = false;
        const int available = mCount - mSkip;
        const int window = available / 2;
        const int maxLag = std::min(mMaxLag, available - window - 1);

        if (window < 2 * mMinLag || maxLag <= mMinLag + 1)
        {
            return;
        }

        // YIN, as PitchTracker runs it: the difference function, its cumulative-mean
        // normalisation, and the first dip under the threshold followed down to its minimum.
        const float* x = mBuffer.data() + mSkip;
        float* normalised = mNormalised.data();
        float running = 0.0f;
        int lag = 0;
        normalised[0] = 1.0f;

        for (int tau = 1; tau <= maxLag; ++tau)
        {
            const float difference = SquaredDifference(x, x + tau, window);
            running += difference;
            normalised[tau] = running > 0.0f ? difference * static_cast<float>(tau) / running : 1.0f;

            if (lag != 0)
            {
                if (normalised[tau] < normalised[lag])
                {
                    lag = tau;
                    continue;
                }

                break;
            }

            if (normalised[tau] < kDipThreshold && tau >= mMinLag)
            {
                lag = tau;
            }
        }

        if (lag == 0 || lag + 1 > maxLag)
        {
            return;
        }

        const double left = normalised[lag - 1];
        const double middle = normalised[lag];
        const double right = normalised[lag + 1];
        const double curvature = left - 2.0 * middle + right;
        const double offset =
            std::abs(curvature) > 1.0e-12 ? std::clamp(0.5 * (left - right) / curvature, -1.0, 1.0) : 0.0;
        const double hz = mDecimatedRate / (static_cast<double>(lag) + offset);

        if (!(hz > 0.0 && hz <= kMaxHz))
        {
            return;
        }

        mHz = hz;
        mConfidence = 1.0 - middle;
        mComplete = maxLag >= std::min(mMaxLag, 2 * lag + 2);
    }

    double mSampleRate = 48000.0;
    double mDecimatedRate = 12000.0;
    int mDecimation = 4;
    int mDecimationCount = 0;
    BiquadCoefficients mLowPass[2];
    biquad::State mFilter[2];
    std::vector<float> mBuffer;
    std::vector<float> mNormalised;
    int mMinLag = 6;
    int mMaxLag = 173;
    int mSkip = 36;
    int mHop = 30;
    int mCount = 0;
    int mSinceHop = 0;
    bool mActive = false;
    bool mComplete = false;
    double mHz = 0.0;
    double mConfidence = 0.0;
};
} // namespace guitarfx
