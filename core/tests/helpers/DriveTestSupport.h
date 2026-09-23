#pragma once

/**
 * Helpers for the drive pedals' tests (DrivePedalTests.cpp): pass/fail reporting, building a
 * pedal by model, running it in blocks, test signals (sines and a plucked-string guitar
 * phrase), and measurements (K-weighted loudness, harmonic and alias content, RMS, steps).
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/DistortionEffect.h"
#include "dsp/effects/FuzzEffect.h"
#include "dsp/effects/OverdriveEffect.h"

namespace drive_test
{
inline constexpr double kSampleRate = 48000.0;
inline constexpr int kBlockSize = 64;
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kNominalRmsDb = -18.0;

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

/// A NaN or infinity made at run time. Under -ffast-math clang assumes non-finite constants
/// away, so a quiet_NaN() literal may never reach the code under test.
inline double RuntimeDouble(std::uint64_t bits)
{
    volatile std::uint64_t pattern = bits;
    return std::bit_cast<double>(static_cast<std::uint64_t>(pattern));
}

inline float RuntimeFloat(std::uint32_t bits)
{
    volatile std::uint32_t pattern = bits;
    return std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
}

inline std::string Num(double v, int precision = 2)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

struct Pedal
{
    const char* name;
    const char* type;
    const char* alias;
    std::size_t models;
    const char* const* labels;
};

inline const std::vector<Pedal>& Pedals()
{
    static const std::vector<Pedal> pedals = {
        {"Overdrive", guitarfx::EffectGuids::kOverdrive, "overdrive", std::size(guitarfx::overdrive::kModelLabels),
         guitarfx::overdrive::kModelLabels},
        {"Distortion", guitarfx::EffectGuids::kDistortion, "distortion", std::size(guitarfx::distortion::kModelLabels),
         guitarfx::distortion::kModelLabels},
        {"Fuzz", guitarfx::EffectGuids::kFuzz, "fuzz", std::size(guitarfx::fuzz::kModelLabels),
         guitarfx::fuzz::kModelLabels},
    };
    return pedals;
}

inline bool IsBoost(const Pedal& pedal, std::size_t model)
{
    return std::string(pedal.type) == guitarfx::EffectGuids::kOverdrive &&
           model == static_cast<std::size_t>(guitarfx::overdrive::Model::Lpb1);
}

inline std::unique_ptr<guitarfx::EffectProcessor> Make(const Pedal& pedal, std::size_t model, const Params& params = {},
                                                       double sampleRate = kSampleRate)
{
    auto effect = guitarfx::EffectRegistry::Instance().Create(pedal.type);
    effect->SetParam("model", static_cast<double>(model));

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(sampleRate, kBlockSize);
    return effect;
}

/// The left output of `input` fed to both channels. The right one is only rendered when
/// `stereo` asks for it: the pedals skip a channel with no output, and each channel runs on its
/// own state, so the left is the same either way at half the cost (8x oversampled, in Debug).
inline std::vector<float> Render(guitarfx::EffectProcessor& effect, const std::vector<float>& input,
                                 bool stereo = false)
{
    std::vector<float> in(input);
    std::vector<float> outLeft(input.size());
    std::vector<float> outRight(stereo ? input.size() : 0);

    for (std::size_t start = 0; start < input.size(); start += kBlockSize)
    {
        const int count = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - start));
        float* inputs[2] = {in.data() + start, in.data() + start};
        float* outputs[2] = {outLeft.data() + start, stereo ? outRight.data() + start : nullptr};
        effect.Process(inputs, outputs, count);
    }

    return outLeft;
}

inline std::vector<float> Sine(double hz, double rmsDb, std::size_t length, double sampleRate = kSampleRate)
{
    const double amplitude = std::pow(10.0, rmsDb / 20.0) * std::sqrt(2.0);
    std::vector<float> signal(length);

    for (std::size_t i = 0; i < length; ++i)
    {
        signal[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sampleRate));
    }

    return signal;
}

/// Plucked strings (Karplus-Strong): a power chord, two single notes and an open chord, at
/// the nominal level while notes sound. The pedals' level trims were measured on this phrase.
inline std::vector<float> GuitarPhrase()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    const auto length = static_cast<std::size_t>(kSampleRate * 3.0);
    std::vector<double> mix(length, 0.0);

    const auto pluck = [&](double hz, double start, double gain, double seconds) {
        const auto period = static_cast<std::size_t>(std::lround(kSampleRate / hz));
        std::vector<double> line(period);
        double previous = 0.0;

        for (double& sample : line)
        {
            const double x = noise(rng);
            sample = 0.5 * (x + previous);
            previous = x;
        }

        const auto first = static_cast<std::size_t>(start * kSampleRate);
        const auto last = std::min(length, first + static_cast<std::size_t>(seconds * kSampleRate));
        std::size_t index = 0;

        for (std::size_t i = first; i < last; ++i)
        {
            const std::size_t next = (index + 1) % period;
            mix[i] += gain * line[index];
            line[index] = 0.4985 * (line[index] + line[next]);
            index = next;
        }
    };

    pluck(82.41, 0.0, 1.0, 1.0);
    pluck(123.47, 0.012, 1.0, 1.0);
    pluck(164.81, 0.024, 1.0, 1.0);
    pluck(329.63, 1.0, 1.0, 0.6);
    pluck(493.88, 1.6, 1.0, 0.6);
    double strum = 2.2;

    for (double hz : {82.41, 123.47, 164.81, 207.65, 246.94, 329.63})
    {
        pluck(hz, strum, 0.8, 0.8);
        strum += 0.015;
    }

    double squares = 0.0;
    std::size_t sounding = 0;

    for (double sample : mix)
    {
        if (std::fabs(sample) > 1.0e-4)
        {
            squares += sample * sample;
            ++sounding;
        }
    }

    const double gain = std::pow(10.0, kNominalRmsDb / 20.0) / std::sqrt(squares / static_cast<double>(sounding));
    std::vector<float> phrase(length);

    for (std::size_t i = 0; i < length; ++i)
    {
        phrase[i] = static_cast<float>(mix[i] * gain);
    }

    return phrase;
}

/// Loudness as a meter reads it: the BS.1770 K-weighting, then mean square, in dB.
inline double KWeightedDb(const std::vector<float>& signal)
{
    using namespace guitarfx;
    const auto shelf = biquad::HighShelf(1681.974450955533, 0.7071752369554196, 3.999843853973347, kSampleRate);
    const auto highPass = biquad::HighPass(38.13547087602444, 0.5003270373238773, kSampleRate);
    biquad::State a;
    biquad::State b;
    double squares = 0.0;

    for (float sample : signal)
    {
        const double y = b.Process(highPass, a.Process(shelf, sample));
        squares += y * y;
    }

    return 10.0 * std::log10(squares / static_cast<double>(signal.size()) + 1.0e-30);
}

inline void Fft(std::vector<std::complex<double>>& data)
{
    const std::size_t n = data.size();

    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;

        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }

        j ^= bit;

        if (i < j)
        {
            std::swap(data[i], data[j]);
        }
    }

    for (std::size_t length = 2; length <= n; length <<= 1)
    {
        const std::complex<double> step = std::polar(1.0, -2.0 * kPi / static_cast<double>(length));

        for (std::size_t i = 0; i < n; i += length)
        {
            std::complex<double> w(1.0);

            for (std::size_t k = 0; k < length / 2; ++k)
            {
                const auto u = data[i + k];
                const auto v = data[i + k + length / 2] * w;
                data[i + k] = u + v;
                data[i + k + length / 2] = u - v;
                w *= step;
            }
        }
    }
}

struct Spectrum
{
    double thdDb = 0.0; ///< harmonics 2 and up, re the fundamental
    double evenOverOddDb = 0.0;
    double aliasDb = 0.0; ///< inharmonic power below 20 kHz, re all power
};

/// Analyses the last 16384 samples of a steady tone at `hz`.
inline Spectrum Analyse(const std::vector<float>& signal, double hz)
{
    constexpr std::size_t kSize = 16384;
    std::vector<std::complex<double>> bins(kSize);
    const std::size_t offset = signal.size() - kSize;

    for (std::size_t i = 0; i < kSize; ++i)
    {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(kSize - 1);
        const double window = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        bins[i] = signal[offset + i] * window;
    }

    Fft(bins);
    const double binHz = kSampleRate / static_cast<double>(kSize);
    std::vector<bool> harmonic(kSize / 2, false);
    std::vector<double> power(64, 0.0);

    for (int k = 1; k < 64 && k * hz < kSampleRate / 2.0 - 10.0; ++k)
    {
        const auto centre = static_cast<std::size_t>(std::lround(k * hz / binHz));

        for (std::size_t b = centre > 6 ? centre - 6 : 0; b <= std::min(kSize / 2 - 1, centre + 6); ++b)
        {
            power[static_cast<std::size_t>(k)] += std::norm(bins[b]);
            harmonic[b] = true;
        }
    }

    double total = 0.0;
    double alias = 0.0;

    for (std::size_t b = 0; b < kSize / 2; ++b)
    {
        total += std::norm(bins[b]);

        if (!harmonic[b] && b > 4 && static_cast<double>(b) * binHz < 20000.0)
        {
            alias += std::norm(bins[b]);
        }
    }

    double even = 1.0e-30;
    double odd = 1.0e-30;

    for (std::size_t k = 2; k < power.size(); ++k)
    {
        (k % 2 == 0 ? even : odd) += power[k];
    }

    return {10.0 * std::log10((even + odd) / power[1]), 10.0 * std::log10(even / odd),
            10.0 * std::log10(alias / total + 1.0e-30)};
}

inline double RmsDb(const std::vector<float>& signal, std::size_t from)
{
    double squares = 0.0;

    for (std::size_t i = from; i < signal.size(); ++i)
    {
        squares += static_cast<double>(signal[i]) * signal[i];
    }

    return 10.0 * std::log10(squares / static_cast<double>(signal.size() - from) + 1.0e-30);
}

inline double LargestStep(const std::vector<float>& signal, std::size_t from, std::size_t to)
{
    double largest = 0.0;

    for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i)
    {
        largest = std::max(largest, static_cast<double>(std::fabs(signal[i] - signal[i - 1])));
    }

    return largest;
}
} // namespace drive_test
