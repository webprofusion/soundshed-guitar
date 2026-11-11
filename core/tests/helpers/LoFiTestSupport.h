#pragma once

/**
 * Measurement helpers shared by the lo-fi effects' tests (TapeSaturationEffectTests.cpp,
 * BitcrusherEffectTests.cpp): pass/fail reporting, running an effect in blocks, test signals,
 * and single-bin amplitude, RMS and peak measurements.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectProcessor.h"

namespace lofi_test
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr double kPi = 3.14159265358979323846;

using Params = std::vector<std::pair<std::string, double>>;

inline int gFailures = 0;
inline int gChecks = 0;

inline std::string Num(double v, int precision = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

inline void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

inline double ToDb(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1.0e-12));
}

template <typename Effect> void Configure(Effect& effect, const Params& params, double sampleRate = kSampleRate)
{
    effect.Prepare(sampleRate, kBlockSize);

    for (const auto& [key, value] : params)
    {
        effect.SetParam(key, value);
    }
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

inline Stereo Run(guitarfx::EffectProcessor& effect, const std::vector<float>& inL, const std::vector<float>& inR,
                  int blockSize = kBlockSize)
{
    Stereo out{std::vector<float>(inL.size()), std::vector<float>(inL.size())};

    for (std::size_t start = 0; start < inL.size(); start += static_cast<std::size_t>(blockSize))
    {
        const int count =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(blockSize), inL.size() - start));
        float* inputs[2] = {const_cast<float*>(inL.data() + start), const_cast<float*>(inR.data() + start)};
        float* outputs[2] = {out.left.data() + start, out.right.data() + start};
        effect.Process(inputs, outputs, count);
    }

    return out;
}

inline std::vector<float> RunMono(guitarfx::EffectProcessor& effect, const std::vector<float>& input,
                                  int blockSize = kBlockSize)
{
    std::vector<float> out(input.size());

    for (std::size_t start = 0; start < input.size(); start += static_cast<std::size_t>(blockSize))
    {
        const int count =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(blockSize), input.size() - start));
        effect.ProcessMono(const_cast<float*>(input.data() + start), out.data() + start, count);
    }

    return out;
}

inline std::vector<float> Sine(double frequency, double amplitude, double seconds, double sampleRate = kSampleRate)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        signal[n] =
            static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(n) / sampleRate));
    }

    return signal;
}

inline std::vector<float> Noise(double seconds, double amplitude, double sampleRate = kSampleRate,
                                std::uint32_t seed = 12345u)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (auto& sample : signal)
    {
        seed = seed * 1664525u + 1013904223u;
        sample = static_cast<float>(amplitude * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0));
    }

    return signal;
}

/// Amplitude of the component at `frequency` over the last second of `x`: with whole-hertz
/// frequencies and a one-second window every component sits exactly on a bin.
inline double Amplitude(const std::vector<float>& x, double frequency, double sampleRate = kSampleRate)
{
    const auto count = static_cast<std::size_t>(sampleRate);
    const std::size_t start = x.size() - count;
    double re = 0.0;
    double im = 0.0;

    for (std::size_t n = 0; n < count; ++n)
    {
        const double w = 2.0 * kPi * frequency * static_cast<double>(n) / sampleRate;
        re += x[start + n] * std::cos(w);
        im -= x[start + n] * std::sin(w);
    }

    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

inline double Rms(const std::vector<float>& x, std::size_t from = 0)
{
    double sum = 0.0;

    for (std::size_t n = from; n < x.size(); ++n)
    {
        sum += static_cast<double>(x[n]) * x[n];
    }

    return x.size() > from ? std::sqrt(sum / static_cast<double>(x.size() - from)) : 0.0;
}

inline double Peak(const std::vector<float>& x)
{
    double peak = 0.0;

    for (const float sample : x)
    {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }

    return peak;
}

inline double MaxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;

    for (std::size_t n = 0; n < std::min(a.size(), b.size()); ++n)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(a[n] - b[n])));
    }

    return worst;
}

inline bool AllFinite(const std::vector<float>& x)
{
    for (const float sample : x)
    {
        // Bit test: this file may be built with fast-math, which folds std::isfinite away.
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(sample));
        std::copy_n(reinterpret_cast<const unsigned char*>(&sample), sizeof(bits),
                    reinterpret_cast<unsigned char*>(&bits));

        if ((bits & 0x7f800000u) == 0x7f800000u)
        {
            return false;
        }
    }

    return true;
}
} // namespace lofi_test
