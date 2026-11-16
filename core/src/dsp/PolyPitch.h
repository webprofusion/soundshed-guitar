#pragma once

#include "dsp/PolySpectrum.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace guitarfx
{
/**
 * The exact pitch of one note in a chord, from the phase-placed spectrum (PolySpectrum): what a
 * polyphonic pitch bend follows.
 *
 * Each of the note's first few partials is looked for within a little of where it should be,
 * given where the note was last frame. The phase-placed grid holds each partial's energy together
 * with the exact place its phase gave it (PlacedMoment), so the energy-weighted place over those
 * bins is the partial's frequency to well under a cent when it stands alone. Each partial gives
 * the note's pitch (the place less its harmonic interval); the partials are averaged by energy.
 *
 * In a chord a partial may share its place with one of another note's. A partial of this note
 * that falls within kCollisionSemitones of one of the other notes' (their first kOtherPartials)
 * is left out, unless every one is, so a note's bend is not pulled by its neighbours.
 */
namespace poly_pitch
{
constexpr int kMaxPartials = 8;
constexpr int kOtherPartials = 6;
constexpr double kCollisionSemitones = 0.6;

struct Options
{
    int window = 2;               ///< which PolySpectrum window, 0 the longest
    int partials = 4;             ///< how many to read, from the fundamental up
    double searchSemitones = 0.4; ///< how far either side of where each is expected
    bool avoidCollisions = true;
    /// When set, each partial is looked for as far as halfway to the nearest partial of another
    /// note, up to this far, and no less than kMinSearchSemitones; searchSemitones is not used.
    double widestSearchSemitones = 0.0;
};

constexpr double kMinSearchSemitones = 0.15;

struct Reading
{
    double pitch = 0.0;  ///< fractional MIDI note
    double energy = 0.0; ///< of the partials used, linear; 0 when nothing was found
    double spread = 0.0; ///< how far the partials used disagree, energy-weighted, in semitones
    int partials = 0;    ///< how many were used
};

/// The pitch near `aroundMidi`, the other sounding notes' pitches being `others`.
[[nodiscard]] inline Reading Measure(const PolySpectrum& spectrum, double aroundMidi, std::span<const double> others,
                                     const Options& options)
{
    const float* energy = spectrum.PlacedEnergy(options.window);
    const float* moment = spectrum.PlacedMoment(options.window);
    const int partials = std::clamp(options.partials, 1, kMaxPartials);
    const double perSemitone = PolySpectrum::kBinsPerSemitone;

    double sums[2][3] = {}; // [collides][energy, energy x pitch, energy x pitch squared]
    int counts[2] = {};

    for (int h = 1; h <= partials; ++h)
    {
        const double interval = 12.0 * std::log2(static_cast<double>(h));
        const double expected = aroundMidi + interval;
        const double centre = (expected - PolySpectrum::kLowestMidi) * perSemitone;
        double reach = options.searchSemitones;

        if (options.widestSearchSemitones > 0.0)
        {
            reach = options.widestSearchSemitones;

            for (double other : others)
            {
                for (int g = 1; g <= kOtherPartials; ++g)
                {
                    const double place = other + 12.0 * std::log2(static_cast<double>(g));
                    reach = std::min(reach, std::max(kMinSearchSemitones, 0.5 * std::abs(place - expected)));
                }
            }
        }

        const int first = std::max(0, static_cast<int>(std::ceil(centre - reach * perSemitone)));
        const int last = std::min(PolySpectrum::kBins - 1, static_cast<int>(std::floor(centre + reach * perSemitone)));

        if (first > last)
        {
            continue;
        }

        double e = 0.0;
        double m = 0.0;

        for (int k = first; k <= last; ++k)
        {
            e += energy[k];
            m += moment[k];
        }

        if (!(e > 0.0))
        {
            continue;
        }

        bool collides = false;

        if (options.avoidCollisions)
        {
            for (double other : others)
            {
                for (int g = 1; g <= kOtherPartials && !collides; ++g)
                {
                    const double place = other + 12.0 * std::log2(static_cast<double>(g));
                    collides = std::abs(place - expected) < kCollisionSemitones;
                }

                if (collides)
                {
                    break;
                }
            }
        }

        const double pitch = PolySpectrum::BinMidi(0) + (m / e) / perSemitone - interval;
        const int group = collides ? 1 : 0;
        sums[group][0] += e;
        sums[group][1] += e * pitch;
        sums[group][2] += e * pitch * pitch;
        ++counts[group];
    }

    const int use = sums[0][0] > 0.0 ? 0 : 1;
    Reading reading;

    if (sums[use][0] > 0.0)
    {
        reading.energy = sums[use][0];
        reading.pitch = sums[use][1] / sums[use][0];
        reading.spread = std::sqrt(std::max(0.0, sums[use][2] / sums[use][0] - reading.pitch * reading.pitch));
        reading.partials = counts[use];
    }

    return reading;
}
} // namespace poly_pitch
} // namespace guitarfx
