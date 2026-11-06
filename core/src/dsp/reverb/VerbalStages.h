#pragma once

/**
 * VerbalStages.h — the parts of Verbal's machines that sit around the shared tank: the input
 * diffuser, early-reflection taps and the room model behind Reflections, the Choir's formant
 * filters, the Swell's envelope and the Nonlinear machine's shaped tap generator.
 *
 * Everything allocates in Prepare and nothing after.
 */

#include "dsp/reverb/ReverbBlocks.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace guitarfx::reverb
{
/**
 * A chain of up to eight allpasses per side, its lengths set per machine. At gain 0 each stage
 * is a plain delay, which is what makes Diffusion 0 sound grainy: discrete arrivals, not a wash.
 * The lengths can be swept by a quadrature LFO, as the Cloud's are. A change of length, gain or
 * sweep eases in, so moving a control bends the sound for a moment rather than ticking.
 */
class DiffuserChain
{
  public:
    static constexpr std::size_t kMaxStages = 8;

    void Prepare(double sampleRate, double maxStageMs)
    {
        mSampleRate = sampleRate;
        mGlide = 1.0 - std::exp(-1.0 / (0.12 * sampleRate));
        mEase = static_cast<float>(1.0 - std::exp(-1.0 / (0.03 * sampleRate)));
        const double samples = (maxStageMs + 4.0) * 0.001 * sampleRate;

        for (std::size_t i = 0; i < kMaxStages; ++i)
        {
            mLeft[i].Prepare(samples);
            mRight[i].Prepare(samples);
        }
    }

    void Reset() noexcept
    {
        for (std::size_t i = 0; i < kMaxStages; ++i)
        {
            mLeft[i].Reset();
            mRight[i].Reset();
        }

        mLfo.Reset();
    }

    /// `stages` allpasses per side, lengths spread geometrically from `minMs` to `maxMs`.
    void Configure(std::size_t stages, double minMs, double maxMs) noexcept
    {
        mStages = std::min(stages, kMaxStages);
        const double ratio = mStages > 1 ? std::pow(maxMs / minMs, 1.0 / static_cast<double>(mStages - 1)) : 1.0;
        double ms = minMs;

        for (std::size_t i = 0; i < mStages; ++i)
        {
            // Right a little longer than left, and the two never in a simple ratio.
            mTargetL[i] = std::min(ms * 0.001 * mSampleRate, mLeft[i].MaxDelay() - 4.0);
            mTargetR[i] = std::min(ms * 1.0731 * 0.001 * mSampleRate, mRight[i].MaxDelay() - 4.0);
            mMoving = mMoving || mTargetL[i] != mDelayL[i] || mTargetR[i] != mDelayR[i];
            ms *= ratio;
        }
    }

    void SetGain(float gain) noexcept
    {
        mGainTarget = std::clamp(gain, 0.0f, 0.8f);
    }

    /// Lands every glide at once, for a fresh machine.
    void SnapControls() noexcept
    {
        mDelayL = mTargetL;
        mDelayR = mTargetR;
        mGain = mGainTarget;
        mModDepth = mModTarget;
        mMoving = false;
    }

    /// Peak swing in samples and rate, applied in quadrature to the two sides.
    void SetModulation(double depthSamples, double rateHz) noexcept
    {
        mModTarget = static_cast<float>(depthSamples);
        mLfo.SetRate(rateHz, mSampleRate);
    }

    void Process(float& l, float& r) noexcept
    {
        if (mMoving)
        {
            StepDelays();
        }

        mGain += (mGainTarget - mGain) * mEase;
        mModDepth += (mModTarget - mModDepth) * mEase;
        mLfo.Advance();
        const float modL = mModDepth * mLfo.Sin();
        const float modR = mModDepth * mLfo.Cos();

        for (std::size_t i = 0; i < mStages; ++i)
        {
            const float sign = (i & 1u) ? -1.0f : 1.0f;
            l = mLeft[i].Process(l, std::max(2.0, mDelayL[i] + static_cast<double>(sign * modL)), mGain);
            r = mRight[i].Process(r, std::max(2.0, mDelayR[i] + static_cast<double>(sign * modR)), mGain);
        }
    }

  private:
    /// Moves each length toward its target, no faster than half real time so a big change
    /// bends the pitch instead of reading the line backwards.
    void StepDelays() noexcept
    {
        bool moving = false;

        const auto step = [this, &moving](double& delay, double target) {
            const double move =
                std::clamp(mGlide * (target - delay), -delay_line::kMaxGlideStep, delay_line::kMaxGlideStep);
            delay = std::fabs(target - delay) < 0.01 ? target : delay + move;
            moving = moving || delay != target;
        };

        for (std::size_t i = 0; i < mStages; ++i)
        {
            step(mDelayL[i], mTargetL[i]);
            step(mDelayR[i], mTargetR[i]);
        }

        mMoving = moving;
    }

    double mSampleRate = 48000.0;
    std::array<FractionalAllpass, kMaxStages> mLeft;
    std::array<FractionalAllpass, kMaxStages> mRight;
    std::array<double, kMaxStages> mDelayL{};
    std::array<double, kMaxStages> mDelayR{};
    std::array<double, kMaxStages> mTargetL{};
    std::array<double, kMaxStages> mTargetR{};
    std::size_t mStages = 0;
    double mGlide = 0.001;
    float mEase = 0.001f;
    float mGain = 0.6f;
    float mGainTarget = 0.6f;
    float mModDepth = 0.0f;
    float mModTarget = 0.0f;
    bool mMoving = false;
    RotatingPhasor mLfo;
};

/// A set of taps read off a stereo line: an early-reflection pattern or a room's image sources.
struct TapSet
{
    static constexpr std::size_t kMaxTaps = 64;

    std::array<float, kMaxTaps> delay{};  ///< samples
    std::array<float, kMaxTaps> spread{}; ///< half the arrival-time difference between the ears, samples
    std::array<float, kMaxTaps> gainL{};
    std::array<float, kMaxTaps> gainR{};
    std::array<std::uint8_t, kMaxTaps> late{}; ///< 1 = a later-order tap, darkened by Damping
    std::size_t count = 0;

    /// Whether the two sets read the same taps. Only the first `count` of each array are live.
    [[nodiscard]] bool Same(const TapSet& other) const noexcept
    {
        if (count != other.count)
        {
            return false;
        }

        for (std::size_t i = 0; i < count; ++i)
        {
            if (delay[i] != other.delay[i] || spread[i] != other.spread[i] || gainL[i] != other.gainL[i] ||
                gainR[i] != other.gainR[i] || late[i] != other.late[i])
            {
                return false;
            }
        }

        return true;
    }
};

/**
 * An early-reflection pattern: `count` arrivals spread from `startMs` to `endMs`, denser and
 * quieter toward the end, each side its own times so the two never coincide. Seeded, so the
 * same machine always has the same room.
 */
inline void BuildEarlyPattern(TapSet& taps, std::size_t count, double startMs, double endMs, double decayDb,
                              std::uint32_t seed, double sampleRate) noexcept
{
    delay_line::Rng rng;
    rng.Seed(seed);
    taps.count = std::min(count, TapSet::kMaxTaps);
    float norm = 0.0f;

    for (std::size_t i = 0; i < taps.count; ++i)
    {
        // Arrivals bunch up with time, as reflection density grows with t².
        const double u = (static_cast<double>(i) + 0.5f + 0.4f * rng.NextBipolar()) / static_cast<double>(taps.count);
        const double ms = startMs + (endMs - startMs) * std::sqrt(std::clamp(u, 0.0, 1.0));
        const double level = std::pow(10.0, -decayDb * (ms - startMs) / std::max(1.0, endMs - startMs) / 20.0);
        const float pan = 0.5f + 0.45f * rng.NextBipolar();
        const float sign = rng.NextBipolar() >= 0.0f ? 1.0f : -1.0f;
        taps.delay[i] = static_cast<float>(ms * 0.001 * sampleRate);
        taps.gainL[i] = static_cast<float>(level) * std::sqrt(1.0f - pan) * sign;
        taps.gainR[i] = static_cast<float>(level) * std::sqrt(pan) * (rng.NextBipolar() >= -0.3f ? sign : -sign);
        taps.late[i] = 0;
        taps.spread[i] = 0.0f;
        norm += taps.gainL[i] * taps.gainL[i] + taps.gainR[i] * taps.gainR[i];
    }

    const float scale = norm > 0.0f ? 1.0f / std::sqrt(0.5f * norm) : 0.0f;

    for (std::size_t i = 0; i < taps.count; ++i)
    {
        taps.gainL[i] *= scale;
        taps.gainR[i] *= scale;
    }
}

/**
 * Image sources for a rectangular room, 9 ft high, listened to from a position on its floor
 * plan. Each wall reflection mirrors the source; reflection order sets how many walls a path
 * has met, and each meeting costs `absorption`. Delays are relative to the direct path, so the
 * dry signal stands in for it, and level falls as distance ratio. The strongest
 * TapSet::kMaxTaps are kept.
 *
 * `areaSqFt` 100-1000; `aspect` = length / width; `x`, `y` 0-1 across the width and from front
 * to back.
 */
inline void BuildRoomImages(TapSet& taps, double areaSqFt, double aspect, double x, double y, double absorption,
                            double sampleRate) noexcept
{
    constexpr double kFeetPerMetre = 3.28084;
    constexpr double kSpeedOfSound = 343.0;
    constexpr int kOrder = 4;
    const double width = std::sqrt(areaSqFt / aspect) / kFeetPerMetre;
    const double length = width * aspect;
    const double height = 9.0 / kFeetPerMetre;
    // The source sits front and centre, a metre in; the listener anywhere else on the plan.
    const double sx = 0.5 * width, sy = std::min(1.0, 0.25 * length), sz = 1.2;
    const double lx = (0.1 + 0.8 * x) * width;
    const double ly = sy + (0.15 + 0.8 * y) * (length - sy) * 0.95;
    const double lz = 1.2;
    const double direct = std::max(0.5, std::hypot(lx - sx, ly - sy));

    taps.count = 0;
    float weakest = 0.0f;
    std::size_t weakestIndex = 0;

    for (int ix = -kOrder; ix <= kOrder; ++ix)
    {
        for (int iy = -kOrder; iy <= kOrder; ++iy)
        {
            for (int iz = -1; iz <= 1; ++iz)
            {
                const int order = std::abs(ix) + std::abs(iy) + std::abs(iz);

                if (order == 0 || order > kOrder + 1)
                {
                    continue;
                }

                // Mirror the source across the walls: odd images reflect it, even ones translate it.
                const double imx = (ix % 2 == 0) ? ix * width + sx : (ix + 1) * width - sx;
                const double imy = (iy % 2 == 0) ? iy * length + sy : (iy + 1) * length - sy;
                const double imz = (iz % 2 == 0) ? iz * height + sz : (iz + 1) * height - sz;
                const double dx = imx - lx, dy = imy - ly, dz = imz - lz;
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                const auto level =
                    static_cast<float>((direct / distance) * std::pow(1.0 - absorption, static_cast<double>(order)));
                // Arrival angle across the head: -1 hard left to +1 hard right.
                const auto side = static_cast<float>(std::clamp(dx / std::max(0.1, std::hypot(dx, dy)), -1.0, 1.0));
                const float gainL = level * std::sqrt(0.5f * (1.0f - 0.9f * side));
                const float gainR = level * std::sqrt(0.5f * (1.0f + 0.9f * side));
                // The far ear hears it up to 0.65 ms later: what keeps a room's two sides apart.
                const auto itd = static_cast<float>(0.5 * 0.00065 * side * sampleRate);
                const auto delay = static_cast<float>((distance - direct) / kSpeedOfSound * sampleRate);

                if (delay < 1.0f)
                {
                    continue;
                }

                std::size_t slot = taps.count;

                if (taps.count == TapSet::kMaxTaps)
                {
                    if (gainL + gainR <= weakest)
                    {
                        continue;
                    }

                    slot = weakestIndex;
                }
                else
                {
                    ++taps.count;
                }

                taps.delay[slot] = delay;
                taps.gainL[slot] = gainL;
                taps.gainR[slot] = gainR;
                taps.late[slot] = order > 2 ? 1 : 0;
                taps.spread[slot] = itd;

                if (taps.count == TapSet::kMaxTaps)
                {
                    weakest = 1.0e9f;

                    for (std::size_t i = 0; i < taps.count; ++i)
                    {
                        const float l = std::fabs(taps.gainL[i]) + std::fabs(taps.gainR[i]);

                        if (l < weakest)
                        {
                            weakest = l;
                            weakestIndex = i;
                        }
                    }
                }
            }
        }
    }
}

/**
 * A state-variable band-pass (topology-preserving, so its centre can move every block without
 * the zipper a biquad's coefficients give), unity gain at the centre.
 */
class SvfBandpass
{
  public:
    void Set(double hz, double q, double sampleRate) noexcept
    {
        const double g = std::tan(kPi * std::clamp(hz, 20.0, 0.45 * sampleRate) / sampleRate);
        const double k = 1.0 / std::max(0.3, q);
        mG = static_cast<float>(g);
        mK = static_cast<float>(k);
        mA1 = static_cast<float>(1.0 / (1.0 + g * (g + k)));
    }

    void Reset() noexcept
    {
        mIc1 = 0.0f;
        mIc2 = 0.0f;
    }

    [[nodiscard]] float Process(float x) noexcept
    {
        const float v3 = x - mIc2;
        const float v1 = mA1 * (mIc1 + mG * v3);
        const float v2 = mIc2 + mG * v1;
        mIc1 = delay_line::FlushDenormal(2.0f * v1 - mIc1);
        mIc2 = delay_line::FlushDenormal(2.0f * v2 - mIc2);
        return mK * v1;
    }

  private:
    float mG = 0.1f;
    float mK = 1.0f;
    float mA1 = 1.0f;
    float mIc1 = 0.0f;
    float mIc2 = 0.0f;
};

/**
 * The Choir: the tail through a vowel's first three formants plus the singer's formant near
 * 2.8 kHz, in parallel. The double vowels glide between their two, and Random wanders over all
 * five; Mod adds a slow drift to each formant so the voices never sit still.
 *
 * The drifts keep phases of their own: a multiple of the vowel's phase that isn't a whole number
 * would jump every time that phase wraps.
 */
class FormantBank
{
  public:
    static constexpr std::size_t kVowels = 5; // Aah, Oh, Ooh, Eh, Ee

    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = sampleRate;
        mPhase = 0.0;
        Reset();
    }

    void Reset() noexcept
    {
        for (auto& side : mFilters)
        {
            for (auto& f : side)
            {
                f.Reset();
            }
        }
    }

    /// Picks the vowel motion (VerbalParams kVowelLabels order) and the formant Q.
    void Configure(int vowel, int resonance, float mod) noexcept
    {
        mVowel = vowel;
        mQ = resonance == 0 ? 5.0 : resonance == 1 ? 9.0 : 15.0;
        mMod = mod;
    }

    /// Advances the vowel motion by one block and recomputes the filters.
    void UpdateBlock(int samples) noexcept
    {
        const double seconds = static_cast<double>(samples) / mSampleRate;
        mPhase += seconds * 0.11;
        mPhase -= std::floor(mPhase);

        for (std::size_t f = 0; f < mDrift.size(); ++f)
        {
            mDrift[f] += seconds * 0.11 * (1.7 + static_cast<double>(f));
            mDrift[f] -= std::floor(mDrift[f]);
        }

        const double glide = 0.5 - 0.5 * std::cos(kTwoPi * mPhase);
        std::size_t a = 0, b = 0;
        double t = 0.0;

        switch (mVowel)
        {
        case 0:
            a = 0, b = 2, t = glide;
            break; // Aah-Ooh
        case 1:
            a = b = 0;
            break; // Aah
        case 2:
            a = 0, b = 1, t = glide;
            break; // Aah-Oh
        case 3:
            a = b = 1;
            break; // Oh
        case 4:
            a = 2, b = 1, t = glide;
            break; // Ooh-Oh
        case 5:
            a = b = 2;
            break; // Ooh
        default: {
            // Random: a slow walk round all five vowels.
            const double walk = mPhase * static_cast<double>(kVowels);
            a = static_cast<std::size_t>(walk) % kVowels;
            b = (a + 1) % kVowels;
            t = 0.5 - 0.5 * std::cos(kPi * (walk - std::floor(walk)));
            break;
        }
        }

        for (std::size_t f = 0; f < 3; ++f)
        {
            const double drift = 1.0 + 0.04 * mMod * std::sin(kTwoPi * (mDrift[f] + 0.3 * f));
            const double hz = (kFormants[a][f] * (1.0 - t) + kFormants[b][f] * t) * drift;
            mFilters[0][f].Set(hz, mQ, mSampleRate);
            mFilters[1][f].Set(hz * 1.012, mQ, mSampleRate);
        }

        mFilters[0][3].Set(2800.0, 4.0, mSampleRate);
        mFilters[1][3].Set(2830.0, 4.0, mSampleRate);
    }

    void Process(float& l, float& r) noexcept
    {
        l = Voice(0, l);
        r = Voice(1, r);
    }

  private:
    static constexpr double kFormants[kVowels][3] = {
        {730.0, 1090.0, 2440.0}, // Aah
        {500.0, 850.0, 2400.0},  // Oh
        {300.0, 870.0, 2240.0},  // Ooh
        {530.0, 1840.0, 2480.0}, // Eh
        {290.0, 2250.0, 2900.0}, // Ee
    };
    static constexpr std::array<float, 4> kFormantGains = {1.0f, 0.7f, 0.35f, 0.3f};

    [[nodiscard]] float Voice(std::size_t side, float x) noexcept
    {
        float y = 0.0f;

        for (std::size_t f = 0; f < 4; ++f)
        {
            y += kFormantGains[f] * mFilters[side][f].Process(x);
        }

        return y;
    }

    std::array<std::array<SvfBandpass, 4>, 2> mFilters;
    double mSampleRate = 48000.0;
    double mPhase = 0.0;
    std::array<double, 3> mDrift{};
    double mQ = 9.0;
    int mVowel = 0;
    float mMod = 0.4f;
};

/**
 * The Swell: a gain that falls to silence at each picked note and rises over Rise Time, like a
 * volume pedal rocked back and forward after every attack. Onsets are a fast envelope jumping
 * 9 dB over a slow one, with a 60 ms hold-off so one note's ring does not retrigger it. The fall
 * takes a few milliseconds: an instant one would cut whatever is sounding dead.
 */
class SwellEnvelope
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = sampleRate;
        mFast.Prepare(sampleRate);
        mFast.SetTimes(1.0, 25.0);
        mSlow.Prepare(sampleRate);
        mSlow.SetTimes(40.0, 400.0);
        Reset();
    }

    void Reset() noexcept
    {
        mFast.Reset();
        mSlow.Reset();
        mPosition = 1.0f;
        mFalling = false;
        mHoldOff = 0;
    }

    void SetRiseSeconds(double seconds) noexcept
    {
        mStep = static_cast<float>(1.0 / (std::max(0.01, seconds) * mSampleRate));
    }

    void SetFallSeconds(double seconds) noexcept
    {
        mFallStep = static_cast<float>(1.0 / (std::max(0.0005, seconds) * mSampleRate));
    }

    /// The swell gain for this input sample.
    [[nodiscard]] float Process(float input) noexcept
    {
        const float fast = mFast.Process(input);
        const float slow = mSlow.Process(input);

        if (mHoldOff > 0)
        {
            --mHoldOff;
        }
        else if (fast > 2.8f * slow + 1.0e-4f && fast > 0.003f)
        {
            mFalling = true;
            mHoldOff = static_cast<int>(0.06 * mSampleRate);
        }

        if (mFalling)
        {
            mPosition = std::max(0.0f, mPosition - mFallStep);
            mFalling = mPosition > 0.0f;
        }
        else
        {
            mPosition = std::min(1.0f, mPosition + mStep);
        }

        // Smoothstep: a pedal moves slowly at both ends of its travel.
        return mPosition * mPosition * (3.0f - 2.0f * mPosition);
    }

  private:
    delay_line::EnvelopeFollower mFast;
    delay_line::EnvelopeFollower mSlow;
    double mSampleRate = 48000.0;
    float mPosition = 1.0f;
    float mStep = 0.0f;
    float mFallStep = 1.0f;
    bool mFalling = false;
    int mHoldOff = 0;
};

/**
 * The Nonlinear machine's generator: 48 taps spread over Time, their levels following one of
 * six envelopes (VerbalParams kShapeLabels order), so an impulse comes out as that shape —
 * rising to a cut-off for the reverse shapes, flat then cut for Gate, a bell, an anti-bell.
 * The taps drift slowly at Mod Speed, so the grain is never a fixed pattern. Time glides and a
 * new Shape cross-fades its levels, so moving either bends the sound rather than ticking.
 */
class ShapeGenerator
{
  public:
    static constexpr std::size_t kTaps = 48;

    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = sampleRate;
        mLfo.Reset();
        mTime.Prepare(sampleRate);
        mTime.SetTimeConstantMs(60.0);
        mTime.SetMaxStep(delay_line::kMaxGlideStep);
        mEase = static_cast<float>(1.0 - std::exp(-1.0 / (0.03 * sampleRate)));
        mGainSettleSamples = static_cast<int>(0.25 * sampleRate);
    }

    void Reset() noexcept
    {
        mLfo.Reset();
    }

    /// With `snap`, Time and Shape land at once, for a fresh machine.
    void Configure(int shape, double timeMs, double modSpeed, double mod, bool snap) noexcept
    {
        delay_line::Rng rng;
        rng.Seed(0x51a9e5u);
        float norm = 0.0f;

        for (std::size_t i = 0; i < kTaps; ++i)
        {
            const double u = (static_cast<double>(i) + 0.5 + 0.35 * rng.NextBipolar()) / static_cast<double>(kTaps);
            mFraction[i] = static_cast<float>(std::clamp(u, 0.0, 1.0));
            const double e = Envelope(shape, u);
            const float pan = 0.5f + 0.4f * rng.NextBipolar();
            mTargetL[i] = static_cast<float>(e) * std::sqrt(1.0f - pan) * (rng.NextBipolar() >= 0.0f ? 1.0f : -1.0f);
            mTargetR[i] = static_cast<float>(e) * std::sqrt(pan) * (rng.NextBipolar() >= 0.0f ? 1.0f : -1.0f);
            norm += mTargetL[i] * mTargetL[i] + mTargetR[i] * mTargetR[i];
        }

        const float scale = norm > 0.0f ? 1.0f / std::sqrt(0.5f * norm) : 0.0f;

        for (std::size_t i = 0; i < kTaps; ++i)
        {
            mTargetL[i] *= scale;
            mTargetR[i] *= scale;
        }

        mTime.SetTarget(timeMs * 0.001 * mSampleRate);
        mModTarget = static_cast<float>((0.2 + 2.5 * mod) * 0.001 * mSampleRate);
        mLfo.SetRate(0.1 + 4.0 * modSpeed * modSpeed, mSampleRate);

        if (snap)
        {
            mTime.Snap(mTime.Target());
            mGainL = mTargetL;
            mGainR = mTargetR;
            mModDepth = mModTarget;
            mGainsSettling = 0;
        }
        else if (shape != mShape)
        {
            mGainsSettling = mGainSettleSamples;
        }

        mShape = shape;
    }

    /// Reads the taps off `line` (mono), which the caller writes.
    void Process(const delay_line::FractionalDelayLine& line, float& outL, float& outR) noexcept
    {
        if (mGainsSettling > 0)
        {
            StepGains();
        }

        mLfo.Advance();
        mModDepth += (mModTarget - mModDepth) * mEase;
        const auto time = static_cast<float>(mTime.Next());
        float l = 0.0f;
        float r = 0.0f;

        for (std::size_t i = 0; i < kTaps; ++i)
        {
            const float wobble = (i & 1u) ? mLfo.Sin() : mLfo.Cos();
            const float tap = mFraction[i] * time + 2.0f + mModDepth * (1.0f + wobble);
            const float sample = line.ReadLinear(static_cast<double>(tap));
            l += mGainL[i] * sample;
            r += mGainR[i] * sample;
        }

        outL = l;
        outR = r;
    }

    /// Time as it stands now, in samples: one repeat of the shape.
    [[nodiscard]] double TimeSamples() const noexcept
    {
        return mTime.Value();
    }

  private:
    void StepGains() noexcept
    {
        for (std::size_t i = 0; i < kTaps; ++i)
        {
            mGainL[i] += (mTargetL[i] - mGainL[i]) * mEase;
            mGainR[i] += (mTargetR[i] - mGainR[i]) * mEase;
        }

        if (--mGainsSettling == 0)
        {
            mGainL = mTargetL;
            mGainR = mTargetR;
        }
    }

    [[nodiscard]] static double Envelope(int shape, double u) noexcept
    {
        switch (shape)
        {
        case 0:
            return u * u * u; // Swoosh: late build
        case 1:
            return u; // Reverse: ramp to a cut
        case 2:
            return std::sqrt(u); // Ramp: fast rise
        case 3:
            return 1.0; // Gate: flat then cut
        case 4:
            return std::exp(-((u - 0.5) * (u - 0.5)) / (2.0 * 0.16 * 0.16)); // Gauss: a bell
        default:
            return 0.12 + 0.88 * (1.0 - std::exp(-((u - 0.5) * (u - 0.5)) / (2.0 * 0.2 * 0.2))); // Bounce
        }
    }

    std::array<float, kTaps> mFraction{};
    std::array<float, kTaps> mGainL{};
    std::array<float, kTaps> mGainR{};
    std::array<float, kTaps> mTargetL{};
    std::array<float, kTaps> mTargetR{};
    delay_line::GlideRamp mTime;
    double mSampleRate = 48000.0;
    float mEase = 0.001f;
    float mModDepth = 0.0f;
    float mModTarget = 0.0f;
    int mShape = -1;
    int mGainsSettling = 0;
    int mGainSettleSamples = 1;
    RotatingPhasor mLfo;
};
} // namespace guitarfx::reverb
