#pragma once

/**
 * Measurement helpers for the ring modulator's tests (RingModEffectTests.cpp): pass/fail
 * reporting, running an effect over a signal in blocks, test signals, and single-bin amplitude
 * and RMS measurements.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectProcessor.h"
#include "dsp/effects/RingModEffect.h"

namespace ring_mod_test
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr double kPi = 3.14159265358979323846;

using guitarfx::RingModEffect;
using Params = std::vector<std::pair<std::string, double>>;

inline int gFailures = 0;
inline int gChecks = 0;

inline void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;

    if (condition)
    {
        std::cout << "  [PASS] " << what;
    }
    else
    {
        ++gFailures;
        std::cout << "  [FAIL] " << what;
    }

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

inline std::string Num(double v, int precision = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

inline double ToDb(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1.0e-12));
}

inline std::unique_ptr<RingModEffect> MakeRing(const Params& params, double sampleRate = kSampleRate)
{
    auto ring = std::make_unique<RingModEffect>();
    ring->Prepare(sampleRate, kBlockSize);

    for (const auto& [key, value] : params)
    {
        ring->SetParam(key, value);
    }

    return ring;
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

/// Runs the two inputs through `effect` in blocks of `blockSize`.
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

inline Stereo RunMono(guitarfx::EffectProcessor& effect, const std::vector<float>& input)
{
    return Run(effect, input, input);
}

/// Sum of sines, each {frequency, amplitude}.
inline std::vector<float> Sines(const std::vector<std::pair<double, double>>& partials, double seconds,
                                double sampleRate = kSampleRate)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        double value = 0.0;

        for (const auto& [frequency, amplitude] : partials)
        {
            value += amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(n) / sampleRate);
        }

        signal[n] = static_cast<float>(value);
    }

    return signal;
}

inline std::vector<float> Noise(double seconds, double sampleRate, std::uint32_t seed = 12345u)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (auto& sample : signal)
    {
        seed = seed * 1664525u + 1013904223u;
        sample = static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
    }

    return signal;
}

/// Amplitude of the component at `frequency` over the last second of `x`. With whole-hertz
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

/// The two channels of a mono input agree to rounding, not bit for bit: under /arch:AVX2 with
/// /fp:fast the compiler may fuse the left and right channels' multiply-adds differently
/// (measured 7.6e-7 apart). Nothing relies on more.
constexpr float kChannelTolerance = 1.0e-5f;

inline float MaxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    float worst = 0.0f;

    for (std::size_t n = 0; n < std::min(a.size(), b.size()); ++n)
    {
        worst = std::max(worst, std::abs(a[n] - b[n]));
    }

    return worst;
}

inline double Rms(const std::vector<float>& x, std::size_t start, std::size_t count)
{
    double sum = 0.0;

    for (std::size_t n = start; n < start + count; ++n)
    {
        sum += static_cast<double>(x[n]) * x[n];
    }

    return std::sqrt(sum / static_cast<double>(count));
}
} // namespace ring_mod_test
