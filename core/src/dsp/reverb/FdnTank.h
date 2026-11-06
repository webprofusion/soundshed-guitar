#pragma once

/**
 * FdnTank.h — the late reverb Verbal's machines share: sixteen modulated delay lines mixed by an
 * orthonormal Hadamard matrix.
 *
 * Each line's loss is solved from the decay time asked for, per band: a line M samples long
 * loses 60·M/(fs·T60) dB a pass, so every line decays at the same rate whatever its length and
 * the measured RT60 lands on the knob. Low End and Tone set the low and high bands' T60 as
 * ratios of the mid band's. Hold takes every band's loss to zero, so what is in the tank rings on,
 * under a soft ceiling that keeps what is played into it bounded.
 *
 * The two outputs are two different sign patterns over the lines: orthogonal sums, so the left
 * and right tails are uncorrelated even from a mono input.
 */

#include "dsp/reverb/ReverbBlocks.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace guitarfx::reverb
{
class FdnTank
{
  public:
    static constexpr std::size_t kLines = 16;

    /// Allocates for lines up to `maxLineMs` plus modulation. Call from Prepare.
    void Prepare(double sampleRate, double maxLineMs)
    {
        mSampleRate = sampleRate;
        const double maxSamples = (maxLineMs + kMaxModMs + 2.0) * 0.001 * sampleRate;

        for (auto& line : mLines)
        {
            line.Resize(static_cast<std::size_t>(std::ceil(maxSamples)) + 8);
        }

        mMaxLength = mLines[0].MaxDelay() - kMaxModMs * 0.001 * sampleRate - 2.0;
        mGlideCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.12 * sampleRate)));
        mGainCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.02 * sampleRate)));
        mModCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.05 * sampleRate)));

        for (std::size_t i = 0; i < kLines; ++i)
        {
            const double offset = kTwoPi * static_cast<double>(i) * 0.61803398875;
            mOffsetSin[i] = static_cast<float>(std::sin(offset));
            mOffsetCos[i] = static_cast<float>(std::cos(offset));
            const double offset2 = kTwoPi * static_cast<double>(i) * 0.2763932;
            mOffset2Sin[i] = static_cast<float>(std::sin(offset2));
            mOffset2Cos[i] = static_cast<float>(std::cos(offset2));
        }

        SetCrossovers(250.0, 3500.0);
        SetModulation(0.0, 0.5);
        Reset();
    }

    void Reset() noexcept
    {
        for (auto& line : mLines)
        {
            line.Clear();
        }

        for (auto& d : mDamping)
        {
            d.Reset();
        }

        mLfo.Reset();
        mLfo2.Reset();
        SnapControls();
        mEnergy = 0.0f;
    }

    /// Lands every glide at once, for a fresh machine.
    void SnapControls() noexcept
    {
        mLength = mLengthTarget;
        mGains = mGainsTarget;
        mModDepth = mModTarget;
    }

    /// Target line lengths in samples; a change glides over about 120 ms, so Size moves the
    /// tank's pitch for a moment rather than flushing it.
    void SetLengths(const std::array<double, kLines>& samples, bool snap) noexcept
    {
        for (std::size_t i = 0; i < kLines; ++i)
        {
            mLengthTarget[i] = static_cast<float>(std::clamp(samples[i], 8.0, mMaxLength));
        }

        if (snap)
        {
            mLength = mLengthTarget;
        }

        UpdateGains();
    }

    void SetCrossovers(double lowHz, double highHz) noexcept
    {
        for (auto& d : mDamping)
        {
            d.SetCrossovers(lowHz, highHz, mSampleRate);
        }
    }

    /// Mid-band decay in seconds, and the low and high bands' as multiples of it.
    void SetDecay(double rt60, double lowRatio, double highRatio) noexcept
    {
        mRt60 = std::max(0.05, rt60);
        mLowRatio = std::max(0.05, lowRatio);
        mHighRatio = std::max(0.02, highRatio);
        UpdateGains();
    }

    /// Delay modulation: peak swing in samples and the base rate. The lines use two
    /// incommensurate LFOs at per-line phases, so no two lines move together. A new depth eases
    /// in over about 50 ms: stepped, every line would jump in the same sample.
    void SetModulation(double depthSamples, double rateHz) noexcept
    {
        mModTarget = static_cast<float>(std::clamp(depthSamples, 0.0, kMaxModMs * 0.001 * mSampleRate));
        mLfo.SetRate(rateHz, mSampleRate);
        mLfo2.SetRate(rateHz * 0.37, mSampleRate);
    }

    /// The most a line may hold while held. Decaying, it may hold twice that: a very long decay
    /// also builds on sustained playing, but less fast, and should not be touched at normal level.
    void SetCeiling(float rail) noexcept
    {
        mRail = rail;
    }

    /// 0 = decaying, 1 = held: every band's loop gain pulled to exactly one.
    void SetHold(float hold) noexcept
    {
        mHold = std::clamp(hold, 0.0f, 1.0f);
    }

    /// One sample in, one stereo sample out. `inL` feeds the even lines and `inR` the odd ones,
    /// each with its own sign.
    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        mLfo.Advance();
        mLfo2.Advance();
        mModDepth += (mModTarget - mModDepth) * mModCoeff;
        std::array<float, kLines> y{};

        for (std::size_t i = 0; i < kLines; ++i)
        {
            mLength[i] += (mLengthTarget[i] - mLength[i]) * mGlideCoeff;
            const float mod = mModDepth * (0.7f * mLfo.SinOffset(mOffsetSin[i], mOffsetCos[i]) +
                                           0.3f * mLfo2.SinOffset(mOffset2Sin[i], mOffset2Cos[i]));
            const double delay = static_cast<double>(mLength[i] + mod);
            BandDamping::Gains& g = mGains[i];
            const BandDamping::Gains& t = mGainsTarget[i];
            g.low += (t.low - g.low) * mGainCoeff;
            g.mid += (t.mid - g.mid) * mGainCoeff;
            g.high += (t.high - g.high) * mGainCoeff;
            const BandDamping::Gains held = {g.low + (1.0f - g.low) * mHold, g.mid + (1.0f - g.mid) * mHold,
                                             g.high + (1.0f - g.high) * mHold};
            y[i] = mDamping[i].Process(mLines[i].ReadHermite(delay), held);
        }

        float l = 0.0f;
        float r = 0.0f;

        for (std::size_t i = 0; i < kLines; ++i)
        {
            l += kOutSignL[i] * y[i];
            r += kOutSignR[i] * y[i];
        }

        outL = 0.25f * l;
        outR = 0.25f * r;
        // A sum, not a max: a NaN must reach StateFinite, and std::max can drop one.
        mEnergy = std::fabs(outL) + std::fabs(outR);

        Hadamard16(y);
        // Held, nothing leaves the tank, so playing into Infinite would build without limit. A
        // soft ceiling bounds it, set by the owner so it means the same level at the output.
        const float rail = mRail * (2.0f - mHold);

        for (std::size_t i = 0; i < kLines; ++i)
        {
            const float in = (i & 1u) ? inR : inL;
            const float w = delay_line::RailClip(y[i] + kInSign[i] * in * kInjectGain, 0.64f * rail, rail);
            mLines[i].Write(delay_line::FlushDenormal(w));
        }
    }

    /// The last output's peak, for telling when a spilled-over tail has died away.
    [[nodiscard]] float Energy() const noexcept
    {
        return mEnergy;
    }

    /// The mean line length in seconds, which sets how fast echo density builds.
    [[nodiscard]] double MeanLengthSeconds() const noexcept
    {
        double sum = 0.0;

        for (float length : mLengthTarget)
        {
            sum += length;
        }

        return sum / (static_cast<double>(kLines) * mSampleRate);
    }

    [[nodiscard]] bool StateFinite() const noexcept
    {
        return IsFinite(mEnergy);
    }

  private:
    static constexpr double kMaxModMs = 6.0;
    /// Eight lines take each input, so this keeps the input's energy at about unity.
    static constexpr float kInjectGain = 0.35355339f;
    static constexpr std::array<float, kLines> kInSign = {1, 1, -1, 1, 1, -1, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1};
    // Rows 5 and 10 of a 16-point Hadamard: orthogonal, so the outputs are uncorrelated.
    static constexpr std::array<float, kLines> kOutSignL = {1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1, 1, -1, 1};
    static constexpr std::array<float, kLines> kOutSignR = {1, 1, -1, -1, 1, 1, -1, -1, -1, -1, 1, 1, -1, -1, 1, 1};

    void UpdateGains() noexcept
    {
        for (std::size_t i = 0; i < kLines; ++i)
        {
            const double seconds = static_cast<double>(mLengthTarget[i]) / mSampleRate;
            mGainsTarget[i].mid = static_cast<float>(Rt60LoopGain(seconds, mRt60));
            mGainsTarget[i].low = static_cast<float>(Rt60LoopGain(seconds, mRt60 * mLowRatio));
            mGainsTarget[i].high = static_cast<float>(Rt60LoopGain(seconds, mRt60 * mHighRatio));
        }
    }

    double mSampleRate = 48000.0;
    double mMaxLength = 1000.0;
    std::array<delay_line::FractionalDelayLine, kLines> mLines;
    std::array<BandDamping, kLines> mDamping;
    std::array<float, kLines> mLength{};
    std::array<float, kLines> mLengthTarget{};
    std::array<BandDamping::Gains, kLines> mGains{};
    std::array<BandDamping::Gains, kLines> mGainsTarget{};
    std::array<float, kLines> mOffsetSin{};
    std::array<float, kLines> mOffsetCos{};
    std::array<float, kLines> mOffset2Sin{};
    std::array<float, kLines> mOffset2Cos{};
    RotatingPhasor mLfo;
    RotatingPhasor mLfo2;
    double mRt60 = 2.0;
    double mLowRatio = 1.0;
    double mHighRatio = 0.5;
    float mModDepth = 0.0f;
    float mModTarget = 0.0f;
    float mGlideCoeff = 1.0f;
    float mGainCoeff = 1.0f;
    float mModCoeff = 1.0f;
    float mHold = 0.0f;
    float mRail = 1.4f;
    float mEnergy = 0.0f;
};
} // namespace guitarfx::reverb
