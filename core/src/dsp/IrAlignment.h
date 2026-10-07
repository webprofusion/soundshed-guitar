#pragma once

#include "dsp/BiquadDesign.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace guitarfx::ir_alignment
{
/**
 * Where IR B lines up with IR A, for the IR Cabinet's Alignment section.
 *
 * Two IRs blended in one cabinet sum like two mics on a speaker, and how they sum depends on
 * their timing. Moving B by `o` samples against A changes the cross term of the sum's energy,
 * 2 * sum(a[n] * b[n - o]), and nothing else, so the offset that sums fullest is the one with the
 * largest cross-correlation. Its sign says whether B lines up the right way up or inverted.
 *
 * Both IRs are band-limited to 100 Hz-6 kHz first. Below that a millisecond hardly matters and a
 * cabinet's low resonance makes the correlation broad; above it two mics never line up anyway, and
 * fizz is not where a cancellation is heard. The same filter on both keeps their relative timing.
 *
 * A strong cabinet resonance gives the correlation side peaks one period apart, any of which can
 * be the tallest. The arrivals settle which one is meant: the search is held to 2 ms either side
 * of the offset that lines up the two IRs' onsets.
 */

/// The band the correlation listens to, in Hz.
inline constexpr double kLowEdgeHz = 100.0;
inline constexpr double kHighEdgeHz = 6000.0;
/// How far from the onsets' offset the peak may be, in ms.
inline constexpr double kSearchAroundOnsetMs = 2.0;

struct Analysis
{
    bool valid = false;
    /// The slotBOffset, in ms, that lines IR B up with IR A.
    double alignedOffsetMs = 0.0;
    /// Whether B lines up with A inverted, its polarity opposite to A's.
    bool invertB = false;
    /// How alike the two are once lined up: the normalised correlation, 0..1.
    double match = 0.0;
    /// Where each IR starts, in ms from its first sample.
    double onsetAMs = 0.0;
    double onsetBMs = 0.0;
    /// The normalised correlation with B moved by each whole number of samples, from
    /// -maxOffsetSamples (entry 0) to +maxOffsetSamples. Signed: negative where the two cancel.
    std::vector<double> matchByOffset;
    int maxOffsetSamples = 0;
};

/// The first sample at or above `fractionOfPeak` of the IR's largest. 0 for a silent IR.
[[nodiscard]] inline std::size_t FindOnset(std::span<const float> ir, double fractionOfPeak = 0.1)
{
    float peak = 0.0f;

    for (const float sample : ir)
    {
        peak = std::max(peak, std::abs(sample));
    }

    if (peak <= 0.0f)
    {
        return 0;
    }

    const auto threshold = static_cast<float>(fractionOfPeak) * peak;

    for (std::size_t n = 0; n < ir.size(); ++n)
    {
        if (std::abs(ir[n]) >= threshold)
        {
            return n;
        }
    }

    return 0;
}

/// The first `length` samples of `ir`, band-limited to what the correlation listens to.
[[nodiscard]] inline std::vector<double> BandLimit(std::span<const float> ir, double sampleRate, std::size_t length)
{
    const BiquadCoefficients highPass = biquad::HighPass(kLowEdgeHz, biquad::kButterworthQ, sampleRate);
    const BiquadCoefficients lowPass = biquad::LowPass(kHighEdgeHz, biquad::kButterworthQ, sampleRate);
    biquad::State highPassState;
    biquad::State lowPassState;
    std::vector<double> filtered(length);

    for (std::size_t n = 0; n < length; ++n)
    {
        const double sample = n < ir.size() ? static_cast<double>(ir[n]) : 0.0;
        filtered[n] = lowPassState.Process(lowPass, highPassState.Process(highPass, sample));
    }

    return filtered;
}

/// Lines `b` up with `a`, both mono and at `sampleRate`, looking up to `maxOffsetMs` either way
/// over the first `windowMs` of each. Not valid when either is empty or silent.
[[nodiscard]] inline Analysis Analyse(std::span<const float> a, std::span<const float> b, double sampleRate,
                                      double maxOffsetMs = 10.0, double windowMs = 100.0)
{
    Analysis result;

    if (a.empty() || b.empty() || !(sampleRate > 0.0))
    {
        return result;
    }

    const auto window = static_cast<std::size_t>(std::max(1.0, windowMs * sampleRate / 1000.0));
    const std::size_t length = std::min(window, std::max(a.size(), b.size()));
    const std::vector<double> filteredA = BandLimit(a, sampleRate, length);
    const std::vector<double> filteredB = BandLimit(b, sampleRate, length);
    double energyA = 0.0;
    double energyB = 0.0;

    for (std::size_t n = 0; n < length; ++n)
    {
        energyA += filteredA[n] * filteredA[n];
        energyB += filteredB[n] * filteredB[n];
    }

    if (energyA <= 1e-20 || energyB <= 1e-20)
    {
        return result;
    }

    const double norm = 1.0 / std::sqrt(energyA * energyB);
    const int maxOffset = std::max(1, static_cast<int>(std::lround(maxOffsetMs * sampleRate / 1000.0)));
    const auto signedLength = static_cast<int>(length);
    result.maxOffsetSamples = maxOffset;
    result.matchByOffset.assign(static_cast<std::size_t>(2 * maxOffset + 1), 0.0);

    // matchByOffset[o + maxOffset] = sum over n of a[n] * b[n - o]: B played o samples later.
    for (int offset = -maxOffset; offset <= maxOffset; ++offset)
    {
        const int first = std::max(0, offset);
        const int last = std::min(signedLength, signedLength + offset);
        double sum = 0.0;

        for (int n = first; n < last; ++n)
        {
            sum += filteredA[static_cast<std::size_t>(n)] * filteredB[static_cast<std::size_t>(n - offset)];
        }

        result.matchByOffset[static_cast<std::size_t>(offset + maxOffset)] = sum * norm;
    }

    const std::size_t onsetA = FindOnset(a);
    const std::size_t onsetB = FindOnset(b);
    result.onsetAMs = static_cast<double>(onsetA) * 1000.0 / sampleRate;
    result.onsetBMs = static_cast<double>(onsetB) * 1000.0 / sampleRate;

    // B starting later than A wants B moved earlier, by the difference.
    const int onsetOffset = std::clamp(static_cast<int>(onsetA) - static_cast<int>(onsetB), -maxOffset, maxOffset);
    const int around = static_cast<int>(std::lround(kSearchAroundOnsetMs * sampleRate / 1000.0));
    const int searchFrom = std::max(-maxOffset, onsetOffset - around);
    const int searchTo = std::min(maxOffset, onsetOffset + around);
    const auto at = [&](int offset) { return result.matchByOffset[static_cast<std::size_t>(offset + maxOffset)]; };
    int best = searchFrom;

    for (int offset = searchFrom; offset <= searchTo; ++offset)
    {
        if (std::abs(at(offset)) > std::abs(at(best)))
        {
            best = offset;
        }
    }

    // A parabola through the peak and its neighbours places it between samples.
    double fraction = 0.0;
    double peak = std::abs(at(best));

    if (best > -maxOffset && best < maxOffset)
    {
        const double before = std::abs(at(best - 1));
        const double after = std::abs(at(best + 1));
        const double curvature = before - 2.0 * peak + after;

        if (curvature < 0.0)
        {
            fraction = std::clamp(0.5 * (before - after) / curvature, -0.5, 0.5);
            peak -= 0.25 * (before - after) * fraction;
        }
    }

    result.valid = true;
    result.alignedOffsetMs = (static_cast<double>(best) + fraction) * 1000.0 / sampleRate;
    result.invertB = at(best) < 0.0;
    result.match = std::clamp(peak, 0.0, 1.0);
    return result;
}
} // namespace guitarfx::ir_alignment
