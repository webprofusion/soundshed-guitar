#pragma once

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

/**
 * Pickup Match's String Comb (a prototype): what a pickup's place on the string does to one
 * string, on top of String Sensing's average of every note.
 *
 * A pickup hears the wave travelling along the string towards the bridge and, a moment later,
 * the same wave coming back inverted off the saddle. For a string whose open pitch is f0 on a
 * scale L, a pickup p from the saddle hears the return p / (L f0) seconds late, whatever fret is
 * played: a comb, 1 - z^-D, with notches at the frequencies whose half-waves fit between the
 * pickup and the saddle. Those notches are 10-20 dB deep and differ on each string, and String
 * Sensing's note average (PickupSensing.generated.h) smooths them into a shelf of a few dB.
 *
 * The comb here is the target's, as detail over that average, which stays as it is:
 *
 *     H(z) = 1 - g (L(z) z^-D - F(z))
 *
 * L is a low-pass on the returning wave and F a lower one on the wave going out. F gives back
 * below the comb's first peak what the return takes away there (measured: within 0.4 dB), so
 * the low end is the average's alone; L fades the return out above the first few notches, so
 * the notches do not run evenly to the top of the band, which on anything but a clean string
 * (pick noise, hiss, a chord) is the sound of a flanger. The source's own comb is already in the
 * signal and is left there: undoing it takes a feedback loop that rings. The depth g also falls
 * as the two pickups' places come together, to nothing where they are the same.
 *
 * One input cannot say which string a note is on, so the comb runs for one string at a time:
 * a chosen one, or the one the tracked pitch would be played on nearest the nut. Chords, and
 * notes played higher up a lower string, get the wrong string's comb. A change of string fades
 * from one comb to the other; a delay swept between them would be heard as a pitch bend.
 */
namespace guitarfx::pickup_match
{
/// A sensing profile's geometry in millimetres.
struct StringGeometry
{
    double scaleMm;
    double positionMm; ///< from the bridge saddle
    double coilGapMm;  ///< between a humbucker's coils, 0 for one coil (the average's, not the comb's)
};

/// In kSensingFits order (single coil, P-90, humbucker; each neck, middle, bridge), and the
/// same figures as tools/gen-pickup-sensing.mjs's FAMILIES: keep the two alike.
inline constexpr std::array<StringGeometry, 9> kStringGeometry = {{
    {648.0, 160.0, 0.0},
    {648.0, 100.0, 0.0},
    {648.0, 41.0, 0.0},
    {628.0, 152.0, 0.0},
    {628.0, 100.0, 0.0},
    {628.0, 45.0, 0.0},
    {628.0, 152.0, 18.0},
    {628.0, 100.0, 18.0},
    {628.0, 45.0, 18.0},
}};

/// Standard tuning, low E to high E.
inline constexpr std::array<double, 6> kOpenStringHz = {82.41, 110.0, 146.83, 196.0, 246.94, 329.63};

/// The string the comb runs for until a pitch has been tracked: the D, in the middle.
inline constexpr std::size_t kFallbackString = 2;

/// How deep the comb goes at String Comb 1, between pickups an octave or more apart in place.
inline constexpr double kMaxCombDepth = 0.7;
/// The return is cut above this many notches, and made up below this fraction of the first.
inline constexpr double kCombNotchesKept = 3.0;
inline constexpr double kCombLowEdge = 0.16;

/// The string a pitch is played on nearest the nut: the highest whose open note is at or
/// below it. `current` holds on through a semitone below its open note, so vibrato on a
/// boundary note does not flip between two strings.
[[nodiscard]] inline std::size_t StringForPitch(double hz, std::size_t current) noexcept
{
    constexpr double kHalfSemitone = 0.971532; // 2^(-0.5/12)
    constexpr double kHold = 0.917004;         // 2^(-1.5/12)
    std::size_t string = 0;

    for (std::size_t i = 1; i < kOpenStringHz.size(); ++i)
    {
        const double floor = kOpenStringHz[i] * (i == current ? kHold : kHalfSemitone);

        if (hz >= floor)
        {
            string = i;
        }
    }

    return string;
}

/// What a design asks of the comb: how strongly, and between which two sensing profiles.
struct CombSettings
{
    double strength = 0.0; ///< 0 (off) to 1
    std::size_t sourceProfile = 0;
    std::size_t targetProfile = 0;
};

/// The comb for one string: its delay in samples, the one-pole coefficients of the return's
/// low-pass and of the low end's make-up, its depth, and the gain that keeps its level.
struct CombValues
{
    double delay = 1.0;
    double lowpass = 1.0;
    double makeup = 0.0;
    double depth = 0.0;
    double gain = 1.0;
};

[[nodiscard]] inline CombValues CombValuesFor(const CombSettings& settings, std::size_t string,
                                              double sampleRate) noexcept
{
    const auto& source = kStringGeometry[std::min(settings.sourceProfile, kStringGeometry.size() - 1)];
    const auto& target = kStringGeometry[std::min(settings.targetProfile, kStringGeometry.size() - 1)];
    const double open = kOpenStringHz[std::min(string, kOpenStringHz.size() - 1)];
    const double strength =
        settings.sourceProfile == settings.targetProfile ? 0.0 : std::clamp(settings.strength, 0.0, 1.0);
    const double seconds = target.positionMm / (target.scaleMm * open);
    const auto pole = [sampleRate](double hz) {
        return 1.0 - std::exp(-2.0 * 3.14159265358979323846 * std::min(hz, 0.45 * sampleRate) / sampleRate);
    };

    // How far the pickup moves, in octaves of its distance from the saddle, up to one.
    const double moved =
        std::min(1.0, std::abs(std::log2((target.positionMm / target.scaleMm) / (source.positionMm / source.scaleMm))));

    CombValues values;
    values.delay = std::max(1.0, sampleRate * seconds);
    values.lowpass = pole(kCombNotchesKept / seconds);
    values.makeup = pole(kCombLowEdge / seconds);
    values.depth = kMaxCombDepth * strength * moved;
    // About half the return's power lies inside its band.
    values.gain = 1.0 / std::sqrt(1.0 + 0.5 * values.depth * values.depth);
    return values;
}

/// The comb's gain in dB at a frequency, with an ideal delay.
[[nodiscard]] inline double CombResponseDb(const CombValues& values, double frequencyHz, double sampleRate) noexcept
{
    const double w = 2.0 * 3.14159265358979323846 * frequencyHz / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const auto onePole = [&z1](double a) { return a / (1.0 - (1.0 - a) * z1); };
    const std::complex<double> back =
        onePole(values.lowpass) * std::polar(1.0, -w * values.delay) - onePole(values.makeup);
    const std::complex<double> response = (1.0 - values.depth * back) * values.gain;
    return 20.0 * std::log10(std::max(std::abs(response), 1.0e-12));
}

/// The comb, for up to two channels that share one string.
class StringComb
{
  public:
    /// Allocates: the longest delay is the low E under a neck pickup, 3 ms.
    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        std::size_t size = 64;

        while (static_cast<double>(size) < mSampleRate * kLongestDelaySeconds + 8.0)
        {
            size *= 2;
        }

        mMask = size - 1;

        for (auto& ring : mRings)
        {
            ring.assign(size, 0.0);
        }

        mGlide = 1.0 - std::exp(-1.0 / (kGlideSeconds * mSampleRate));
        mFadeStep = 1.0 / (kFadeSeconds * mSampleRate);
        mDepth = mGoal.depth;
        mGain = mGoal.gain;
        Clear();
    }

    void Clear() noexcept
    {
        for (auto& ring : mRings)
        {
            std::fill(ring.begin(), ring.end(), 0.0);
        }

        mVoice.state = {};
        mLast.state = {};
        mFade = 1.0;
        mWrite = 0;
    }

    /// Whether Process() changes anything, now or while a change glides out.
    [[nodiscard]] bool Engaged() const noexcept
    {
        return mGoal.depth > 0.0 || mDepth > 1.0e-5;
    }

    /// Where the comb goes: the depth and gain glide, and a new delay fades in over the old.
    void SetGoal(const CombValues& goal) noexcept
    {
        if (!Engaged())
        {
            Clear();
            mDepth = 0.0;
            mGain = 1.0;
            mVoice.Set(goal);
        }
        else if (goal.delay != mVoice.delay)
        {
            mLast = mVoice;
            mVoice.Set(goal);
            mVoice.state = {};
            mFade = 0.0;
        }

        mGoal = goal;
    }

    /// Once per sample, before the channels' Process().
    void Advance() noexcept
    {
        mWrite = (mWrite + 1) & mMask;
        mDepth += mGlide * (mGoal.depth - mDepth);
        mGain += mGlide * (mGoal.gain - mGain);
        mFade = std::min(1.0, mFade + mFadeStep);
    }

    [[nodiscard]] double Process(std::size_t channel, double input) noexcept
    {
        auto& ring = mRings[channel];
        ring[mWrite] = input;
        double back = Return(mVoice, channel, ring);

        if (mFade < 1.0)
        {
            back = mFade * back + (1.0 - mFade) * Return(mLast, channel, ring);
        }

        const double output = (input - mDepth * back) * mGain;

        if (!IsFinite(output))
        {
            std::fill(ring.begin(), ring.end(), 0.0);
            mVoice.state = {};
            mLast.state = {};
            return 0.0;
        }

        return output;
    }

  private:
    static constexpr double kLongestDelaySeconds = 0.0035;
    /// A change of depth settles over about this long, so it does not click.
    static constexpr double kGlideSeconds = 0.008;
    /// One string's comb gives way to the next over this long.
    static constexpr double kFadeSeconds = 0.015;

    /// One string's return: where it is read, and its two filters.
    struct Voice
    {
        struct Filter
        {
            double low = 0.0;
            double under = 0.0;
        };

        void Set(const CombValues& values) noexcept
        {
            delay = values.delay;
            lowpass = values.lowpass;
            makeup = values.makeup;
        }

        double delay = 1.0;
        double lowpass = 1.0;
        double makeup = 0.0;
        std::array<Filter, 2> state{};
    };

    /// The wave coming back for one voice: the input `delay` ago, between samples by a straight
    /// line, low-passed, less the low end's make-up off the newest sample.
    [[nodiscard]] double Return(Voice& voice, std::size_t channel, const std::vector<double>& ring) const noexcept
    {
        const auto whole = static_cast<std::size_t>(voice.delay);
        const double part = voice.delay - static_cast<double>(whole);
        const double a = ring[(mWrite - whole) & mMask];
        const double b = ring[(mWrite - whole - 1) & mMask];
        auto& filter = voice.state[channel];
        filter.low += voice.lowpass * (a + part * (b - a) - filter.low);
        filter.under += voice.makeup * (ring[mWrite] - filter.under);
        return filter.low - filter.under;
    }

    double mSampleRate = 48000.0;
    double mGlide = 1.0;
    double mFadeStep = 1.0;
    double mFade = 1.0;
    double mDepth = 0.0;
    double mGain = 1.0;
    std::size_t mMask = 0;
    std::size_t mWrite = 0;
    CombValues mGoal;
    Voice mVoice;
    Voice mLast;
    std::array<std::vector<double>, 2> mRings;
};
} // namespace guitarfx::pickup_match
