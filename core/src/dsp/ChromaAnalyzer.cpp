#include "dsp/ChromaAnalyzer.h"

#include "dsp/SimdFFT.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kWindowSeconds = 0.17;
// The harmonics that land on another pitch class (the 2nd and 4th are the note's own). The 9th and
// up are too weak on a guitar to matter below kMaxHz.
constexpr std::array<double, 4> kForeignHarmonics = {3.0, 5.0, 6.0, 7.0};
constexpr double kOvertoneStrength = 0.7;     // the lower peak's magnitude against this one's
constexpr double kOvertoneTolerance = 1.0204; // 35 cents: a plucked string's partials run sharp
} // namespace

ChromaAnalyzer::ChromaAnalyzer(std::size_t size)
    : mSize(size), mFft(std::make_unique<SimdFFT>(size)), mWindow(size), mInput(size), mSpectrum(size),
      mMagnitude(size / 2)
{
    mPeaks.reserve(size / 4);

    for (std::size_t i = 0; i < size; ++i)
    {
        mWindow[i] =
            static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(size)));
    }
}

ChromaAnalyzer::~ChromaAnalyzer() = default;

std::size_t ChromaAnalyzer::WindowFor(double sampleRate)
{
    const double target = std::max(1024.0, sampleRate * kWindowSeconds);
    std::size_t size = 1024;

    while (static_cast<double>(size) * 1.5 < target)
    {
        size *= 2;
    }

    return size;
}

ChromaAnalyzer::Chroma ChromaAnalyzer::Analyze(const float* samples, double sampleRate, double referenceHz)
{
    Chroma chroma{};

    if (samples == nullptr || !(sampleRate > 0.0) || !(referenceHz > 0.0))
    {
        return chroma;
    }

    for (std::size_t i = 0; i < mSize; ++i)
    {
        mInput[i] = {samples[i] * mWindow[i], 0.0f};
    }

    mFft->Forward(mSpectrum.data(), mInput.data());

    for (std::size_t k = 0; k < mMagnitude.size(); ++k)
    {
        mMagnitude[k] = std::abs(mSpectrum[k]);
    }

    const double binHz = sampleRate / static_cast<double>(mSize);
    const auto first = static_cast<std::size_t>(std::max(1.0, std::floor(kMinHz / binHz)));
    const auto last = std::min(mMagnitude.size() - 2, static_cast<std::size_t>(std::ceil(kMaxHz / binHz)));
    mPeaks.clear();

    for (std::size_t k = first; k <= last; ++k)
    {
        const float magnitude = mMagnitude[k];

        // Peaks only: the skirts of a peak are the same note leaking, not more of it.
        if (!(magnitude > mMagnitude[k - 1] && magnitude >= mMagnitude[k + 1]))
        {
            continue;
        }

        // The peak's true frequency from the parabola through it and its neighbours.
        const double left = mMagnitude[k - 1];
        const double right = mMagnitude[k + 1];
        const double denominator = left - 2.0 * magnitude + right;
        const double offset = denominator != 0.0 ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5) : 0.0;
        mPeaks.push_back({(static_cast<double>(k) + offset) * binHz, magnitude});
    }

    float total = 0.0f;

    for (std::size_t j = 0; j < mPeaks.size(); ++j)
    {
        if (IsOvertone(j))
        {
            continue;
        }

        const double semitones = 12.0 * std::log2(mPeaks[j].frequency / referenceHz);
        const double nearest = std::round(semitones);
        const double weight = 1.0 - std::min(1.0, std::abs(semitones - nearest) * 2.0);
        const int pitchClass = ((static_cast<int>(nearest) + 9) % 12 + 12) % 12; // A = 9

        chroma[static_cast<std::size_t>(pitchClass)] += static_cast<float>(mPeaks[j].magnitude * weight);
        total += static_cast<float>(mPeaks[j].magnitude * weight);
    }

    if (total > 0.0f)
    {
        for (float& value : chroma)
        {
            value /= total;
        }
    }

    return chroma;
}

bool ChromaAnalyzer::IsOvertone(std::size_t index) const
{
    const auto& peak = mPeaks[index];

    for (std::size_t i = 0; i < index; ++i)
    {
        if (mPeaks[i].magnitude < peak.magnitude * kOvertoneStrength)
        {
            continue;
        }

        for (const double harmonic : kForeignHarmonics)
        {
            const double ratio = mPeaks[i].frequency * harmonic / peak.frequency;

            if (ratio > 1.0 / kOvertoneTolerance && ratio < kOvertoneTolerance)
            {
                return true;
            }
        }
    }

    return false;
}
} // namespace guitarfx
