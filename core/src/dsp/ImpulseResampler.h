#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "dsp/FiniteCheck.h"

namespace guitarfx
{
/*
  Resampling an impulse response for convolution at another rate, as the IR cab and the
  convolution reverb do when the IR's rate is not the host's.

  The filter is a Blackman-windowed sinc reaching 64 source samples either side of the output
  sample, cut off at the lower of the two Nyquist frequencies. An IR's samples are FIR
  coefficients, so the result is scaled by sourceRate / targetRate: the resampled IR keeps the
  original's area (its gain to a steady signal) rather than its peak.

  That filter used to be evaluated tap by tap, a sin and two cos per tap and 129 taps per output
  sample: two seconds for a 6 s true-stereo reverb IR, on the preset-load path. It is the same
  filter now, to within rounding, computed two ways that avoid nearly all of that trig:

  - A kernel is built by angle addition. For one output sample the taps sit at distances
    frac - k, and the sinc's sin(pi c (frac - k)) and the window's two cosines are each an angle
    that moves by a fixed step from tap to tap. So each is one sin/cos per output sample
    combined with per-tap constants built once per call.
  - When both rates are whole numbers there are only L distinct kernels: with
    targetRate / sourceRate = L / M in lowest terms, output sample i sits at source position
    i * M / L exactly, whose fractional part is one of 0, 1/L, ... (L - 1)/L. Those L kernels are
    built once and every output sample is then a dot product. 44.1 -> 48 kHz needs 160 of them,
    48 -> 44.1 kHz 147 and 96 -> 48 kHz one.

  Not real-time safe: it allocates. Build IRs off the audio thread.
*/
namespace impulse_resampler_detail
{
constexpr double kPi = 3.14159265358979323846;

/// The window reaches this many source samples either side of the output sample.
constexpr int kHalfWidth = 64;

/// Taps run over k = -63..64 relative to the source sample at or before the output position,
/// which sits frac (0 <= frac < 1) past it. k = -64 would be 64 + frac away, outside the window,
/// so it is always zero and left out. k = 64 is zero only when frac is 0.
constexpr int kFirstTap = 1 - kHalfWidth;
constexpr int kTapCount = 2 * kHalfWidth;

/// A table of kernels, one per phase, is built for up to this many phases (4 MB of doubles).
/// Past that, or when a rate is not a whole number, each output sample builds its own.
constexpr std::int64_t kMaxTablePhases = 4096;

/// A rate above this is never treated as a whole number, so the conversion to an integer below is
/// always in range. No real rate comes near it.
constexpr double kMaxWholeRate = 1.0e7;

[[nodiscard]] inline double Sinc(double x)
{
    if (std::abs(x) < 1.0e-9)
    {
        return 1.0;
    }

    const double px = kPi * x;
    return std::sin(px) / px;
}

[[nodiscard]] inline double BlackmanWindow(double distance)
{
    const double normalizedDistance = std::abs(distance) / static_cast<double>(kHalfWidth);

    if (normalizedDistance >= 1.0)
    {
        return 0.0;
    }

    return 0.42 + 0.5 * std::cos(kPi * normalizedDistance) + 0.08 * std::cos(2.0 * kPi * normalizedDistance);
}

/// One tap evaluated directly: the filter's definition.
[[nodiscard]] inline double KernelTap(double distance, double cutoff)
{
    return cutoff * Sinc(distance * cutoff) * BlackmanWindow(distance);
}

/// Builds the kernel for any fractional position by angle addition.
class KernelBuilder
{
  public:
    explicit KernelBuilder(double cutoff) : mCutoff(cutoff)
    {
        for (int j = 0; j < kTapCount; ++j)
        {
            const double k = static_cast<double>(kFirstTap + j);
            const double windowAngle = kPi * k / static_cast<double>(kHalfWidth);
            mSincCos[j] = std::cos(kPi * cutoff * k);
            mSincSin[j] = std::sin(kPi * cutoff * k);
            mWindowCos[j] = std::cos(windowAngle);
            mWindowSin[j] = std::sin(windowAngle);
            mWindowCos2[j] = std::cos(2.0 * windowAngle);
            mWindowSin2[j] = std::sin(2.0 * windowAngle);
        }
    }

    /// kernel[j] = KernelTap(frac - k, cutoff) for k = kFirstTap + j, 0 <= frac < 1.
    void Build(double frac, double* kernel) const
    {
        // sin(pi c (frac - k)) = sin(a) cos(pi c k) - cos(a) sin(pi c k) with a = pi c frac, and the
        // window's cos(pi (frac - k) / 64) and cos(2 pi (frac - k) / 64) likewise. The window is
        // even in the distance, so its sign does not matter.
        const double a = kPi * mCutoff * frac;
        const double sinA = std::sin(a);
        const double cosA = std::cos(a);
        const double w = kPi * frac / static_cast<double>(kHalfWidth);
        const double sinW = std::sin(w);
        const double cosW = std::cos(w);
        const double sin2W = 2.0 * sinW * cosW;
        const double cos2W = 1.0 - 2.0 * sinW * sinW;

        const auto fill = [&](int begin, int end) {
            for (int j = begin; j < end; ++j)
            {
                const double distance = frac - static_cast<double>(kFirstTap + j);
                const double sine = sinA * mSincCos[j] - cosA * mSincSin[j];
                const double window = 0.42 + 0.5 * (cosW * mWindowCos[j] + sinW * mWindowSin[j]) +
                                      0.08 * (cos2W * mWindowCos2[j] + sin2W * mWindowSin2[j]);
                // cutoff * sinc(cutoff * d) = sin(pi cutoff d) / (pi d)
                kernel[j] = sine / (kPi * distance) * window;
            }
        };

        // The two taps either side of the output position, k = 0 and k = 1, are evaluated directly:
        // their distance can be 0 or arbitrarily small, where the division above fails or the
        // angle-addition sine, accurate only to rounding in absolute terms, loses its precision.
        constexpr int kNearest = -kFirstTap;
        fill(0, kNearest);
        kernel[kNearest] = KernelTap(frac, mCutoff);
        kernel[kNearest + 1] = KernelTap(frac - 1.0, mCutoff);
        fill(kNearest + 2, kTapCount);

        // At frac = 0 the last tap lands exactly on the window's edge, where the sum above leaves a
        // rounding residue instead of the window's zero.
        if (frac <= 0.0)
        {
            kernel[kTapCount - 1] = 0.0;
        }
    }

  private:
    double mCutoff;
    std::array<double, kTapCount> mSincCos{};
    std::array<double, kTapCount> mSincSin{};
    std::array<double, kTapCount> mWindowCos{};
    std::array<double, kTapCount> mWindowSin{};
    std::array<double, kTapCount> mWindowCos2{};
    std::array<double, kTapCount> mWindowSin2{};
};

/// sum of kernel[j] * source[j] over count taps. It keeps one running sum per lane, each over
/// every kLanes-th tap: no addition waits on the one before, and the loop is lane-wise, so a
/// compiler can vectorise it without being allowed to reorder the additions.
[[nodiscard]] inline double Dot(const double* kernel, const float* source, int count)
{
    constexpr int kLanes = 16;
    double sums[kLanes] = {};
    int j = 0;

    for (; j + kLanes <= count; j += kLanes)
    {
        for (int lane = 0; lane < kLanes; ++lane)
        {
            sums[lane] += kernel[j + lane] * static_cast<double>(source[j + lane]);
        }
    }

    double sum = 0.0;

    for (; j < count; ++j)
    {
        sum += kernel[j] * static_cast<double>(source[j]);
    }

    for (const double laneSum : sums)
    {
        sum += laneSum;
    }

    return sum;
}

/// targetRate / sourceRate as L / M in lowest terms: `phases` is L and `step` is M. Both are 0
/// unless both rates are whole numbers.
struct RationalRatio
{
    std::int64_t phases = 0;
    std::int64_t step = 0;
};

[[nodiscard]] inline RationalRatio FindRationalRatio(double sourceRate, double targetRate)
{
    if (sourceRate > kMaxWholeRate || targetRate > kMaxWholeRate || std::floor(sourceRate) != sourceRate ||
        std::floor(targetRate) != targetRate)
    {
        return {};
    }

    const auto source = static_cast<std::int64_t>(sourceRate);
    const auto target = static_cast<std::int64_t>(targetRate);
    const std::int64_t divisor = std::gcd(source, target);
    return {target / divisor, source / divisor};
}
} // namespace impulse_resampler_detail

/// Resamples `samples`, an impulse response at sourceRate, to targetRate (see above). Leaves it
/// alone when the rates are within 1 Hz of each other or either is not a finite positive number.
inline void ResampleImpulseForConvolution(std::vector<float>& samples, double sourceRate, double targetRate)
{
    using namespace impulse_resampler_detail;

    if (samples.empty() || !IsFinite(sourceRate) || !IsFinite(targetRate) || sourceRate <= 0.0 || targetRate <= 0.0 ||
        std::abs(sourceRate - targetRate) <= 1.0)
    {
        return;
    }

    const double ratio = targetRate / sourceRate;
    const double cutoff = std::min(ratio, 1.0);
    const std::size_t newSize = static_cast<std::size_t>(std::ceil(static_cast<double>(samples.size()) * ratio));

    if (newSize == 0)
    {
        samples.clear();
        return;
    }

    const KernelBuilder builder(cutoff);
    const auto sourceLength = static_cast<std::int64_t>(samples.size());
    const double coefficientScale = sourceRate / targetRate;
    std::vector<float> resampled(newSize);

    // One output sample: the kernel against the source around `center`, leaving out taps that
    // fall before the start or past the end of the IR.
    const auto convolve = [&](std::int64_t center, const double* kernel) {
        const std::int64_t first = center + kFirstTap;
        const auto begin = static_cast<int>(std::max<std::int64_t>(0, -first));
        const auto end = static_cast<int>(std::min<std::int64_t>(kTapCount, sourceLength - first));
        const double sum = Dot(kernel + begin, samples.data() + first + begin, end - begin);
        return static_cast<float>(sum * coefficientScale);
    };

    const RationalRatio rational = FindRationalRatio(sourceRate, targetRate);

    if (rational.phases > 0 && rational.phases <= kMaxTablePhases &&
        rational.phases <= static_cast<std::int64_t>(newSize))
    {
        std::vector<double> kernels(static_cast<std::size_t>(rational.phases) * kTapCount);

        for (std::int64_t phase = 0; phase < rational.phases; ++phase)
        {
            const double frac = static_cast<double>(phase) / static_cast<double>(rational.phases);
            builder.Build(frac, kernels.data() + static_cast<std::size_t>(phase) * kTapCount);
        }

        for (std::size_t outputIndex = 0; outputIndex < newSize; ++outputIndex)
        {
            // Source position outputIndex * step / phases, exactly.
            const std::int64_t position = static_cast<std::int64_t>(outputIndex) * rational.step;
            const auto phase = static_cast<std::size_t>(position % rational.phases);
            resampled[outputIndex] = convolve(position / rational.phases, kernels.data() + phase * kTapCount);
        }
    }
    else
    {
        std::array<double, kTapCount> kernel{};

        for (std::size_t outputIndex = 0; outputIndex < newSize; ++outputIndex)
        {
            const double position = static_cast<double>(outputIndex) / ratio;
            const double center = std::floor(position);
            builder.Build(position - center, kernel.data());
            resampled[outputIndex] = convolve(static_cast<std::int64_t>(center), kernel.data());
        }
    }

    samples = std::move(resampled);
}
} // namespace guitarfx
