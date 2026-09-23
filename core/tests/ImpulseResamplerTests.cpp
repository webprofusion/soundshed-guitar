/**
 * @file ImpulseResamplerTests.cpp
 * @brief The IR resampler computes its windowed sinc to within float rounding, however it gets there.
 *
 * ImpulseResampler.h builds its kernels by angle addition, and for whole-number rates only once per
 * phase, instead of evaluating a sin and two cos per tap. That is a hundred times faster, and it has
 * to be the same filter. Each case here resamples a decaying-noise IR and compares every sample with
 * the filter evaluated tap by tap at the same source position: a Blackman-windowed sinc over 64 source
 * samples either side, cut off at the lower Nyquist, scaled by sourceRate / targetRate. A sample may
 * differ by at most one float ulp of the IR's level around it. Rates that are not whole numbers, or
 * too awkward a ratio for a table, take the other path and are checked the same way.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/ImpulseResampler.h"

namespace
{
using namespace guitarfx;

int gFailures = 0;

void Report(const std::string& what, bool passed, const std::string& detail = {})
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";

    if (!passed)
    {
        ++gFailures;
    }
}

/// Non-finite inputs come from strtod: under -ffast-math a NaN or infinity constant may be assumed away.
double RuntimeDouble(const char* text)
{
    return std::strtod(text, nullptr);
}

/// Decaying noise: a unit first sample, then noise under an envelope falling `decayDb` over the IR.
std::vector<float> MakeIR(std::size_t frames, unsigned seed, double decayDb)
{
    std::vector<float> ir(frames);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    for (std::size_t i = 0; i < frames; ++i)
    {
        const double position = static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(frames, 1));
        const double envelope = std::pow(10.0, -decayDb * position / 20.0);
        ir[i] = i == 0 ? 1.0f : static_cast<float>(noise(rng) * envelope);
    }

    return ir;
}

/// The filter's definition, tap by tap. For whole-number rates the source position is the exact
/// i * sourceRate / targetRate, as the resampler's phase table has it; otherwise it is i / ratio.
std::vector<float> ResampleTapByTap(const std::vector<float>& source, double sourceRate, double targetRate)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kHalfTaps = 64;
    const double ratio = targetRate / sourceRate;
    const double cutoff = std::min(ratio, 1.0);
    const auto newSize = static_cast<std::size_t>(std::ceil(static_cast<double>(source.size()) * ratio));
    const bool wholeRates = std::floor(sourceRate) == sourceRate && std::floor(targetRate) == targetRate;
    const auto sourceWhole = static_cast<std::int64_t>(sourceRate);
    const auto targetWhole = static_cast<std::int64_t>(targetRate);
    const std::int64_t divisor = wholeRates ? std::gcd(sourceWhole, targetWhole) : 1;
    std::vector<float> out(newSize);

    for (std::size_t i = 0; i < newSize; ++i)
    {
        std::int64_t center = 0;
        double frac = 0.0;

        if (wholeRates)
        {
            const std::int64_t position = static_cast<std::int64_t>(i) * (sourceWhole / divisor);
            const std::int64_t phases = targetWhole / divisor;
            center = position / phases;
            frac = static_cast<double>(position % phases) / static_cast<double>(phases);
        }
        else
        {
            const double position = static_cast<double>(i) / ratio;
            center = static_cast<std::int64_t>(std::floor(position));
            frac = position - std::floor(position);
        }

        double sum = 0.0;

        for (int k = -kHalfTaps; k <= kHalfTaps; ++k)
        {
            const std::int64_t index = center + k;

            if (index < 0 || index >= static_cast<std::int64_t>(source.size()))
            {
                continue;
            }

            const double distance = frac - static_cast<double>(k);
            const double x = distance * cutoff;
            const double sinc = std::abs(x) < 1.0e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double normalized = std::abs(distance) / kHalfTaps;
            const double window =
                normalized >= 1.0 ? 0.0
                                  : 0.42 + 0.5 * std::cos(kPi * normalized) + 0.08 * std::cos(2.0 * kPi * normalized);
            sum += static_cast<double>(source[static_cast<std::size_t>(index)]) * cutoff * sinc * window;
        }

        out[i] = static_cast<float>(sum * sourceRate / targetRate);
    }

    return out;
}

double FloatUlp(float magnitude)
{
    const float a = std::abs(magnitude);
    return static_cast<double>(std::nextafter(a, std::numeric_limits<float>::max())) - static_cast<double>(a);
}

/// The largest difference from `expected`, in float ulps of the IR's level around each sample (its
/// largest magnitude within 64 samples), or a huge number when the lengths differ.
double WorstLocalUlps(const std::vector<float>& expected, const std::vector<float>& actual)
{
    if (expected.size() != actual.size())
    {
        return std::numeric_limits<double>::max();
    }

    double worst = 0.0;

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const double difference = std::abs(static_cast<double>(expected[i]) - static_cast<double>(actual[i]));

        if (difference == 0.0)
        {
            continue;
        }

        float level = 0.0f;

        for (std::size_t j = i >= 64 ? i - 64 : 0; j < std::min(expected.size(), i + 65); ++j)
        {
            level = std::max(level, std::abs(expected[j]));
        }

        worst = std::max(worst, difference / FloatUlp(level));
    }

    return worst;
}

std::string Describe(double sourceRate, double targetRate)
{
    std::ostringstream out;
    out << std::setprecision(10) << sourceRate << " -> " << targetRate << " Hz";
    return out.str();
}

void TestMatchesTapByTap()
{
    struct Case
    {
        double from;
        double to;
    };

    // Table path for the common pairs; then rates that are not whole numbers, and a ratio with
    // more phases than the table allows, which build each kernel as they go.
    const Case cases[] = {{44100.0, 48000.0}, {48000.0, 44100.0},  {96000.0, 48000.0},
                          {44100.0, 96000.0}, {22050.0, 48000.0},  {192000.0, 44100.0},
                          {44100.0, 48000.5}, {44100.25, 48000.0}, {44100.0, 47999.0}};

    for (const Case& c : cases)
    {
        const std::vector<float> ir = MakeIR(static_cast<std::size_t>(c.from / 8.0), 7, 90.0);
        std::vector<float> resampled = ir;
        ResampleImpulseForConvolution(resampled, c.from, c.to);
        const double worst = WorstLocalUlps(ResampleTapByTap(ir, c.from, c.to), resampled);
        std::ostringstream detail;
        detail << "worst " << worst << " ulp of the local level";
        Report("125 ms IR, " + Describe(c.from, c.to) + ", matches the filter tap by tap", worst <= 1.0, detail.str());
    }
}

void TestEveryShortLength()
{
    // Short IRs put every output sample within reach of an end, where taps are left out.
    for (const auto& [from, to] : {std::pair{44100.0, 48000.0}, std::pair{48000.0, 44100.0},
                                   std::pair{96000.0, 48000.0}, std::pair{44100.0, 48000.5}})
    {
        double worst = 0.0;

        for (std::size_t length = 1; length <= 200; ++length)
        {
            const std::vector<float> ir = MakeIR(length, static_cast<unsigned>(length), 40.0);
            std::vector<float> resampled = ir;
            ResampleImpulseForConvolution(resampled, from, to);
            worst = std::max(worst, WorstLocalUlps(ResampleTapByTap(ir, from, to), resampled));
        }

        std::ostringstream detail;
        detail << "worst " << worst << " ulp of the local level";
        Report("every IR length 1..200, " + Describe(from, to), worst <= 1.0, detail.str());
    }
}

void TestKeepsGain()
{
    // An IR's samples are FIR coefficients: resampled, a constant IR keeps its area, so each
    // sample away from the ends scales by sourceRate / targetRate.
    for (const auto& [from, to] :
         {std::pair{44100.0, 48000.0}, std::pair{48000.0, 44100.0}, std::pair{96000.0, 48000.0}})
    {
        std::vector<float> constant(4096, 1.0f);
        ResampleImpulseForConvolution(constant, from, to);
        const double expected = from / to;
        double worst = 0.0;

        for (std::size_t i = 256; i + 256 < constant.size(); ++i)
        {
            worst = std::max(worst, std::abs(static_cast<double>(constant[i]) / expected - 1.0));
        }

        std::ostringstream detail;
        detail << "worst " << worst << " off sourceRate / targetRate";
        Report("a constant IR keeps its area, " + Describe(from, to), worst < 1.0e-3, detail.str());
    }
}

void TestLeavesAlone()
{
    const std::vector<float> ir = MakeIR(480, 3, 20.0);
    const double nan = RuntimeDouble("nan");
    const double inf = RuntimeDouble("inf");
    const std::pair<double, double> cases[] = {{48000.0, 48000.5}, {48000.0, 48000.0}, {0.0, 48000.0}, {48000.0, -1.0},
                                               {nan, 48000.0},     {48000.0, nan},     {inf, 48000.0}, {48000.0, inf}};
    bool unchanged = true;

    for (const auto& [from, to] : cases)
    {
        std::vector<float> samples = ir;
        ResampleImpulseForConvolution(samples, from, to);
        unchanged &= samples == ir;
    }

    std::vector<float> empty;
    ResampleImpulseForConvolution(empty, 44100.0, 48000.0);
    Report("rates within 1 Hz, non-positive or non-finite, and an empty IR, are left alone",
           unchanged && empty.empty());
}
} // namespace

int main()
{
    std::cout << "=== ImpulseResamplerTests ===\n";

    TestMatchesTapByTap();
    TestEveryShortLength();
    TestKeepsGain();
    TestLeavesAlone();

    if (gFailures > 0)
    {
        std::cout << "\n" << gFailures << " case(s) failed\n";
        return 1;
    }

    std::cout << "\nAll cases passed\n";
    return 0;
}
